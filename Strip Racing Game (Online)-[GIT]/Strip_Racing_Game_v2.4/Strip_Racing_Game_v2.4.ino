/*
 * ================================================================
 *  LED RACE  —  ESP32 Edition  v3
 *
 *  NEW in v3:
 *    • Countdown: color sweep from strip-center → start for 3,2,1,GO
 *    • Mario win jingle with the final "ding" note added
 *    • Idle timeout (1 min) → defeat tone + "dEAd" display → idle
 *
 *  WIRING
 *  ──────
 *  LED strip DI ←──[300-500Ω]──── PIN_LED   (+ 1000µF cap on 5V)
 *  Player buttons: one leg → GPIO PIN_Px,  other → GND
 *  Passive buzzer: PIN_AUDIO → buzzer+,  buzzer– → GND
 *  TM1637 CLK  ←── PIN_TM_CLK
 *  TM1637 DIO  ←── PIN_TM_DIO
 *  TM1637 VCC  ←── 3.3 V or 5 V
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
#define NUM_PLAYERS     2         // 2 / 3 / 4
#define TOTAL_LAPS      5         // Laps to win

#define NUM_LEDS        120
#define PIN_LED         2

#define PIN_AUDIO       4

#define PIN_P1          12
#define PIN_P2          14
// #define PIN_P3       26
// #define PIN_P4       27

#define PIN_TM_CLK      21
#define PIN_TM_DIO      22
#define TM_BRIGHTNESS   4         // 0 (dim) … 7 (bright)

#define ACCEL           0.20f
#define FRICTION        0.015f
#define TICK_MS         5

#define RAINBOW_STEP    512       // Hue increment per tick (0-65535 range)
#define IDLE_TIMEOUT_MS 60000UL   // 1 minute before defeat tone

// Player colors (0x00RRGGBB)
const uint32_t PLAYER_COLOR[4] = {
    0x00FF0000,   // P1 — Red
    0x000000FF,   // P2 — Blue
    0x0000FF00,   // P3 — Green
    0x00FF00FF,   // P4 — Pink
};

// ================================================================
//  ESP-NOW PACKET
// ================================================================
typedef struct {
    uint8_t buttons;   // bit0=P1, bit1=P2, bit2=P3, bit3=P4
} EspNowBtnPacket;

// ================================================================
//  GAME STATE
// ================================================================
enum GameState { STATE_IDLE, STATE_COUNTDOWN, STATE_RACING, STATE_WIN };
GameState gameState = STATE_IDLE;

// ================================================================
//  PLAYER DATA
// ================================================================
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
//  MISC GLOBALS
// ================================================================
int           buzzerTicks   = 0;
uint16_t      rainbowHue    = 0;
unsigned long idleEnteredMs = 0;   // When we last entered idle

// 7-segment constants
static const uint8_t SEG_BLANK = 0b00000000;
static const uint8_t SEG_DASH  = 0b01000000;
// Letters for "dEAd" (defeat screen)
// d=0x5E  E=0x79  A=0x77  d=0x5E
static const uint8_t SEG_DEAD[4] = {0x5E, 0x79, 0x77, 0x5E};
// " Go " on 7-seg:  G=0x3D  o=0x5C
static const uint8_t SEG_GO[4]   = {SEG_BLANK, 0x3D, 0x5C, SEG_BLANK};

// ── Mario course-clear jingle ───────────────────────────────────
// Original: tud tu du  tud du duu  +  final DING
const int WIN_NOTES[] = {
    2637, 2637, 0, 2637, 0, 2093, 2637, 0, 3136,   // main phrase
    0, 0, 2000                                       // rest × 2, then DING (C8)
};
const int WIN_NOTES_LEN = sizeof(WIN_NOTES) / sizeof(int);

// Defeat / sad descending tone notes
const int DEFEAT_NOTES[] = { 523, 494, 466, 440, 415, 392 };
const int DEFEAT_NOTES_LEN = sizeof(DEFEAT_NOTES) / sizeof(int);

// ================================================================
//  UTILITIES
// ================================================================

bool readButton(int i) {
    bool phys = false;
    switch (i) {
        case 0: phys = (digitalRead(PIN_P1) == LOW); break;
        case 1: phys = (digitalRead(PIN_P2) == LOW); break;
        // case 2: phys = (digitalRead(PIN_P3) == LOW); break;
        // case 3: phys = (digitalRead(PIN_P4) == LOW); break;
    }
    return phys || players[i].remoteBtn;
}

/** True on the tick when a button transitions released → pressed. */
bool risingEdge(int i) {
    bool now  = readButton(i);
    bool edge = now && !players[i].prevBtn;
    players[i].prevBtn = now;
    return edge;
}

