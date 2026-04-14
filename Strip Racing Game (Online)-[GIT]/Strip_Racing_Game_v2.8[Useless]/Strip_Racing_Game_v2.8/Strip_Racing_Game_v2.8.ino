/*
 * ================================================================
 *  LED RACE  —  ESP32 Edition  v8
 *
 *  DISPLAY: Delegated entirely to an Arduino Uno running
 *           led_race_UNO_display.ino via SoftwareSerial.
 *
 *  ESP32 sends:  "P<pos><char>\n"  for each digit position.
 *  Example: dispOver() sends P1O, P2v, P3E, P4r  → "OvEr"
 *
 *  WIRING
 *  ──────
 *  LED strip DI ←──[300-500Ω]──── PIN_LED   (+1000µF cap on 5V)
 *  Player buttons: one leg → GPIO PIN_Px, other → GND
 *  Passive buzzer: PIN_AUDIO → buzzer+, buzzer– → GND
 *  ESP32 GPIO 33 (TX) ────────────────────→ Uno A0 (SoftSerial RX)
 *  Common GND between ESP32 and Uno
 *
 *  ALWAYS 4-PLAYER:
 *  All 4 digit slots are always active regardless of how many
 *  physical buttons are actually pressed. Unplayed lanes just
 *  show '0' and never advance.
 *
 *  DISPLAY PATTERNS:
 *    Idle    "----"   Rainbow on strip, waiting for any press
 *    Racing  "0123"   Live lap count for each of the 4 players
 *    Win     "OvEr"   + strip fade to winner colour + jingle
 *    Defeat  "dEAd"   + dim red strip + sad tune → idle
 *    Count   " 3 3"   Symmetric countdown 3 / 2 / 1 / Go
 * ================================================================
 */

//ESP1
#include <Adafruit_NeoPixel.h>
#include <esp_now.h>
#include <WiFi.h>

// 0 = no Serial output (cleaner timing, recommended)
// 1 = enable Serial debug log
#define DEBUG 1
#if DEBUG
  #define DBG(x) Serial.println(x)
  #define DBGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGF(...)
#endif
// ================================================================
//  ★  USER CONFIG
// ================================================================
#define TOTAL_LAPS            5       // Laps required to win
#define NUM_LEDS              120
#define PIN_LED               23      // SAFE (best for NeoPixel/WS2812)
#define PIN_AUDIO             25      // SAFE analog/PWM friendly

// Physical player button pins (button → pin + GND, INPUT_PULLUP)
#define PIN_P1                16
#define PIN_P2                17
#define PIN_P3                5       // still OK IF only button + pullup
#define PIN_P4                18


// ESP32 → Uno serial link
// Connect UNO_TX_PIN to Uno A0 (SoftwareSerial RX)
#define UNO_TX_PIN            33
#define UNO_RX_PIN            34     // Input-only pin, tied to nothing
#define UNO_BAUD              9600   // Must match Uno sketch BAUD_RATE

#define ACCEL                 0.20f
#define FRICTION              0.015f
#define TICK_MS               5

#define RAINBOW_STEP          512
#define COUNTDOWN_BEAT_MS     1000UL
#define RACING_INACTIVITY_MS  60000UL   // 1 minute no button → defeat

// Player colours (0x00RRGGBB)
const uint32_t PLAYER_COLOR[4] = 
{
    0x00FF4500,   // P1 Orange-Red
    0x00007FFF,   // P2 Azure Blue
    0x00FFFF00,   // P3 Yellow
    0x00FF00FF,   // P4 Pink
};

// ================================================================
//  ESP-NOW  (remote buttons from a second ESP32)
// ================================================================
typedef struct { uint8_t buttons; } EspNowBtnPacket;

// ================================================================
//  GAME STATE
// ================================================================
enum GameState { STATE_IDLE, STATE_COUNTDOWN, STATE_RACING, STATE_WIN };
GameState gameState = STATE_IDLE;

