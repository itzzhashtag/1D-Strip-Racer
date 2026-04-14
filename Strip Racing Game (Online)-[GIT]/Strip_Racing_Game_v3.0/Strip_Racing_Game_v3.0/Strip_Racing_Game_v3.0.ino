/*
 * ================================================================
 *  ESP1_LED_Race.ino  —  Main / Track Unit
 *  4-Player WS2812B LED Strip Racing Game  ·  ESP32 Edition
 * ================================================================
 *
 *  WHAT THIS UNIT DOES
 *  ────────────────────
 *  · Runs ALL game logic (physics, laps, win detection)
 *  · Drives the WS2812B LED strip (the "track")
 *  · Shows all 4 players' lap counts on a single TM1637 display
 *  · Reads 4 local push-buttons (each with an LED inside)
 *  · Plays buzzer tones for lap / countdown / win / defeat
 *  · Sends game-state updates to ESP2 via ESP-NOW
 *  · Receives remote button-press events from ESP2 via ESP-NOW
 *
 *  HARDWARE WIRING
 *  ────────────────
 *  WS2812B strip data       → GPIO 5
 *  Passive buzzer           → GPIO 15
 *  TM1637 CLK               → GPIO 18
 *  TM1637 DIO               → GPIO 19
 *  Player 1  button         → GPIO 32   (INPUT_PULLUP)
 *  Player 1  button LED     → GPIO 25   (PWM out)
 *  Player 2  button         → GPIO 33
 *  Player 2  button LED     → GPIO 26
 *  Player 3  button         → GPIO 4
 *  Player 3  button LED     → GPIO 27
 *  Player 4  button         → GPIO 13
 *  Player 4  button LED     → GPIO 14
 *
 *  ESP-NOW SETUP
 *  ─────────────
 *  Both units use the Wi-Fi broadcast address (FF:FF:FF:FF:FF:FF)
 *  so no MAC address pairing is required.  They must be on the
 *  same Wi-Fi channel (channel 0 = driver default).
 *
 *  DISPLAY FORMAT (TM1637, 4 digits)
 *  ────────────────────────────────
 *  [ P1 laps ][ P2 laps ][ P3 laps ][ P4 laps ]
 *  Example when laps = 3,0,2,1  →  shows  3 0 2 1
 *
 *  BUTTON LED BEHAVIOUR
 *  ─────────────────────
 *  IDLE      : all 4 LEDs breathe (sine-wave pulse) in sync
 *  COUNTDOWN : all 4 LEDs ramp to full brightness
 *  RACING    : all 4 LEDs stay at full brightness
 *  WIN       : all 4 LEDs fade OUT → winner LED fades IN → idle
 *
 *  LIBRARIES REQUIRED  (install via Arduino Library Manager)
 *  ───────────────────
 *  · Adafruit NeoPixel
 *  · TM1637Display  (by Avishay Orpaz)
 *  · ESP32 board support (espressif/arduino-esp32 v3.x)
 * ================================================================
 */

// ────────────────────────────────────────────────────────────────
//  LIBRARIES
// ────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <TM1637Display.h>
#include <WiFi.h>
#include <esp_now.h>
#include <math.h>

// ────────────────────────────────────────────────────────────────
//  ★ SETTINGS  — edit the values below to customise your game
// ────────────────────────────────────────────────────────────────

// ── Track ────────────────────────────────────────────────────────
#define NUM_LEDS        655    // ★ total LEDs in your strip
#define LED_STARTLINE   0      // ★ index of the start / finish LED

// ── Game rules ───────────────────────────────────────────────────
#define NUM_PLAYERS     4      // fixed at 4 for this build
#define TOTAL_LAPS      3      // ★ laps needed to win

// ── Pins ─────────────────────────────────────────────────────────
#define PIN_LED         5      // ★ NeoPixel data pin
#define PIN_AUDIO       15     // ★ passive buzzer pin

// Button pin and button-LED pin for each player (index 0-3)
const uint8_t BTN_PIN    [NUM_PLAYERS] = { 32, 33,  4, 13 }; // ★
const uint8_t BTN_LED_PIN[NUM_PLAYERS] = { 25, 26, 27, 14 }; // ★

// TM1637 4-digit display
#define PIN_TM_CLK      18     // ★
#define PIN_TM_DIO      19     // ★
#define TM_BRIGHTNESS    7     // ★ 0 (dim) … 7 (bright)

