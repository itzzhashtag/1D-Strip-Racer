 /*
 * ================================================================
 *  LED RACE  —  ESP32 Edition  v4
 *
 *  FIXES in v4:
 *    • Win jingle "ding" lowered to C5 (523 Hz) — much less sharp
 *    • Inactivity timeout now watches for no button presses DURING
 *      a race; idle rainbow has no timeout of its own
 *    • Countdown sweeps from BOTH sides of the strip symmetrically
 *      (since the track is circular) and every beat lasts equally long
 *
 *  WIRING
 *  ──────
 *  LED strip DI ←──[300-500Ω]──── PIN_LED   (+ 1000µF cap on 5V)
 *  Player buttons: one leg → GPIO PIN_Px,  other → GND
 *  Passive buzzer: PIN_AUDIO → buzzer+,  buzzer– → GND
 *  TM1637 CLK  ←── PIN_TM_CLK
 *  TM1637 DIO  ←── PIN_TM_DIO
 *
 *  LIBRARIES  (Library Manager)
 *    Adafruit NeoPixel  ≥ 1.9.0
 *    TM1637Display      by Avishay Orpaz
 * ================================================================
 */

#include <Adafruit_NeoPixel.h>
#include <TM1637Display.h>
#include <esp_now.h>
#include <WiFi.h>

// ================================================================
//  ★  USER CONFIG
// ================================================================
#define NUM_PLAYERS     2
#define TOTAL_LAPS      5

#define NUM_LEDS       620
#define PIN_LED         2
#define PIN_AUDIO       4

#define PIN_P1          12
#define PIN_P2          14
// #define PIN_P3       26
// #define PIN_P4       27

#define PIN_TM_CLK      21
#define PIN_TM_DIO      22
#define TM_BRIGHTNESS   1

#define ACCEL           0.20f
#define FRICTION        0.015f
#define TICK_MS         5

#define RAINBOW_STEP    512

// Every countdown beat (3 / 2 / 1 / GO) lasts exactly this many ms
#define COUNTDOWN_BEAT_MS     1000UL

// Racing inactivity → defeat tone
#define RACING_INACTIVITY_MS  60000UL

// Player colours (0x00RRGGBB)
const uint32_t PLAYER_COLOR[4] = {
    0x00FF4500,   // P1 Orange Red
    0x00007FFF,   // P2 Azure Blue
    0x00FFFF00,   // P3 Yellow
    0x00FF00FF,   // P4 Pink
};

// ================================================================
//  ESP-NOW
// ================================================================
typedef struct { uint8_t buttons; } EspNowBtnPacket;

// ================================================================
//  STATE
// ================================================================
enum GameState { STATE_IDLE, STATE_COUNTDOWN, STATE_RACING, STATE_WIN };
GameState gameState = STATE_IDLE;

struct PlayerState {
    float   speed;
    float   dist;
    uint8_t laps;
    bool    prevBtn;
    bool    remoteBtn;
};
PlayerState players[NUM_PLAYERS];

// ================================================================
//  HARDWARE
// ================================================================
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);
TM1637Display     tmDisplay(PIN_TM_CLK, PIN_TM_DIO);

// ================================================================
//  GLOBALS
// ================================================================
int           buzzerTicks       = 0;
uint16_t      rainbowHue        = 0;
unsigned long lastButtonPressMs = 0;

// FIX 1 — display dirty flag so TM1637 I2C write never blocks physics
bool displayDirty = false;

// 7-seg helpers
static const uint8_t SEG_BLANK   = 0b00000000;
static const uint8_t SEG_DASH    = 0b01000000;
static const uint8_t SEG_DEAD[4] = { 0x5E, 0x79, 0x77, 0x5E };  // dEAd
static const uint8_t SEG_GO[4]   = { SEG_BLANK, 0x3D, 0x5C, SEG_BLANK };

// Mario jingle  —  final note is C5 (523 Hz), low resolving ding
const int WIN_NOTES[]   = { 2637,2637,0,2637,0,2093,2637,0,3136, 0,0,0,1568 };
const int WIN_NOTES_LEN = sizeof(WIN_NOTES) / sizeof(int);

const int DEFEAT_NOTES[]   = { 523, 494, 466, 440, 415, 392 };
const int DEFEAT_NOTES_LEN = sizeof(DEFEAT_NOTES) / sizeof(int);

// ================================================================
//  UTILITIES
// ================================================================

bool readButton(int i) {
    bool phys = false;
    switch (i) {
        case 0: phys = digitalRead(PIN_P1) == LOW; break;
        case 1: phys = digitalRead(PIN_P2) == LOW; break;
        // case 2: phys = digitalRead(PIN_P3) == LOW; break;
        // case 3: phys = digitalRead(PIN_P4) == LOW; break;
    }
    return phys || players[i].remoteBtn;
}