uint32_t dimColor(uint32_t color, float factor) {
    uint8_t r = ((color >> 16) & 0xFF) * factor;
    uint8_t g = ((color >>  8) & 0xFF) * factor;
    uint8_t b = ( color        & 0xFF) * factor;
    return strip.Color(r, g, b);
}

void beep(int freq, int ticks) {
    tone(PIN_AUDIO, freq);
    buzzerTicks = ticks;
}

void tickBuzzer() {
    if (buzzerTicks > 0 && --buzzerTicks == 0) noTone(PIN_AUDIO);
}

// ================================================================
//  TM1637 DISPLAY
// ================================================================

/*
 *  2P:  [P1][ — ][ — ][P2]
 *  3P:  [P1][P2][P3][ — ]
 *  4P:  [P1][P2][P3][P4]
 */
void updateDisplay() {
    uint8_t segs[4];
    if (NUM_PLAYERS == 2) {
        segs[0] = tmDisplay.encodeDigit(players[0].laps);
        segs[1] = SEG_DASH;
        segs[2] = SEG_DASH;
        segs[3] = tmDisplay.encodeDigit(players[1].laps);
    } else if (NUM_PLAYERS == 3) {
        segs[0] = tmDisplay.encodeDigit(players[0].laps);
        segs[1] = tmDisplay.encodeDigit(players[1].laps);
        segs[2] = tmDisplay.encodeDigit(players[2].laps);
        segs[3] = SEG_BLANK;
    } else {
        for (int i = 0; i < 4; i++)
            segs[i] = tmDisplay.encodeDigit(players[i].laps);
    }
    tmDisplay.setSegments(segs, 4, 0);
}

void displayIdle() {
    uint8_t segs[4] = {SEG_DASH, SEG_DASH, SEG_DASH, SEG_DASH};
    tmDisplay.setSegments(segs, 4, 0);
}

void displayWinner(int winner) {
    uint8_t segs[4]  = {SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK};
    uint8_t winDigit = (NUM_PLAYERS == 2 && winner == 1) ? 3 : winner;
    for (int f = 0; f < 5; f++) {
        segs[winDigit] = tmDisplay.encodeDigit(players[winner].laps);
        tmDisplay.setSegments(segs, 4, 0);
        delay(300);
        segs[winDigit] = SEG_BLANK;
        tmDisplay.setSegments(segs, 4, 0);
        delay(200);
    }
}

// ================================================================
//  RENDERING
// ================================================================

void drawCars() {
    strip.clear();
    bool flip = (millis() / 1000) & 1;
    for (int n = 0; n < NUM_PLAYERS; n++) {
        int i     = flip ? (NUM_PLAYERS - 1 - n) : n;
        int pos   = (int)players[i].dist % NUM_LEDS;
        int trail = (pos - 1 + NUM_LEDS) % NUM_LEDS;
        strip.setPixelColor(pos,   PLAYER_COLOR[i]);
        strip.setPixelColor(trail, dimColor(PLAYER_COLOR[i], 0.15f));
    }
}

// ================================================================
//  IDLE STATE
// ================================================================