// ── Strip brightness ─────────────────────────────────────────────
#define SR_BRIGHTNESS   150    // ★ 0-255

// ── Physics ──────────────────────────────────────────────────────
#define ACCEL           0.20f  // ★ speed added per button press
#define FRICTION        0.015f // ★ 0.01 = slippery | 0.05 = sticky
#define TICK_MS         5      // ★ ms per physics / render frame

// ── Countdown ────────────────────────────────────────────────────
#define COUNTDOWN_BEAT_MS       1000UL // ★ ms per 3-2-1-GO beat
#define COUNTDOWN_SWEEP_FRAMES  35     // ★ sweep animation steps (20-50)

// ── Idle rainbow ─────────────────────────────────────────────────
#define RAINBOW_STEP    512    // ★ larger = faster hue cycling

// ── Inactivity timeout ───────────────────────────────────────────
#define RACING_INACTIVITY_MS  60000UL  // ★ ms with no press → abort

// ── Start-line width ─────────────────────────────────────────────
#define STARTLINE_WIDTH  3     // ★ 1=single pixel, 3=centre+sides

// ── Button LED breathing (idle) ──────────────────────────────────
#define BREATHE_PERIOD_MS  2000UL  // ★ ms for one full breath cycle
#define BTN_LED_STEP       3       // ★ PWM change per tick (1=slow,10=fast)
#define BTN_LED_MIN        20      // ★ darkest point of breath (0-255)

// ── Player colours  (0x00RRGGBB) ─────────────────────────────────
const uint32_t PLAYER_COLOR[NUM_PLAYERS] = {
    0x00FF0000,   // P1 — Red
    0x00007FFF,   // P2 — Azure Blue
    0x00FFFF00,   // P3 — Yellow
    0x00FF00FF,   // P4 — Pink
};

// ── Audio sequences ──────────────────────────────────────────────
// Each value is a frequency in Hz; 0 = silence (rest).
const int WIN_NOTES[]    = { 0,2637,2637,0,2637,0,2093,2637,0,3136,0,0,0,1568 };
const int WIN_NOTES_LEN  = sizeof(WIN_NOTES)   / sizeof(int);

const int DEFEAT_NOTES[]    = { 523, 494, 466, 440, 415, 392 };
const int DEFEAT_NOTES_LEN  = sizeof(DEFEAT_NOTES) / sizeof(int);

// ────────────────────────────────────────────────────────────────
//  ESP-NOW PACKET DEFINITION
//  Both ESP1 and ESP2 use this same struct — do not change one
//  without changing the other.
// ────────────────────────────────────────────────────────────────
#define PKT_STATE  1   // direction: ESP1 → ESP2  (game state)
#define PKT_BTN    2   // direction: ESP2 → ESP1  (button event)

// Broadcast address — no MAC pairing needed
static const uint8_t BROADCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

struct __attribute__((packed)) Packet {
    uint8_t type;       // PKT_STATE or PKT_BTN
    uint8_t state;      // GameState enum value
    uint8_t laps[4];    // lap count for each player (0-3)
    uint8_t winner;     // 0-3 = winner index; 0xFF = no winner yet
    uint8_t btnPlayer;  // (PKT_BTN only) which player's button was pressed
    uint8_t cdNum;      // countdown beat number: 3 / 2 / 1 / 0(GO) / 0xFF
};

// ────────────────────────────────────────────────────────────────
//  HARDWARE OBJECTS
// ────────────────────────────────────────────────────────────────
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);
TM1637Display     tmDisplay(PIN_TM_CLK, PIN_TM_DIO);

// ────────────────────────────────────────────────────────────────
//  GAME STATE
// ────────────────────────────────────────────────────────────────
enum GameState { STATE_IDLE, STATE_COUNTDOWN, STATE_RACING, STATE_WIN };
GameState gameState = STATE_IDLE;

// Per-player data bundle
struct PlayerState {
    float   speed;    // current speed in LEDs-per-tick
    float   dist;     // total distance in LED widths (always increasing)
    uint8_t laps;     // completed laps
    bool    prevBtn;  // button state last tick (for rising-edge detection)
};
PlayerState players[NUM_PLAYERS];

// ────────────────────────────────────────────────────────────────
//  BUTTON LED GLOBALS
// ────────────────────────────────────────────────────────────────
// stepButtonLEDs() smoothly moves 'current' toward 'target' each tick.
uint8_t btnLedCurrent[NUM_PLAYERS] = { 0, 0, 0, 0 };
uint8_t btnLedTarget [NUM_PLAYERS] = { 0, 0, 0, 0 };