bool risingEdge(int i) {
    bool now  = readButton(i);
    bool edge = now && !players[i].prevBtn;
    players[i].prevBtn = now;
    return edge;
}

uint32_t dimColor(uint32_t c, float f) {
    return strip.Color(((c>>16)&0xFF)*f, ((c>>8)&0xFF)*f, (c&0xFF)*f);
}

void beep(int freq, int ticks) { tone(PIN_AUDIO, freq); buzzerTicks = ticks; }
void tickBuzzer() { if (buzzerTicks > 0 && --buzzerTicks == 0) noTone(PIN_AUDIO); }

// ================================================================
//  TM1637
// ================================================================
void updateDisplay() {
    uint8_t segs[4];
    if (NUM_PLAYERS == 2) {
        segs[0] = tmDisplay.encodeDigit(players[0].laps);
        segs[1] = SEG_DASH; segs[2] = SEG_DASH;
        segs[3] = tmDisplay.encodeDigit(players[1].laps);
    } else if (NUM_PLAYERS == 3) {
        segs[0] = tmDisplay.encodeDigit(players[0].laps);
        segs[1] = tmDisplay.encodeDigit(players[1].laps);
        segs[2] = tmDisplay.encodeDigit(players[2].laps);
        segs[3] = SEG_BLANK;
    } else {
        for (int i = 0; i < 4; i++) segs[i] = tmDisplay.encodeDigit(players[i].laps);
    }
    tmDisplay.setSegments(segs, 4, 0);
}

void displayIdle()  { uint8_t s[4]={SEG_DASH,SEG_DASH,SEG_DASH,SEG_DASH}; tmDisplay.setSegments(s,4,0); }

void displayCount(int n) {
    uint8_t d = tmDisplay.encodeDigit(n);
    uint8_t s[4] = { SEG_BLANK, d, d, SEG_BLANK };
    tmDisplay.setSegments(s, 4, 0);
}

void displayWinner(int w) {
    uint8_t segs[4] = {};
    uint8_t wd = (NUM_PLAYERS == 2 && w == 1) ? 3 : (uint8_t)w;
    for (int f = 0; f < 5; f++) {
        segs[wd] = tmDisplay.encodeDigit(players[w].laps);
        tmDisplay.setSegments(segs, 4, 0); delay(300);
        segs[wd] = SEG_BLANK;
        tmDisplay.setSegments(segs, 4, 0); delay(200);
    }
}

// ================================================================
//  RENDERING
// ================================================================
void drawCars() {
    strip.clear();
    bool flip = (millis() / 1000) & 1;
    for (int n = 0; n < NUM_PLAYERS; n++) {
        int i   = flip ? (NUM_PLAYERS - 1 - n) : n;
        int pos = (int)players[i].dist % NUM_LEDS;
        strip.setPixelColor(pos, PLAYER_COLOR[i]);
        strip.setPixelColor((pos - 1 + NUM_LEDS) % NUM_LEDS, dimColor(PLAYER_COLOR[i], 0.15f));
    }
}

// ================================================================
//  COUNTDOWN — FIX 3: millis()-paced, every beat is EXACTLY equal
// ================================================================

/*
 * Both sweep pixels start at the midpoint (opposite side of circle
 * to the start line) and travel toward LED 0 from both directions.
 * sweepMs is calculated from whatever time remains in the beat after
 * the display/beep overhead, so total beat = COUNTDOWN_BEAT_MS exactly.
 */