void enterIdle() {
    gameState     = STATE_IDLE;
    rainbowHue    = 0;
    idleEnteredMs = millis();
    displayIdle();
    Serial.println("[IDLE] Waiting for button press…");
}

void loopIdle() {
    // ── 1-minute timeout → defeat tone ──────────────────────────
    if (millis() - idleEnteredMs >= IDLE_TIMEOUT_MS) {
        Serial.println("[IDLE] Timeout — playing defeat tone");

        // Show "dEAd" on display
        tmDisplay.setSegments(SEG_DEAD, 4, 0);

        // Descending sad tune
        for (int n = 0; n < DEFEAT_NOTES_LEN; n++) {
            tone(PIN_AUDIO, DEFEAT_NOTES[n], 280);
            delay(310);
        }
        noTone(PIN_AUDIO);
        delay(500);

        // Brief red flash on strip then go dark
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x00200000);
        strip.show();
        delay(600);
        strip.clear();
        strip.show();

        // Reset idle timer so it doesn't fire immediately again
        idleEnteredMs = millis();
        displayIdle();
        return;
    }

    // ── Rainbow animation ────────────────────────────────────────
    strip.rainbow(rainbowHue);
    strip.show();
    rainbowHue += RAINBOW_STEP;
    delay(TICK_MS);

    // ── Watch for any button press to start ──────────────────────
    for (int i = 0; i < NUM_PLAYERS; i++) {
        if (risingEdge(i)) {
            Serial.printf("[IDLE] P%d pressed — starting countdown\n", i + 1);
            gameState = STATE_COUNTDOWN;
            return;
        }
    }
}

// ================================================================
//  COUNTDOWN — sweep from center → start for each of 3 / 2 / 1 / GO
// ================================================================

/*
 * For each count (3, 2, 1) a lit pixel sweeps from the midpoint of
 * the strip toward LED 0 (the start/finish line), with a short tail.
 * The sweep gets faster each beat.  After GO a white flash fires.
 *
 * delayPerLed:  3 = 15 ms  →  2 = 8 ms  →  1 = 3 ms  →  GO = instant
 */
void sweepToStart(uint32_t color, int delayPerLed) {
    int mid = NUM_LEDS / 2;

    for (int pos = mid; pos >= 0; pos--) {
        strip.clear();
        strip.setPixelColor(pos,     color);
        if (pos + 1 <= mid) strip.setPixelColor(pos + 1, dimColor(color, 0.35f));
        if (pos + 2 <= mid) strip.setPixelColor(pos + 2, dimColor(color, 0.12f));
        strip.show();
        delay(delayPerLed);
    }
    strip.clear();
    strip.show();
}

/** Show a single count digit centred on the TM1637 (e.g.  " 3 "). */
void displayCount(int count) {
    uint8_t d = tmDisplay.encodeDigit(count);
    uint8_t segs[4] = {SEG_BLANK, d, d, SEG_BLANK};
    tmDisplay.setSegments(segs, 4, 0);
}

void runCountdown() {
    gameState = STATE_COUNTDOWN;

    struct Beat {
        int      count;         // number to show (0 = GO)
        uint32_t color;         // sweep color
        int      sweepDelay;    // ms per LED  (0 = instant flash)
        int      beepFreq;
    } beats[4] = {
        {3, 0x00FF0000, 14, 400 },   // Red   — slow
        {2, 0x00FFAA00, 7,  600 },   // Amber — medium
        {1, 0x0000FF00, 3,  900 },   // Green — fast
        {0, 0x00FFFFFF, 0,  1800},   // White — GO (instant flash)
    };

    strip.clear();
    strip.show();
    delay(300);

    for (int b = 0; b < 4; b++) {

        if (beats[b].count > 0) {
            // Show count on TM1637
            displayCount(beats[b].count);

            // Sweep from centre to start
            sweepToStart(beats[b].color, beats[b].sweepDelay);

            // Beep after sweep
            tone(PIN_AUDIO, beats[b].beepFreq, 180);
            delay(200);
            noTone(PIN_AUDIO);

        } else {
            // "GO" — flash full strip white + high beep
            tmDisplay.setSegments(SEG_GO, 4, 0);

            for (int i = 0; i < NUM_LEDS; i++)
                strip.setPixelColor(i, 0x00FFFFFF);
            strip.show();

            tone(PIN_AUDIO, beats[b].beepFreq, 180);
            delay(200);
            noTone(PIN_AUDIO);

            strip.clear();
            strip.show();
        }
    }

    Serial.println("[COUNTDOWN] GO!");

    // Zero all player state and switch to racing
    for (int i = 0; i < NUM_PLAYERS; i++) {
        players[i] = {0.0f, 0.0f, 0, false, false};
    }
    updateDisplay();
    gameState = STATE_RACING;
}