// ────────────────────────────────────────────────────────────────
//  MISC GLOBALS
// ────────────────────────────────────────────────────────────────
int           buzzerTicks       = 0;
uint16_t      rainbowHue        = 0;
unsigned long lastButtonPressMs = 0;
bool          displayDirty      = false;

// Remote button inbox — written by ESP-NOW callback, read in main loop
volatile bool    remoteBtnEvent  = false;
volatile uint8_t remoteBtnPlayer = 0;

// Common 7-segment patterns
static const uint8_t SEG_BLANK   = 0b00000000;
static const uint8_t SEG_DASH    = 0b01000000;
static const uint8_t SEG_DEAD[4] = { 0x5E, 0x79, 0x77, 0x5E }; // "dEAd"
static const uint8_t SEG_GO[4]   = { SEG_BLANK, 0x3D, 0x5C, SEG_BLANK }; // " Go "

// ================================================================
//  SECTION: BUTTON LED CONTROL
// ================================================================

// Set the target brightness for one button LED (0-255).
// stepButtonLEDs() will glide toward this value each tick.
void setButtonLedTarget(int i, uint8_t brt) {
    btnLedTarget[i] = brt;
}

// Set all 4 button LEDs to the same target brightness.
void setAllButtonLedTargets(uint8_t brt) {
    for (int i = 0; i < NUM_PLAYERS; i++) btnLedTarget[i] = brt;
}

/*
 * stepButtonLEDs()
 * ─────────────────
 * Called every tick (and from inside blocking loops).
 * Moves each LED's PWM one step toward its target, creating smooth
 * fade-in / fade-out transitions without any blocking delay.
 */
void stepButtonLEDs() {
    for (int i = 0; i < NUM_PLAYERS; i++) {
        if      (btnLedCurrent[i] < btnLedTarget[i])
            btnLedCurrent[i] = (uint8_t)min(255, (int)btnLedCurrent[i] + BTN_LED_STEP);
        else if (btnLedCurrent[i] > btnLedTarget[i])
            btnLedCurrent[i] = (uint8_t)max(0,   (int)btnLedCurrent[i] - BTN_LED_STEP);

        analogWrite(BTN_LED_PIN[i], btnLedCurrent[i]);
    }
}

/*
 * breatheButtonLEDs()
 * ────────────────────
 * Used in idle state.  Computes a sine-wave brightness so all 4
 * button LEDs pulse together — making the unit look "alive" while
 * waiting for someone to press a button.
 */
void breatheButtonLEDs() {
    float   t   = (float)(millis() % BREATHE_PERIOD_MS) / BREATHE_PERIOD_MS;
    float   s   = 0.5f * (1.0f - cosf(2.0f * PI * t)); // smooth 0…1
    uint8_t brt = (uint8_t)(BTN_LED_MIN + (255 - BTN_LED_MIN) * s);
    setAllButtonLedTargets(brt);
    stepButtonLEDs();
}

// ================================================================
//  SECTION: ESP-NOW
// ================================================================

/*
 * sendState(winner, cdNum)
 * ─────────────────────────
 * Broadcasts the current game state to ESP2.
 * Called whenever state changes, or each lap, or each countdown beat.
 *
 * winner  : winning player index (0-3) or 0xFF if none yet
 * cdNum   : countdown number 3/2/1/0 or 0xFF when not counting down
 */
void sendState(uint8_t winner = 0xFF, uint8_t cdNum = 0xFF) {
    Packet pkt;
    pkt.type      = PKT_STATE;
    pkt.state     = (uint8_t)gameState;
    for (int i = 0; i < NUM_PLAYERS; i++) pkt.laps[i] = players[i].laps;
    pkt.winner    = winner;
    pkt.btnPlayer = 0xFF;
    pkt.cdNum     = cdNum;
    esp_now_send(BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
}

/*
 * onDataReceived()
 * ─────────────────
 * ESP-NOW receive callback — runs in Wi-Fi task context, NOT in loop().
 * We ONLY set volatile flags here; actual handling happens in loop()
 * to avoid race conditions with the game logic.
 */
void onDataReceived(const esp_now_recv_info_t* info,
                    const uint8_t* data, int len)
{
    if (len < (int)sizeof(Packet)) return;
    const Packet* pkt = (const Packet*)data;

    if (pkt->type == PKT_BTN) {
        // Store the remote button event; loop() will pick it up
        remoteBtnEvent  = true;
        remoteBtnPlayer = pkt->btnPlayer;
    }
}

void initEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Print this unit's MAC — paste it into ESP2's PEER_MAC if needed
    Serial.print("ESP1 MAC: ");
    Serial.println(WiFi.macAddress());

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED — check board/antenna");
        while (true) delay(1000);
    }

    esp_now_register_recv_cb(onDataReceived);

    // Add broadcast peer so we can send to ESP2 without its MAC
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.println("ESP-NOW ready (broadcast mode)");
}