// ================================================================
//  PLAYER STATE  (always 4 slots — unused lanes just stay at 0)
// ================================================================
struct PlayerState 
{
    float   speed;
    float   dist;
    uint8_t laps;
    bool    prevBtn;
    bool    remoteBtn;
};
PlayerState players[4];   // Always 4, hard-coded

// ================================================================
//  HARDWARE
// ================================================================
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);
HardwareSerial    unoSerial(2);   // UART2 — custom-mapped to UNO pins

// ================================================================
//  GLOBALS
// ================================================================
int           buzzerTicks       = 0;
uint16_t      rainbowHue        = 0;
unsigned long lastButtonPressMs = 0;
unsigned long lastDisplayMs     = 0;

// Tunes
const int WIN_NOTES[]   = { 2637,2637,0,2637,0,2093,2637,0,3136, 0,0,0,1568 };
const int WIN_NOTES_LEN = sizeof(WIN_NOTES) / sizeof(int);

const int DEFEAT_NOTES[]   = { 523, 494, 466, 440, 415, 392 };
const int DEFEAT_NOTES_LEN = sizeof(DEFEAT_NOTES) / sizeof(int);

// ================================================================
//  DISPLAY — sends P<pos><char>\n commands to the Uno
// ================================================================

/**
 * Send one digit update to the Uno display.
 * pos : 1-4 (left to right)
 * ch  : any character the SevSeg library understands
 *       ('0'-'9', 'A'-'Z', 'a'-'z', '-', ' ')
 *
 * The Uno's SoftwareSerial receives and parses "P<pos><char>\n".
 * This call is non-blocking (UART hardware buffer, 9600 baud).
 */
void sendChar(uint8_t pos, char ch) 
{
    char buf[5];
    snprintf(buf, sizeof(buf), "P%d%c\n", pos, ch);
    unoSerial.print(buf);   // actual UNO transmission
    #if DEBUG
    Serial.print("[UNO TX] ");
    Serial.print(buf);
    #endif
}

/**
 * Set all four digits at once.
 * Sends 4 individual sendChar() calls — ~1.6 ms total at 9600 baud.
 */
void sendDisplay(char d1, char d2, char d3, char d4) 
{

    #if DEBUG
    Serial.printf("[DISPLAY] %c %c %c %c\n", d1, d2, d3, d4);
    #endif

    sendChar(1, d1);
    sendChar(2, d2);
    sendChar(3, d3);
    sendChar(4, d4);
}

// ── Named display states ──────────────────────────────────────────

/** "----"  idle / waiting for players */
void dispIdle() 
{
    sendDisplay('0', '-', '-', '0');
}

/** " N N "  for countdown beats (3, 2, 1) */
void dispCount(int n) 
{
    char d = '0' + n;
    sendDisplay(' ', d, d, ' ');
}

/** " Go "  for the GO beat */
void dispGo() 
{
    sendDisplay(' ', 'G', 'o', ' ');
}

/** Live lap counts for all 4 players: "0000" → "1234" etc. */
void dispLaps() 
{
    sendDisplay(
        '0' + players[0].laps,
        '0' + players[1].laps,
        '0' + players[2].laps,
        '0' + players[3].laps
    );
}

/** "OvEr"  shown when someone wins */
void dispOver() 
{
    sendDisplay('O', 'v', 'E', 'r');
}

/** "dEAd"  shown on inactivity defeat */
void dispDead() 
{
    sendDisplay('d', 'E', 'A', 'd');
}

// ================================================================
//  UTILITIES
// ================================================================

bool readButton(int i) 
{
    bool phys = false;
    switch (i) {
        case 0: phys = digitalRead(PIN_P1) == LOW; break;
        case 1: phys = digitalRead(PIN_P2) == LOW; break;
        case 2: phys = digitalRead(PIN_P3) == LOW; break;
        case 3: phys = digitalRead(PIN_P4) == LOW; break;
    }
    return phys || players[i].remoteBtn;
}

