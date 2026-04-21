/*
 * ================================================================
 *  ESP2_Remote.ino  —  Handheld Remote / Scoreboard Unit
 *  4-Player WS2812B LED Strip Racing Game  ·  ESP32 Edition
 * ================================================================
 *
 *  WHAT THIS UNIT DOES
 *  ────────────────────
 *  · Reads 4 buttons and sends button-press events to ESP1
 *  · Receives game-state packets from ESP1 via ESP-NOW
 *  · Mirrors the current lap counts on TWO TM1637 displays
 *  · Controls 4 button LEDs to match the game phase:
 *      IDLE      → all 4 LEDs breathe (sine-wave pulse)
 *      COUNTDOWN → all 4 LEDs at full brightness
 *      RACING    → all 4 LEDs at full brightness
 *      WIN       → all LEDs fade OUT → winner LED fades IN → idle
 *  · Game can be started by pressing any button on THIS unit
 *    (the event is relayed to ESP1 which runs the countdown)
 *
 *  HARDWARE WIRING
 *  ────────────────
 *  TM1637-A  CLK    → GPIO 18
 *  TM1637-A  DIO    → GPIO 19    (shows P1 left, P2 right)
 *  TM1637-B  CLK    → GPIO 21
 *  TM1637-B  DIO    → GPIO 22    (shows P3 left, P4 right)
 *  Player 1  button → GPIO 32   (INPUT_PULLUP)
 *  Player 1  LED    → GPIO 25   (PWM out)
 *  Player 2  button → GPIO 33
 *  Player 2  LED    → GPIO 26
 *  Player 3  button → GPIO 4
 *  Player 3  LED    → GPIO 27
 *  Player 4  button → GPIO 13
 *  Player 4  LED    → GPIO 14
 *
 *  DISPLAY FORMAT
 *  ───────────────
 *  TM1637-A:  [P1 laps] [--] [--] [P2 laps]
 *  TM1637-B:  [P3 laps] [--] [--] [P4 laps]
 *
 *  Example when laps = P1:3, P2:0, P3:2, P4:1
 *    Display A  →  3 - - 0
 *    Display B  →  2 - - 1
 *
 *  Both displays show the same countdown number during 3-2-1-GO.
 *
 *  NOTE: This unit has NO NeoPixel strip.  All strip rendering
 *        happens on ESP1.  This unit is the controller / scoreboard.
 *
 *  LIBRARIES REQUIRED
 *  ───────────────────
 *  · TM1637Display  (by Avishay Orpaz)
 *  · ESP32 board support (espressif/arduino-esp32 v3.x)
 * ================================================================
 */

// ────────────────────────────────────────────────────────────────
//  LIBRARIES
// ────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <TM1637Display.h>
#include <WiFi.h>
#include <esp_now.h>
#include <math.h>

// ────────────────────────────────────────────────────────────────
//  ★ SETTINGS
// ────────────────────────────────────────────────────────────────

// ── Button & button-LED pins (must match ESP1's layout) ──────────
const uint8_t BTN_PIN    [4] = { 32, 33,  4, 13 }; // ★ button inputs
const uint8_t BTN_LED_PIN[4] = { 25, 26, 27, 14 }; // ★ LED outputs (PWM)

// ── TM1637 Display A  (left unit — shows P1 and P2) ──────────────
#define TM_A_CLK  18  // ★
#define TM_A_DIO  19  // ★

// ── TM1637 Display B  (right unit — shows P3 and P4) ─────────────
#define TM_B_CLK  21  // ★
#define TM_B_DIO  22  // ★

#define TM_BRIGHTNESS  7   // ★ 0 (dim) … 7 (bright), applied to both

// ── Button LED breathing (idle) ──────────────────────────────────
#define BREATHE_PERIOD_MS  2000UL  // ★ ms for one full breath cycle
#define BTN_LED_STEP       3       // ★ PWM change per tick (1=slow,10=fast)
#define BTN_LED_MIN        20      // ★ darkest point of breath (0-255)

#define LOOP_DELAY_MS      5       // ★ main loop tick (ms)

// ────────────────────────────────────────────────────────────────
//  ESP-NOW PACKET  — MUST match ESP1 exactly, byte for byte
// ────────────────────────────────────────────────────────────────
#define PKT_STATE  1   // direction: ESP1 → ESP2
#define PKT_BTN    2   // direction: ESP2 → ESP1

static const uint8_t BROADCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