// ================================================================
//  SECTION: UTILITIES
// ================================================================

// Returns true if player i's physical button is currently pressed
bool readButton(int i) { return digitalRead(BTN_PIN[i]) == LOW; }

// Rising-edge detector: returns true only on the frame the button
// goes from NOT pressed → pressed.
bool risingEdge(int i) {
    bool now  = readButton(i);
    bool edge = now && !players[i].prevBtn;
    players[i].prevBtn = now;
    return edge;
}

// Consume and return the buffered remote button player index,
// or -1 if no remote event is pending.
int checkRemoteButton() {
    if (!remoteBtnEvent) return -1;
    remoteBtnEvent = false;
    return (int)remoteBtnPlayer;
}

// Scale an RGB colour by a float factor (for dim tail pixels)
uint32_t dimColor(uint32_t c, float f) {
    uint8_t r = ((c >> 16) & 0xFF) * f;
    uint8_t g = ((c >>  8) & 0xFF) * f;
    uint8_t b = ( c        & 0xFF) * f;
    return strip.Color(r, g, b);
}

// Start a buzzer tone and let tickBuzzer() stop it after 'ticks' ticks
void beep(int freq, int ticks) {
    tone(PIN_AUDIO, freq);
    buzzerTicks = ticks;
}

// Must be called every tick during racing to auto-stop the buzzer
void tickBuzzer() {
    if (buzzerTicks > 0) {
        buzzerTicks--;
        if (buzzerTicks == 0) noTone(PIN_AUDIO);
    }
}

// ================================================================
//  SECTION: TM1637 DISPLAY
// ================================================================

/*
 * updateDisplay()
 * ────────────────
 * Shows all 4 players' lap counts across the 4 digits.
 * No separators — every digit belongs to one player.
 *
 * Example (laps = 3, 0, 2, 1):
 *   ┌─┬─┬─┬─┐
 *   │3│0│2│1│
 *   └─┴─┴─┴─┘
 *    P1 P2 P3 P4
 */
void updateDisplay() {
    uint8_t segs[4];
    for (int i = 0; i < 4; i++) segs[i] = tmDisplay.encodeDigit(players[i].laps);
    tmDisplay.setSegments(segs, 4, 0);
}

// Show ---- while idle (no game running)
void displayIdle() {
    uint8_t s[4] = { SEG_DASH, SEG_DASH, SEG_DASH, SEG_DASH };
    tmDisplay.setSegments(s, 4, 0);
}

// Show  n n  (centre two digits) during countdown beats
void displayCount(int n) {
    uint8_t d  = tmDisplay.encodeDigit(n);
    uint8_t s[4] = { SEG_BLANK, d, d, SEG_BLANK };
    tmDisplay.setSegments(s, 4, 0);
}

// ================================================================
//  SECTION: LED RENDERING
// ================================================================

/*
 * drawStartLine()
 * ────────────────
 * Paints STARTLINE_WIDTH white LEDs centred on LED_STARTLINE.
 * Centre pixel is full white; side pixels are dimmer so the
 * centre stands out as the "line" crossing point.
 */
void drawStartLine() {
    int half = STARTLINE_WIDTH / 2;
    for (int offset = -half; offset <= half; offset++) {
        int pos = (LED_STARTLINE + offset + NUM_LEDS) % NUM_LEDS;
        strip.setPixelColor(pos, (offset == 0) ? 0x00FFFFFF : 0x00999999);
    }
}

/*
 * drawCars()
 * ───────────
 * Clears the strip, draws the start-line, then draws each player
 * as a bright head pixel + dimmer tail pixel.
 *
 * The draw order alternates every second so no single player
 * always appears "on top" when two cars share a pixel.
 */