/** Returns true only on the tick a button transitions LOW for the first time. */
bool risingEdge(int i) 
{
    bool now  = readButton(i);
    bool edge = now && !players[i].prevBtn;
    players[i].prevBtn = now;
    return edge;
}

uint32_t dimColor(uint32_t c, float f) 
{
    return strip.Color(((c>>16)&0xFF)*f, ((c>>8)&0xFF)*f, (c&0xFF)*f);
}

/** Non-blocking beep: sets duration in loop ticks (1 tick ≈ TICK_MS ms). */
void beep(int freq, int ticks) { tone(PIN_AUDIO, freq); buzzerTicks = ticks; }
void tickBuzzer() { if (buzzerTicks > 0 && --buzzerTicks == 0) noTone(PIN_AUDIO); }

// ================================================================
//  RENDERING
// ================================================================

void drawCars() 
{
    strip.clear();
    // Alternate draw order every second so neither player is always on top
    bool flip = (millis() / 1000) & 1;
    for (int n = 0; n < 4; n++) {
        int i   = flip ? (3 - n) : n;
        int pos = (int)players[i].dist % NUM_LEDS;
        strip.setPixelColor(pos, PLAYER_COLOR[i]);
        strip.setPixelColor((pos - 1 + NUM_LEDS) % NUM_LEDS, dimColor(PLAYER_COLOR[i], 0.15f));
    }
}

// ================================================================
//  COUNTDOWN  — symmetric sweep from mid-strip, millis()-paced
// ================================================================

/**
 * Shoot two pixels from the strip midpoint toward LED 0 (start line)
 * from both sides simultaneously — correct for a circular track.
 * sweepMs is whatever time budget remains in the current beat.
 */
void symmetricSweep(uint32_t color, unsigned long sweepMs) 
{
    int half        = NUM_LEDS / 2;
    int delayPerLed = max(1, (int)(sweepMs / (half + 1)));

    for (int step = 0; step <= half; step++) 
    {
        int lp = half - step;                          // left pixel  → LED 0
        int rp = (half + step) % NUM_LEDS;            // right pixel → LED 0 (wraps)
        strip.clear();
        strip.setPixelColor(lp, color);
        strip.setPixelColor(rp, color);
        if (lp + 1 <= half) strip.setPixelColor(lp + 1, dimColor(color, 0.35f));
        if (lp + 2 <= half) strip.setPixelColor(lp + 2, dimColor(color, 0.12f));
        strip.setPixelColor((rp - 1 + NUM_LEDS) % NUM_LEDS, dimColor(color, 0.35f));
        strip.setPixelColor((rp - 2 + NUM_LEDS) % NUM_LEDS, dimColor(color, 0.12f));
        strip.show();
        delay(delayPerLed);
    }
    strip.clear(); strip.show();
}

void runCountdown() 
{
    gameState = STATE_COUNTDOWN;

    struct Beat { int count; uint32_t color; int beepFreq; }
    beats[3] = {
        {3, 0x00FF0000, 400},   // Red
        {2, 0x00FFAA00, 600},   // Amber
        {1, 0x0000FF00, 900},   // Green
    };

    strip.clear(); strip.show();
    delay(300);

    // ── 3 / 2 / 1 — millis()-anchored so every beat is exactly equal ──
    for (int b = 0; b < 3; b++) 
    {
        unsigned long beatStart = millis();
        dispCount(beats[b].count);
        tone(PIN_AUDIO, beats[b].beepFreq); delay(160); noTone(PIN_AUDIO);
        long sweepMs = (long)COUNTDOWN_BEAT_MS - (long)(millis() - beatStart);
        if (sweepMs > 0) symmetricSweep(beats[b].color, (unsigned long)sweepMs);
        while ((long)(millis() - beatStart) < (long)COUNTDOWN_BEAT_MS);
        strip.clear(); strip.show();
    }

    // ── GO — same beat duration, full white flash ─────────────────────
    {
        unsigned long beatStart = millis();
        dispGo();
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x00FFFFFF);
        strip.show();
        tone(PIN_AUDIO, 1800); delay(160); noTone(PIN_AUDIO);
        while ((long)(millis() - beatStart) < (long)COUNTDOWN_BEAT_MS);
        strip.clear(); strip.show();
    }

    // Reset all 4 player slots and start race
    for (int i = 0; i < 4; i++) players[i] = {0, 0, 0, false, false};
    lastButtonPressMs = millis();
    lastDisplayMs     = millis();
    dispLaps();   // Show "0000" at race start
    gameState = STATE_RACING;
}