struct __attribute__((packed)) Packet {
    uint8_t type;       // PKT_STATE or PKT_BTN
    uint8_t state;      // GameState enum value (matches ESP1's enum)
    uint8_t laps[4];    // lap count for players 0-3
    uint8_t winner;     // 0-3 = winner; 0xFF = none
    uint8_t btnPlayer;  // (PKT_BTN only) which player pressed
    uint8_t cdNum;      // countdown number: 3/2/1/0(GO)/0xFF
};

// Mirror of ESP1's GameState enum — keep values identical
enum GameState { STATE_IDLE, STATE_COUNTDOWN, STATE_RACING, STATE_WIN };

// ────────────────────────────────────────────────────────────────
//  HARDWARE OBJECTS
// ────────────────────────────────────────────────────────────────
TM1637Display tmA(TM_A_CLK, TM_A_DIO); // left display  (P1 | P2)
TM1637Display tmB(TM_B_CLK, TM_B_DIO); // right display (P3 | P4)

// ────────────────────────────────────────────────────────────────
//  MIRRORED GAME STATE
//  Written by the ESP-NOW receive callback; read in loop().
//  All volatile because the callback runs in a different task.
// ────────────────────────────────────────────────────────────────
volatile GameState mirrorState      = STATE_IDLE;
volatile uint8_t  mirrorLaps[4]     = { 0, 0, 0, 0 };
volatile uint8_t  mirrorWinner      = 0xFF;
volatile uint8_t  mirrorCdNum       = 0xFF;
volatile bool     stateReceived     = false; // flag: new packet arrived

// ────────────────────────────────────────────────────────────────
//  BUTTON LED STATE
// ────────────────────────────────────────────────────────────────
uint8_t btnLedCurrent[4] = { 0, 0, 0, 0 }; // actual PWM value right now
uint8_t btnLedTarget [4] = { 0, 0, 0, 0 }; // desired PWM value to glide toward

// ────────────────────────────────────────────────────────────────
//  BUTTON STATE  (for rising-edge detection)
// ────────────────────────────────────────────────────────────────
bool prevBtnState[4] = { false, false, false, false };

// ── Common 7-segment constants ───────────────────────────────────
static const uint8_t SEG_BLANK   = 0b00000000;
static const uint8_t SEG_DASH    = 0b01000000;
static const uint8_t SEG_DEAD[4] = { 0x5E, 0x79, 0x77, 0x5E }; // "dEAd"
static const uint8_t SEG_GO[4]   = { SEG_BLANK, 0x3D, 0x5C, SEG_BLANK }; // " Go "

// ================================================================
//  SECTION: BUTTON LED CONTROL
// ================================================================

// Set the target brightness for one button LED.
// The actual PWM glides toward this in stepButtonLEDs().
void setButtonLedTarget(int i, uint8_t brt) {
    btnLedTarget[i] = brt;
}

// Set the same target for all 4 button LEDs at once.
void setAllButtonLedTargets(uint8_t brt) {
    for (int i = 0; i < 4; i++) btnLedTarget[i] = brt;
}

/*
 * stepButtonLEDs()
 * ─────────────────
 * Called every loop tick (~LOOP_DELAY_MS ms).
 * Moves each LED's PWM value one step toward its target.
 * Speed of fade is controlled by BTN_LED_STEP:
 *   fade time ≈ ceil(255 / BTN_LED_STEP) × LOOP_DELAY_MS milliseconds
 *   e.g. step=3, delay=5 ms  →  ceil(255/3)×5 = 425 ms full fade
 */