void drawCars() {
    strip.clear();
    drawStartLine();

    bool flip = (millis() / 1000) & 1; // alternate draw order each second
    for (int n = 0; n < NUM_PLAYERS; n++) {
        int i    = flip ? (NUM_PLAYERS - 1 - n) : n;
        int pos  = (int)players[i].dist % NUM_LEDS;
        int tail = (pos - 1 + NUM_LEDS) % NUM_LEDS;

        strip.setPixelColor(pos,  PLAYER_COLOR[i]);
        strip.setPixelColor(tail, dimColor(PLAYER_COLOR[i], 0.15f));
    }
}

// ================================================================
//  SECTION: COUNTDOWN
// ================================================================

/*
 * symmetricSweep(color, sweepMs)
 * ───────────────────────────────
 * Animates two light points that travel from the centre of the strip
 * outward to the ends, meeting at LED_STARTLINE on the last frame.
 * Runs for exactly sweepMs milliseconds.
 * Calls stepButtonLEDs() each frame so button LEDs stay smooth.
 */
void symmetricSweep(uint32_t color, unsigned long sweepMs) {
    int  half    = NUM_LEDS / 2;
    long frameMs = (long)sweepMs / COUNTDOWN_SWEEP_FRAMES;
    if (frameMs < 1) frameMs = 1;

    for (int f = 0; f < COUNTDOWN_SWEEP_FRAMES; f++) {
        unsigned long t0 = millis();

        int progress = (int)((long)f * half / COUNTDOWN_SWEEP_FRAMES);
        int lp = half - progress;                // left  point (moving left)
        int rp = (half + progress) % NUM_LEDS;  // right point (moving right)

        strip.clear();
        strip.setPixelColor(lp, color);
        strip.setPixelColor(rp, color);
        // Trailing glow on each point
        strip.setPixelColor((lp + 1) % NUM_LEDS,            dimColor(color, 0.35f));
        strip.setPixelColor((lp + 2) % NUM_LEDS,            dimColor(color, 0.12f));
        strip.setPixelColor((rp - 1 + NUM_LEDS) % NUM_LEDS, dimColor(color, 0.35f));
        strip.setPixelColor((rp - 2 + NUM_LEDS) % NUM_LEDS, dimColor(color, 0.12f));
        strip.show();

        stepButtonLEDs(); // keep button LEDs fading smoothly

        while ((long)(millis() - t0) < frameMs) {} // busy-wait rest of frame
    }
    strip.clear();
    strip.show();
}

/*
 * runCountdown()
 * ───────────────
 * Plays the 3-2-1-GO sequence.
 *   · Each beat: display the number, play a tone, run sweep animation
 *   · GO beat: flash the whole strip white
 *   · Resets all player state so the race starts fresh
 *   · Sends each beat to ESP2 so its displays sync
 */
void runCountdown() {
    gameState = STATE_COUNTDOWN;
    setAllButtonLedTargets(255); // ramp button LEDs to full during countdown

    // Beat definitions: display number, sweep colour, tone frequency
    struct Beat { int num; uint32_t color; int freq; };
    const Beat beats[3] = {
        { 3, 0x00FF0000, 400 },
        { 2, 0x00FFAA00, 600 },
        { 1, 0x0000FF00, 900 },
    };

    strip.clear(); strip.show();
    delay(300); // brief pause before 3 appears

    for (int b = 0; b < 3; b++) {
        unsigned long beatStart = millis();

        sendState(0xFF, (uint8_t)beats[b].num);   // tell ESP2 the beat number
        displayCount(beats[b].num);
        tone(PIN_AUDIO, beats[b].freq);
        delay(160);
        noTone(PIN_AUDIO);

        // Fill the remaining beat time with the sweep animation
        long elapsed = (long)(millis() - beatStart);
        long sweepMs = (long)COUNTDOWN_BEAT_MS - elapsed;
        if (sweepMs > 0) symmetricSweep(beats[b].color, (unsigned long)sweepMs);

        // Pad any leftover ms so each beat is exactly COUNTDOWN_BEAT_MS long
        while ((long)(millis() - beatStart) < (long)COUNTDOWN_BEAT_MS) {}
        strip.clear(); strip.show();
    }

    // ── GO! beat ────────────────────────────────────────────────────
    {
        unsigned long beatStart = millis();
        sendState(0xFF, 0);                    // 0 = GO on ESP2
        tmDisplay.setSegments(SEG_GO, 4, 0);
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x00FFFFFF);
        strip.show();
        tone(PIN_AUDIO, 1800); delay(160); noTone(PIN_AUDIO);
        while ((long)(millis() - beatStart) < (long)COUNTDOWN_BEAT_MS) {}
        strip.clear(); strip.show();
    }

    // ── Reset all player state ───────────────────────────────────────
    Serial.println("GO!");
    for (int i = 0; i < NUM_PLAYERS; i++) {
        players[i].speed   = 0;
        players[i].dist    = 0;
        players[i].laps    = 0;
        players[i].prevBtn = false;
    }
    lastButtonPressMs = millis();
    updateDisplay();
    gameState = STATE_RACING;
    sendState(); // tell ESP2 race has begun
}