// ================================================================
//  WIN ANIMATION  — freeze → fade green → cross-fade to winner
// ================================================================

void crossFadeStrip(uint32_t from, uint32_t to, int steps, int stepMs) 
{
    float fr=(from>>16)&0xFF, fg=(from>>8)&0xFF, fb=from&0xFF;
    float tr=(to>>16)  &0xFF, tg=(to>>8)  &0xFF, tb=to  &0xFF;
    for (int s = 0; s <= steps; s++) {
        float t = (float)s / steps;
        for (int i = 0; i < NUM_LEDS; i++)
            strip.setPixelColor(i, strip.Color(
                fr+(tr-fr)*t, fg+(tg-fg)*t, fb+(tb-fb)*t));
        strip.show();
        delay(stepMs);
    }
}

void runWinAnimation(int winner) 
{
    gameState = STATE_WIN;
    noTone(PIN_AUDIO);

    drawCars(); strip.show(); delay(400);             // Freeze frame
    crossFadeStrip(0x00000000, 0x0000FF00, 80, 10);  // Fade → green (~800 ms)
    delay(250);
    crossFadeStrip(0x0000FF00, PLAYER_COLOR[winner], 100, 12); // Fade → winner (~1200 ms)

    dispOver();   // Show "OvEr" on display

    // Mario course-clear jingle
    for (int n = 0; n < WIN_NOTES_LEN; n++) 
    {
        if (WIN_NOTES[n]) tone(PIN_AUDIO, WIN_NOTES[n], 200);
        delay(230);
        noTone(PIN_AUDIO);
    }

    delay(2000);
    enterIdle();
}

// ================================================================
//  DEFEAT  — 1-minute racing inactivity timeout
// ================================================================

void playDefeatSequence() 
{
    noTone(PIN_AUDIO);
    dispDead();   // Show "dEAd"
    for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x00200000);
    strip.show();
    for (int n = 0; n < DEFEAT_NOTES_LEN; n++) {
        tone(PIN_AUDIO, DEFEAT_NOTES[n], 280); delay(310);
    }
    noTone(PIN_AUDIO);
    delay(600);
    strip.clear(); strip.show();
}

// ================================================================
//  IDLE  — rainbow until any button pressed
// ================================================================

void enterIdle() 
{
    gameState  = STATE_IDLE;
    rainbowHue = 0;
    dispIdle();   // Show "----"
    #if DEBUG
    Serial.println("[IDLE] Waiting…");
    #endif
}

void loopIdle() 
{
    // Rainbow animation — no timeout; defeat only fires during a race
    strip.rainbow(rainbowHue);
    strip.show();
    rainbowHue += RAINBOW_STEP;
    delay(TICK_MS);

    // Any button press → start countdown
    for (int i = 0; i < 4; i++) {
        if (risingEdge(i)) {
            gameState = STATE_COUNTDOWN;
            return;
        }
    }
}

// ================================================================
//  RACING LOOP
// ================================================================