void stepButtonLEDs() {
    for (int i = 0; i < 4; i++) {
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
 * Sine-wave breathing effect for the idle state.
 * All 4 button LEDs pulse together so the remote feels alive.
 * Uses millis() — no blocking.
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
 * sendButtonToMaster(player)
 * ──────────────────────────
 * Broadcasts a PKT_BTN packet to ESP1 telling it which player's
 * button was just pressed on the remote.
 * ESP1 will act on this exactly as if its own local button was pressed.
 */
void sendButtonToMaster(uint8_t player) {
    Packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type      = PKT_BTN;
    pkt.state     = 0;
    pkt.winner    = 0xFF;
    pkt.btnPlayer = player;
    pkt.cdNum     = 0xFF;
    esp_now_send(BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
}

/*
 * onDataReceived()
 * ─────────────────
 * ESP-NOW receive callback — fires in Wi-Fi task context.
 * Only stores data into volatiles; all display/LED logic
 * runs in loop() to avoid ISR-context issues.
 */
void onDataReceived(const esp_now_recv_info_t* info,
                    const uint8_t* data, int len)
{
    if (len < (int)sizeof(Packet)) return;
    const Packet* pkt = (const Packet*)data;

    if (pkt->type == PKT_STATE) {
        mirrorState  = (GameState)pkt->state;
        for (int i = 0; i < 4; i++) mirrorLaps[i] = pkt->laps[i];
        mirrorWinner = pkt->winner;
        mirrorCdNum  = pkt->cdNum;
        stateReceived = true; // signal loop() to refresh displays
    }
}

void initEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // ★ This MAC is what you paste into ESP1's PEER_MAC (if needed)
    Serial.print("ESP2 MAC: ");
    Serial.println(WiFi.macAddress());

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED — check board/antenna");
        while (true) delay(1000);
    }

    esp_now_register_recv_cb(onDataReceived);

    // Add broadcast peer so we can send to ESP1 without its MAC
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.println("ESP-NOW ready (broadcast mode)");
}

// ================================================================
//  SECTION: DISPLAY FUNCTIONS
// ================================================================

/*
 * updateDisplays(laps)
 * ─────────────────────
 * Renders both TM1637 displays from the 4-element laps array.
 *
 * Layout:
 *   Display A:  [P1] [ - ] [ - ] [P2]
 *   Display B:  [P3] [ - ] [ - ] [P4]
 *
 * The two dashes in the middle clearly separate the two player
 * numbers on each display so they are easy to read at a glance.
 */
void updateDisplays(const uint8_t laps[4]) {
    // Display A — Player 1 (leftmost digit) and Player 2 (rightmost digit)
    uint8_t segsA[4] = {
        tmA.encodeDigit(laps[0]),  // P1 laps
        SEG_DASH,                   // middle separator
        SEG_DASH,
        tmA.encodeDigit(laps[1])   // P2 laps
    };
    tmA.setSegments(segsA, 4, 0);

    // Display B — Player 3 (leftmost digit) and Player 4 (rightmost digit)
    uint8_t segsB[4] = {
        tmB.encodeDigit(laps[2]),  // P3 laps
        SEG_DASH,
        SEG_DASH,
        tmB.encodeDigit(laps[3])   // P4 laps
    };
    tmB.setSegments(segsB, 4, 0);
}

// Show  ----  on both displays (idle / waiting state)
void displayBothIdle() {
    uint8_t s[4] = { SEG_DASH, SEG_DASH, SEG_DASH, SEG_DASH };
    tmA.setSegments(s, 4, 0);
    tmB.setSegments(s, 4, 0);
}

/*
 * displayBothCount(n)
 * ────────────────────
 * Shows a countdown number on both displays simultaneously.
 * n = 3, 2, or 1 → shows  _n n_  (blank, digit, digit, blank)
 * n = 0          → shows   Go    using segment patterns
 */
void displayBothCount(int n) {
    if (n == 0) {
        // "GO" — same pattern as ESP1
        tmA.setSegments(SEG_GO, 4, 0);
        tmB.setSegments(SEG_GO, 4, 0);
    } else {
        uint8_t d  = tmA.encodeDigit(n);
        uint8_t s[4] = { SEG_BLANK, d, d, SEG_BLANK };
        tmA.setSegments(s, 4, 0);
        tmB.setSegments(s, 4, 0);
    }
}

// Show "dEAd" on both displays (defeat / timeout)
void displayBothDead() {
    tmA.setSegments(SEG_DEAD, 4, 0);
    tmB.setSegments(SEG_DEAD, 4, 0);
}

// ================================================================
//  SECTION: STATE HANDLING
// ================================================================

/*
 * applyStateUpdate()
 * ───────────────────
 * Called from loop() whenever stateReceived is true.
 * Reads the mirrored volatile values and updates both displays
 * and the button LED targets to match the current game phase.
 *
 * Note: The smooth LED fading (stepButtonLEDs) happens in loop(),
 * not here.  We only set targets; the stepping is continuous.
 */
void applyStateUpdate() {
    // Snapshot volatiles into local variables (safe read)
    GameState st     = mirrorState;
    uint8_t   laps[4];
    for (int i = 0; i < 4; i++) laps[i] = mirrorLaps[i];
    uint8_t   winner = mirrorWinner;
    uint8_t   cdNum  = mirrorCdNum;

    switch (st) {

        // ── IDLE ────────────────────────────────────────────────────
        case STATE_IDLE:
            displayBothIdle();
            // Button LED breathing is handled in loop() — nothing to set here
            Serial.println("Mirror: IDLE");
            break;

        // ── COUNTDOWN ───────────────────────────────────────────────
        case STATE_COUNTDOWN:
            // Show the countdown number (3, 2, 1, or 0=GO) sent by ESP1
            if (cdNum <= 3) {
                displayBothCount((int)cdNum);
            } else {
                displayBothIdle(); // safety fallback
            }
            // Ramp all button LEDs to full during countdown
            setAllButtonLedTargets(255);
            break;

        // ── RACING ──────────────────────────────────────────────────
        case STATE_RACING:
            updateDisplays(laps);     // show live lap counts
            setAllButtonLedTargets(255); // all fully lit
            Serial.print("Mirror laps: ");
            for (int i = 0; i < 4; i++) {
                Serial.print(laps[i]); Serial.print(" ");
            }
            Serial.println();
            break;

        // ── WIN ─────────────────────────────────────────────────────
        case STATE_WIN:
            // Update the lap count display one last time (final counts)
            updateDisplays(laps);

            // Button LED win sequence:
            //   · All LEDs target 0 (fade OUT)
            //   · Winner's LED target 255 (fade IN over all others)
            setAllButtonLedTargets(0);
            if (winner < 4) {
                setButtonLedTarget(winner, 255);
            }

            Serial.print("Mirror: WIN  winner=P");
            if (winner < 4) Serial.println(winner + 1);
            else            Serial.println("?");
            break;
    }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("=== ESP2 Remote — booting... ===");

    // ── Button pins + button LED pins ────────────────────────────────
    for (int i = 0; i < 4; i++) {
        pinMode(BTN_PIN[i],     INPUT_PULLUP);
        pinMode(BTN_LED_PIN[i], OUTPUT);
        analogWrite(BTN_LED_PIN[i], 0); // all off at boot
    }

    // ── TM1637 displays ───────────────────────────────────────────────
    tmA.setBrightness(TM_BRIGHTNESS);
    tmB.setBrightness(TM_BRIGHTNESS);
    displayBothIdle();

    // ── ESP-NOW ───────────────────────────────────────────────────────
    initEspNow();

    Serial.println("Ready — waiting for game state from ESP1.");
    Serial.println("Or press any button here to start a game!");
}

// ================================================================
//  LOOP
// ================================================================
/*
 * loop() runs at approximately 1000/LOOP_DELAY_MS Hz (≈200 Hz).
 *
 * Each iteration:
 *  1. Check if a fresh state packet arrived from ESP1 → apply it
 *  2. Read all 4 buttons for rising edges → send events to ESP1
 *  3. Update button LEDs:
 *       · If IDLE    → call breatheButtonLEDs() (sine wave)
 *       · Otherwise  → call stepButtonLEDs() (glide to target)
 *  4. Short delay for stable tick timing
 */
void loop() {

    // ── 1. Process incoming state from ESP1 ──────────────────────────
    if (stateReceived) {
        stateReceived = false;
        applyStateUpdate();
    }

    // ── 2. Read buttons — send press events to ESP1 ───────────────────
    for (int i = 0; i < 4; i++) {
        bool now  = (digitalRead(BTN_PIN[i]) == LOW);
        bool edge = now && !prevBtnState[i]; // detect rising edge only
        prevBtnState[i] = now;

        if (edge) {
            Serial.print("Remote P"); Serial.print(i + 1); Serial.println(" pressed");
            sendButtonToMaster((uint8_t)i);
            // Note: ESP1 decides whether to start a race or accelerate a car.
            // This unit just relays the press and mirrors the result.
        }
    }

    // ── 3. Button LED management ──────────────────────────────────────
    if (mirrorState == STATE_IDLE) {
        /*
         * Idle: override targets with a breathing sine wave.
         * breatheButtonLEDs() sets and steps in one call.
         */
        breatheButtonLEDs();
    } else {
        /*
         * All other states: applyStateUpdate() already set the targets.
         * We just need to step toward them each tick for smooth fading.
         * 
         * WIN state example:
         *   After applyStateUpdate() sets non-winner targets to 0 and
         *   winner target to 255, stepButtonLEDs() smoothly transitions:
         *     Losing LEDs: 255 → 0  (fade out over ~425 ms)
         *     Winner LED:    0 → 255 (fade in over ~425 ms)
         *   Both happen simultaneously since they have different targets.
         */
        stepButtonLEDs();
    }

    // ── 4. Tick delay ─────────────────────────────────────────────────
    delay(LOOP_DELAY_MS);
}