// ================================================================
//  SECTION: WIN ANIMATION
// ================================================================

/*
 * crossFadeStrip(from, to, steps, stepDelayMs)
 * ──────────────────────────────────────────────
 * Smoothly blends every strip pixel from one colour to another.
 * Calls stepButtonLEDs() at every frame so button LEDs continue
 * fading even while this blocking animation runs.
 */
void crossFadeStrip(uint32_t from, uint32_t to, int steps, int stepDelayMs) {
    float fr = (from >> 16) & 0xFF,  fg = (from >> 8) & 0xFF,  fb = from & 0xFF;
    float tr = (to   >> 16) & 0xFF,  tg = (to   >> 8) & 0xFF,  tb = to   & 0xFF;

    for (int s = 0; s <= steps; s++) {
        float   t = (float)s / steps;
        uint8_t r = fr + (tr - fr) * t;
        uint8_t g = fg + (tg - fg) * t;
        uint8_t b = fb + (tb - fb) * t;

        for (int i = 0; i < NUM_LEDS; i++)
            strip.setPixelColor(i, strip.Color(r, g, b));
        strip.show();

        stepButtonLEDs(); // ← keeps button LED fade alive during blocking strip work
        delay(stepDelayMs);
    }
}

/*
 * fadeInToIdle()
 * ───────────────
 * Bridges the win animation back to the idle rainbow.
 * Drops strip brightness to 0, fills rainbow colours, then ramps
 * brightness back up while advancing the hue — no hard visual cut.
 * Button LEDs switch to breathing mode during this transition.
 */
void fadeInToIdle() {
    gameState    = STATE_IDLE;
    rainbowHue   = 0;
    displayDirty = false;
    displayIdle();
    sendState(); // inform ESP2 we are back to idle

    uint8_t normalBrightness = strip.getBrightness();
    strip.setBrightness(0);

    // ★ EDIT fadeSteps: each step ~15 ms → 50 steps ≈ 750 ms fade-in
    const int fadeSteps = 50;
    for (int b = 0; b <= fadeSteps; b++) {
        uint8_t brt = (uint8_t)((long)b * normalBrightness / fadeSteps);
        strip.setBrightness(brt);
        strip.rainbow(rainbowHue);
        rainbowHue += RAINBOW_STEP;
        strip.show();
        breatheButtonLEDs(); // button LEDs start their sine-wave breath
        delay(15);
    }
    strip.setBrightness(normalBrightness);

    Serial.println("Idle — waiting for button press...");
}

/*
 * runWinAnimation(winner)
 * ────────────────────────
 * Full win sequence with button LED behaviour:
 *
 *  [1] Freeze cars at final positions
 *      → Button LEDs start fading OUT (target = 0 for all)
 *  [2] Strip fades from black → green  (victory flash)
 *      → stepButtonLEDs() called inside crossFadeStrip
 *  [3] Strip fades green → winner's colour
 *      → by end of this, button LEDs are near zero
 *  [4] Non-blocking jingle + display blink loop
 *      → Winner LED target set to 255, fades IN during jingle
 *  [5] Strip fades winner colour → black
 *      → winner LED stays fully lit
 *  [6] Rainbow fades in from black
 *      → all button LEDs begin breathing again
 */