// ================================================================
//  WIN ANIMATION
// ================================================================

void runWinAnimation(int winner) {
    gameState = STATE_WIN;
    noTone(PIN_AUDIO);
    Serial.printf("[WIN] *** Player %d wins! ***\n", winner + 1);

    // Blink winner's digit on TM1637
    displayWinner(winner);

    // Flash strip green ×3
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x0000FF00);
        strip.show(); delay(300);
        strip.clear();
        strip.show(); delay(200);
    }

    // Hold winner's color
    for (int i = 0; i < NUM_LEDS; i++)
        strip.setPixelColor(i, PLAYER_COLOR[winner]);
    strip.show();

    // Mario course-clear jingle  (tud tu du  tud du duu … DING)
    for (int n = 0; n < WIN_NOTES_LEN; n++) {
        if (WIN_NOTES[n]) tone(PIN_AUDIO, WIN_NOTES[n], 200);
        delay(230);
        noTone(PIN_AUDIO);
    }

    delay(2000);
    enterIdle();
}

// ================================================================
//  RACING LOOP
// ================================================================

void loopRacing() {
    for (int i = 0; i < NUM_PLAYERS; i++) {

        if (risingEdge(i)) players[i].speed += ACCEL;

        players[i].speed -= players[i].speed * FRICTION;
        if (players[i].speed < 0.0f) players[i].speed = 0.0f;

        players[i].dist += players[i].speed;

        // Lap completion
        uint32_t threshold = (uint32_t)NUM_LEDS * (players[i].laps + 1);
        if ((uint32_t)players[i].dist >= threshold) {
            players[i].laps++;
            beep(600 + i * 100, 3);
            updateDisplay();
            Serial.printf("[RACE] P%d — lap %d / %d\n",
                          i + 1, players[i].laps, TOTAL_LAPS);

            if (players[i].laps >= TOTAL_LAPS) {
                drawCars();
                strip.show();
                runWinAnimation(i);
                return;
            }
        }
    }

    drawCars();
    strip.show();
    tickBuzzer();
    delay(TICK_MS);
}

// ================================================================
//  ESP-NOW CALLBACK  (Arduino core v3+)
//  On core v2 change first arg to:  const uint8_t *mac
// ================================================================
void onRemoteBtn(const esp_now_recv_info_t *info,
                 const uint8_t             *data,
                 int                        len) {
    if (len < (int)sizeof(EspNowBtnPacket)) return;
    const EspNowBtnPacket *pkt = (const EspNowBtnPacket *)data;
    for (int i = 0; i < NUM_PLAYERS; i++)
        players[i].remoteBtn = (pkt->buttons >> i) & 1;
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("[BOOT] LED Race v3");

    strip.begin();
    strip.clear();
    strip.show();

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
        case STATE_WIN:       enterIdle();     break;  // safety fallback
    }
}


// ================================================================
//  REMOTE SENDER SKETCH — copy into a separate .ino on second ESP32
// ================================================================
/*

#include <esp_now.h>
#include <WiFi.h>

// Paste MAC address printed by main ESP32 on boot:
uint8_t RECEIVER_MAC[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

#define REMOTE_PIN_P1   12
#define REMOTE_PIN_P2   14

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