void loopRacing() 
{

    // ── 1-minute inactivity check ────────────────────────────────
    if (millis() - lastButtonPressMs >= RACING_INACTIVITY_MS) 
    {
        playDefeatSequence();
        enterIdle();
        return;
    }

    // ── Physics — no blocking calls inside here ──────────────────
    for (int i = 0; i < 4; i++) 
    {

        if (risingEdge(i)) {
            players[i].speed  += ACCEL;
            lastButtonPressMs  = millis();   // Reset inactivity clock
        }

        players[i].speed -= players[i].speed * FRICTION;
        if (players[i].speed < 0.0f) players[i].speed = 0.0f;
        players[i].dist += players[i].speed;

        // ── Lap completion check ──────────────────────────────────
        uint32_t threshold = (uint32_t)NUM_LEDS * (players[i].laps + 1);
        if ((uint32_t)players[i].dist >= threshold) 
        {
            players[i].laps++;
            beep(600 + i * 100, 3);   // Non-blocking beep, distinct per player

            #if DEBUG
            Serial.printf("[RACE] P%d lap %d/%d\n", i+1, players[i].laps, TOTAL_LAPS);
            #endif

            // ── Win check ─────────────────────────────────────────
            if (players[i].laps >= TOTAL_LAPS) 
            {
                for (int j = 0; j < 4; j++) players[j].speed = 0.0f;
                runWinAnimation(i);
                return;
            }
        }
    }

    // ── Display refresh on its own 120 ms timer ──────────────────
    // Serial writes to Uno take ~1.6 ms; doing this at 120 ms intervals
    // means display overhead is < 2% of loop time.
    if (millis() - lastDisplayMs >= 120UL) 
    {
        dispLaps();
        lastDisplayMs = millis();
    }

    // ── Render + pace ─────────────────────────────────────────────
    drawCars();
    strip.show();
    tickBuzzer();
    delay(TICK_MS);
}

// ================================================================
//  ESP-NOW CALLBACK  (core 3.x)
//  On core 2.x change first arg to:  const uint8_t *mac
// ================================================================
void onRemoteBtn(const esp_now_recv_info_t *info, const uint8_t *data, int len) 
{
    if (len < (int)sizeof(EspNowBtnPacket)) return;
    const EspNowBtnPacket *p = (const EspNowBtnPacket *)data;
    for (int i = 0; i < 4; i++) players[i].remoteBtn = (p->buttons >> i) & 1;
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
    #if DEBUG
    Serial.begin(115200);
    Serial.println("[BOOT] LED Race v8");
    #endif

    // LED strip
    strip.begin(); strip.clear(); strip.show();

    // Serial link to Uno display — UART2 remapped to custom pins
    // Connect UNO_TX_PIN (GPIO 33) → Uno A0
    unoSerial.begin(UNO_BAUD, SERIAL_8N1, UNO_RX_PIN, UNO_TX_PIN);
    delay(100);   // Let Uno boot and open SoftwareSerial first
    dispIdle();

    // Player buttons
    pinMode(PIN_P1, INPUT_PULLUP);
    pinMode(PIN_P2, INPUT_PULLUP);
    pinMode(PIN_P3, INPUT_PULLUP);
    pinMode(PIN_P4, INPUT_PULLUP);

    // Buzzer
    pinMode(PIN_AUDIO, OUTPUT);

    // WiFi + ESP-NOW
    WiFi.mode(WIFI_STA);
    if (esp_now_init() == ESP_OK) 
    {
        WiFi.setSleep(false);   // Prevent ~500 ms modem-sleep stalls during race
        esp_now_register_recv_cb(onRemoteBtn);
        #if DEBUG
        Serial.print("[ESP-NOW] MAC: "); Serial.println(WiFi.macAddress());
        #endif
    }

    enterIdle();
}

// ================================================================
//  MAIN LOOP
// ================================================================
void loop() 
{
    switch (gameState) 
    {
        case STATE_IDLE:      loopIdle();      break;
        case STATE_COUNTDOWN: runCountdown();  break;
        case STATE_RACING:    loopRacing();    break;
        case STATE_WIN:       enterIdle();     break;
    }
}