void runWinAnimation(int winner) {
    gameState = STATE_WIN;
    noTone(PIN_AUDIO);

    Serial.print("WINNER: Player ");
    Serial.println(winner + 1);

    sendState((uint8_t)winner); // tell ESP2 who won

    // ── Step 1: freeze ───────────────────────────────────────────────
    drawCars();
    strip.show();
    setAllButtonLedTargets(0); // all button LEDs begin fading out
    delay(400);

    // ── Steps 2 & 3: colour reveal (stepButtonLEDs called inside) ────
    crossFadeStrip(0x00000000, 0x0000FF00, 80,  10); // black → green
    delay(250);
    crossFadeStrip(0x0000FF00, PLAYER_COLOR[winner], 100, 12); // green → winner

    // ── Step 4: jingle + display blink (non-blocking millis loop) ────
    {
        uint8_t segs[4] = { SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK };
        int  noteIdx    = 0;
        bool segOn      = false;
        unsigned long noteTimer = millis();
        unsigned long segTimer  = millis();

        // Fire first note and turn the display digit on immediately
        if (WIN_NOTES[0] > 0) tone(PIN_AUDIO, WIN_NOTES[0], 200);
        noteIdx = 1;
        segs[winner] = tmDisplay.encodeDigit(players[winner].laps);
        tmDisplay.setSegments(segs, 4, 0);
        segOn = true;

        // Now winner's button LED starts fading IN
        setButtonLedTarget(winner, 255);

        // Run until all note slots have elapsed (230 ms each)
        while (noteIdx <= WIN_NOTES_LEN) {
            unsigned long now = millis();

            // Advance the jingle one note per 230 ms
            if (now - noteTimer >= 230UL) {
                noteTimer = now;
                noTone(PIN_AUDIO);
                if (noteIdx < WIN_NOTES_LEN && WIN_NOTES[noteIdx] > 0)
                    tone(PIN_AUDIO, WIN_NOTES[noteIdx], 200);
                noteIdx++;
            }

            // Blink the winner's lap digit on / off
            unsigned long segInterval = segOn ? 300UL : 200UL;
            if (now - segTimer >= segInterval) {
                segTimer = now;
                segOn    = !segOn;
                segs[winner] = segOn ? tmDisplay.encodeDigit(players[winner].laps) : SEG_BLANK;
                tmDisplay.setSegments(segs, 4, 0);
            }

            stepButtonLEDs(); // winner LED smoothly fades in
            delay(2);
        }
        noTone(PIN_AUDIO);
    }

    delay(800); // short pause after last jingle note

    // ── Step 5: strip fades to black; winner LED stays at 255 ────────
    crossFadeStrip(PLAYER_COLOR[winner], 0x00000000, 60, 15); // ~900 ms

    // ── Step 6: rainbow fades in + button LEDs start breathing ────────
    fadeInToIdle();
}

// ================================================================
//  SECTION: DEFEAT (inactivity timeout)
// ================================================================

/*
 * playDefeatSequence()
 * ─────────────────────
 * Plays a descending "sad" tone, shows "dEAd" on the display,
 * dims the strip red, and fades all button LEDs out.
 * Called when no button has been pressed for RACING_INACTIVITY_MS.
 */
void playDefeatSequence() {
    noTone(PIN_AUDIO);
    tmDisplay.setSegments(SEG_DEAD, 4, 0);

    for (int i = 0; i < NUM_LEDS; i++)
        strip.setPixelColor(i, 0x00200000); // dim red
    strip.show();

    setAllButtonLedTargets(0); // start fading all button LEDs out

    for (int n = 0; n < DEFEAT_NOTES_LEN; n++) {
        tone(PIN_AUDIO, DEFEAT_NOTES[n], 280);
        stepButtonLEDs();
        delay(310);
    }
    noTone(PIN_AUDIO);
    delay(600);
    strip.clear();
    strip.show();
}

// ================================================================
//  SECTION: IDLE
// ================================================================

// Initialise idle state variables and update the display
void enterIdle() {
    gameState    = STATE_IDLE;
    rainbowHue   = 0;
    displayDirty = false;
    displayIdle();
    sendState();
    Serial.println("Idle — waiting for button press...");
}

/*
 * loopIdle()
 * ───────────
 * Called every frame while in idle state.
 * Cycles the rainbow on the strip and breathes the button LEDs.
 * Starts the countdown when ANY button (local or remote) is pressed.
 */
void loopIdle() {
    strip.rainbow(rainbowHue);
    strip.show();
    rainbowHue += RAINBOW_STEP;

    breatheButtonLEDs(); // sine-wave pulse on all button LEDs
    delay(TICK_MS);

    // Check all local buttons for a rising edge
    for (int i = 0; i < NUM_PLAYERS; i++) {
        if (risingEdge(i)) {
            Serial.print("Local P"); Serial.print(i + 1);
            Serial.println(" pressed — countdown!");
            runCountdown();
            return;
        }
    }

    // Check if ESP2 sent a button event
    int rb = checkRemoteButton();
    if (rb >= 0) {
        Serial.print("Remote P"); Serial.print(rb + 1);
        Serial.println(" pressed — countdown!");
        runCountdown();
    }
}