void symmetricSweep(uint32_t color, unsigned long sweepMs) {
    int  half        = NUM_LEDS / 2;
    int  delayPerLed = max(1, (int)(sweepMs / (half + 1)));

    for (int step = 0; step <= half; step++) {
        int lp = half - step;                           // left: → 0
        int rp = (half + step) % NUM_LEDS;             // right: → NUM_LEDS-1 → 0

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
    strip.clear();
    strip.show();
}

void runCountdown() {
    gameState = STATE_COUNTDOWN;

    struct Beat { int count; uint32_t color; int beepFreq; }
    beats[3] = {
        {3, 0x00FF0000, 400},
        {2, 0x00FFAA00, 600},
        {1, 0x0000FF00, 900},
    };

    strip.clear(); strip.show();
    delay(300);

    // ── 3 / 2 / 1  ───────────────────────────────────────────────
    // Each beat is millis()-anchored so overhead never adds up.
    for (int b = 0; b < 3; b++) {
        unsigned long beatStart = millis();

        displayCount(beats[b].count);          // ~5 ms I2C overhead
        tone(PIN_AUDIO, beats[b].beepFreq);    // beep fires at beat start
        delay(160);
        noTone(PIN_AUDIO);

        // Give remaining beat time to the sweep
        long elapsed  = (long)(millis() - beatStart);
        long sweepMs  = (long)COUNTDOWN_BEAT_MS - elapsed;
        if (sweepMs > 0) symmetricSweep(beats[b].color, (unsigned long)sweepMs);

        // Spin-wait to hit the exact beat boundary
        while ((long)(millis() - beatStart) < (long)COUNTDOWN_BEAT_MS);

        strip.clear(); strip.show();
    }

    // ── GO ────────────────────────────────────────────────────────
    {
        unsigned long beatStart = millis();

        tmDisplay.setSegments(SEG_GO, 4, 0);
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x00FFFFFF);
        strip.show();
        tone(PIN_AUDIO, 1800);
        delay(160);
        noTone(PIN_AUDIO);

        // Hold GO flash for the rest of the beat — same duration as 3/2/1
        while ((long)(millis() - beatStart) < (long)COUNTDOWN_BEAT_MS);

        strip.clear(); strip.show();
    }

    Serial.println("[COUNTDOWN] GO!");

    for (int i = 0; i < NUM_PLAYERS; i++) players[i] = {0,0,0,false,false};
    lastButtonPressMs = millis();
    updateDisplay();
    gameState = STATE_RACING;
}

// ================================================================
//  WIN ANIMATION — FIX 2: freeze → fade green → cross-fade to winner
// ================================================================

void crossFadeStrip(uint32_t fromColor, uint32_t toColor,
                    int steps, int stepDelayMs) {
    float fr = (fromColor >> 16) & 0xFF, fg = (fromColor >> 8) & 0xFF, fb = fromColor & 0xFF;
    float tr = (toColor   >> 16) & 0xFF, tg = (toColor   >> 8) & 0xFF, tb = toColor  & 0xFF;

    for (int s = 0; s <= steps; s++) {
        float t = (float)s / steps;
        uint8_t r = fr + (tr - fr) * t;
        uint8_t g = fg + (tg - fg) * t;
        uint8_t b = fb + (tb - fb) * t;
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(r, g, b));
        strip.show();
        delay(stepDelayMs);
    }
}

void runWinAnimation(int winner) {
    gameState = STATE_WIN;
    noTone(PIN_AUDIO);
    Serial.printf("[WIN] *** Player %d wins! ***\n", winner + 1);

    // ── Step 1: Freeze all players (speeds already 0 from caller) ──
    drawCars();
    strip.show();
    delay(400);

    // ── Step 2: Fade current (dark) state → full green ─────────────
    // We cross-fade from black to green for a clean rising effect.
    crossFadeStrip(0x00000000, 0x0000FF00, 80, 10);   // ~800 ms
    delay(250);

    // ── Step 3: Cross-fade green → winner's colour ──────────────────
    crossFadeStrip(0x0000FF00, PLAYER_COLOR[winner], 100, 12);  // ~1200 ms

    // ── Step 4: Winner digit blink on TM1637 ────────────────────────
    displayWinner(winner);

    // ── Step 5: Mario jingle ────────────────────────────────────────
    for (int n = 0; n < WIN_NOTES_LEN; n++) {
        if (WIN_NOTES[n]) tone(PIN_AUDIO, WIN_NOTES[n], 200);
        delay(230);
        noTone(PIN_AUDIO);
    }

    delay(2000);
    enterIdle();
}

// ================================================================
//  DEFEAT
// ================================================================
void playDefeatSequence() {
    noTone(PIN_AUDIO);
    tmDisplay.setSegments(SEG_DEAD, 4, 0);
    for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x00200000);
    strip.show();
    for (int n = 0; n < DEFEAT_NOTES_LEN; n++) { tone(PIN_AUDIO, DEFEAT_NOTES[n], 280); delay(310); }
    noTone(PIN_AUDIO);
    delay(600);
    strip.clear(); strip.show();
}

// ================================================================
//  IDLE
// ================================================================
void enterIdle() {
    gameState    = STATE_IDLE;
    rainbowHue   = 0;
    displayDirty = false;
    displayIdle();
    Serial.println("[IDLE] Waiting for button press…");
}

void loopIdle() {
    strip.rainbow(rainbowHue);
    strip.show();
    rainbowHue += RAINBOW_STEP;
    delay(TICK_MS);

    for (int i = 0; i < NUM_PLAYERS; i++) {
        if (risingEdge(i)) {
            Serial.printf("[IDLE] P%d pressed — starting countdown\n", i + 1);
            gameState = STATE_COUNTDOWN;
            return;
        }
    }
}