// ================================================================
//  SECTION: RACING
// ================================================================

/*
 * loopRacing()
 * ─────────────
 * Main physics + rendering loop, called every TICK_MS.
 *
 * Per-tick sequence:
 *  1. Check inactivity timeout → abort to idle if expired
 *  2. Read all local + remote button events
 *  3. Apply acceleration + friction to each player's speed
 *  4. Advance each player's distance
 *  5. Detect lap completions and check for a winner
 *  6. Refresh the LED strip and TM1637 display
 *  7. Tick the buzzer auto-stop counter
 */
void loopRacing() {
    // ── 1. Inactivity check ──────────────────────────────────────────
    if (millis() - lastButtonPressMs >= RACING_INACTIVITY_MS) {
        Serial.println("Inactivity timeout — aborting race");
        playDefeatSequence();
        sendState();
        enterIdle();
        return;
    }

    // ── 2. Collect remote button event (once per frame) ───────────────
    int remotePlayer = checkRemoteButton(); // -1 if no event

    // ── 3 & 4. Physics per player ─────────────────────────────────────
    for (int i = 0; i < NUM_PLAYERS; i++) {
        // Pressed = local button OR matching remote button this frame
        bool pressed = risingEdge(i) || (remotePlayer == i);

        if (pressed) {
            players[i].speed  += ACCEL;
            lastButtonPressMs  = millis();
        }

        // Apply friction and advance position
        players[i].speed -= players[i].speed * FRICTION;
        if (players[i].speed < 0.0f) players[i].speed = 0.0f;
        players[i].dist  += players[i].speed;

        // ── 5. Lap detection ──────────────────────────────────────────
        uint32_t lapThreshold = (uint32_t)NUM_LEDS * (players[i].laps + 1);
        if ((uint32_t)players[i].dist >= lapThreshold) {
            players[i].laps++;
            beep(600 + i * 60, 3);  // different pitch per player
            displayDirty = true;
            sendState(); // push updated laps to ESP2

            Serial.print("P"); Serial.print(i + 1);
            Serial.print(" — lap "); Serial.print(players[i].laps);
            Serial.print(" / ");     Serial.println(TOTAL_LAPS);

            // ── Win check ────────────────────────────────────────────
            if (players[i].laps >= TOTAL_LAPS) {
                for (int j = 0; j < NUM_PLAYERS; j++) players[j].speed = 0.0f;
                runWinAnimation(i);
                return; // exit loopRacing; win animation takes over
            }
        }
    }

    // ── 6. Refresh display and strip ─────────────────────────────────
    if (displayDirty) { updateDisplay(); displayDirty = false; }
    drawCars();
    strip.show();

    // Keep all button LEDs fully lit during the race
    setAllButtonLedTargets(255);
    stepButtonLEDs();

    // ── 7. Buzzer tick ────────────────────────────────────────────────
    tickBuzzer();
    delay(TICK_MS);
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("=== ESP1 LED Race — booting... ===");

    // ── Button pins and button LED pins ──────────────────────────────
    for (int i = 0; i < NUM_PLAYERS; i++) {
        pinMode(BTN_PIN[i],     INPUT_PULLUP);
        pinMode(BTN_LED_PIN[i], OUTPUT);
        analogWrite(BTN_LED_PIN[i], 0); // start off
    }
    pinMode(PIN_AUDIO, OUTPUT);

    // ── NeoPixel strip ────────────────────────────────────────────────
    strip.begin();
    strip.clear();
    strip.show();
    strip.setBrightness(SR_BRIGHTNESS);

    // ── TM1637 display ────────────────────────────────────────────────
    tmDisplay.setBrightness(TM_BRIGHTNESS);
    displayIdle();

    // ── ESP-NOW ───────────────────────────────────────────────────────
    initEspNow();

    Serial.println("Ready! Press any button to start.");
    enterIdle();
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
    switch (gameState) {
        case STATE_IDLE:      loopIdle();     break;
        case STATE_COUNTDOWN: runCountdown(); break; // runCountdown is self-contained
        case STATE_RACING:    loopRacing();   break;
        case STATE_WIN:       break;           // win animation is self-contained
    }
}