// ================================================================
//  RACING — FIX 1: display update decoupled from physics via flag
// ================================================================
void loopRacing() {

    // ── Inactivity check ─────────────────────────────────────────
    if (millis() - lastButtonPressMs >= RACING_INACTIVITY_MS) {
        Serial.println("[RACE] Inactivity timeout");
        playDefeatSequence();
        enterIdle();
        return;
    }

    // ── Physics — NO blocking calls inside here ──────────────────
    for (int i = 0; i < NUM_PLAYERS; i++) {

        if (risingEdge(i)) {
            players[i].speed  += ACCEL;
            lastButtonPressMs  = millis();
        }

        players[i].speed -= players[i].speed * FRICTION;
        if (players[i].speed < 0.0f) players[i].speed = 0.0f;
        players[i].dist += players[i].speed;

        // Lap completion — only set flag; no I2C write here
        uint32_t threshold = (uint32_t)NUM_LEDS * (players[i].laps + 1);
        if ((uint32_t)players[i].dist >= threshold) {
            players[i].laps++;
            beep(600 + i * 100, 3);   // non-blocking
            displayDirty = true;      // schedule display update for end of tick
            Serial.printf("[RACE] P%d — lap %d / %d\n", i+1, players[i].laps, TOTAL_LAPS);

            // Win check — stop all players first
            if (players[i].laps >= TOTAL_LAPS) {
                for (int j = 0; j < NUM_PLAYERS; j++) players[j].speed = 0.0f;
                runWinAnimation(i);
                return;
            }
        }
    }

    // ── Display update (once per tick, after all physics) ────────
    // TM1637 I2C write is ~10 ms — doing it here means it never
    // delays one player's physics tick relative to another's.
    if (displayDirty) {
        updateDisplay();
        displayDirty = false;
    }

    // ── Render + pace ────────────────────────────────────────────
    drawCars();
    strip.show();
    tickBuzzer();
    delay(TICK_MS);
}

// ================================================================
//  ESP-NOW CALLBACK  (core v3+)
// ================================================================
void onRemoteBtn(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len < (int)sizeof(EspNowBtnPacket)) return;
    const EspNowBtnPacket *p = (const EspNowBtnPacket *)data;
    for (int i = 0; i < NUM_PLAYERS; i++) players[i].remoteBtn = (p->buttons >> i) & 1;
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("[BOOT] LED Race v5");

    strip.begin(); strip.clear(); strip.show();
    tmDisplay.setBrightness(TM_BRIGHTNESS);
    displayIdle();

    pinMode(PIN_P1, INPUT_PULLUP);
    pinMode(PIN_P2, INPUT_PULLUP);
    // pinMode(PIN_P3, INPUT_PULLUP);
    // pinMode(PIN_P4, INPUT_PULLUP);
    pinMode(PIN_AUDIO, OUTPUT);

    WiFi.mode(WIFI_STA);
    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(onRemoteBtn);
        Serial.print("[ESP-NOW] Ready. MAC: ");
        Serial.println(WiFi.macAddress());
    } else {
        Serial.println("[ESP-NOW] Init failed — physical buttons only");
    }

    enterIdle();
}

// ================================================================
//  MAIN LOOP
// ================================================================
void loop() {
    switch (gameState) {
        case STATE_IDLE:      loopIdle();      break;
        case STATE_COUNTDOWN: runCountdown();  break;
        case STATE_RACING:    loopRacing();    break;
        case STATE_WIN:       enterIdle();     break;
    }
}


// ================================================================
//  REMOTE SENDER — copy into a separate .ino on a second ESP32
// ================================================================
/*

#include <esp_now.h>
#include <WiFi.h>

uint8_t RECEIVER_MAC[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};  // ← paste main MAC here

#define REMOTE_PIN_P1  12
#define REMOTE_PIN_P2  14

typedef struct { uint8_t buttons; } EspNowBtnPacket;
esp_now_peer_info_t peerInfo;
EspNowBtnPacket     packet;
uint8_t             lastPacket = 0xFF;

void onSent(const uint8_t *mac, esp_now_send_status_t status) {}

void setup() {
    Serial.begin(115200);
    pinMode(REMOTE_PIN_P1, INPUT_PULLUP);
    pinMode(REMOTE_PIN_P2, INPUT_PULLUP);
    WiFi.mode(WIFI_STA);
    esp_now_init();
    esp_now_register_send_cb(onSent);
    memcpy(peerInfo.peer_addr, RECEIVER_MAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
}

void loop() {
    uint8_t bits = 0;
    if (digitalRead(REMOTE_PIN_P1) == LOW) bits |= (1 << 0);
    if (digitalRead(REMOTE_PIN_P2) == LOW) bits |= (1 << 1);
    if (bits != lastPacket) {
        packet.buttons = bits;
        esp_now_send(RECEIVER_MAC, (uint8_t *)&packet, sizeof(packet));
        lastPacket = bits;
    }
    delay(10);
}

*/
