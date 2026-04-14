// =============================================================
//  LED RACE  —  DISPLAY RECEIVER  (Arduino Uno)   v9
//  Works identically for Uno #1 (main side) and Uno #2 (remote side)
//
//  Changes from v8:
//    • BAUD_RATE fixed to 9600  (was 115200 — mismatched with ESP)
//    • runSerialTest() — single function, tests every display state
//    • Send 'T' + Enter from Serial Monitor to run full test
//
//  PROTOCOL  (from ESP32 over SoftwareSerial)
//  ──────────────────────────────────────────
//  Format:  P<pos><char>\n
//  Example: "P1A\n"  → digit 1 = 'A'
//           "P20\n"  → digit 2 = '0'
//           "P3-\n"  → digit 3 = '-'
//  pos : '1'…'4'  (left → right)
//  char: '0'-'9', 'A'-'Z', 'a'-'z', '-', ' '
//
//  WIRING
//  ──────
//  Display digit commons  → Uno D2, D3, D4, D5
//  Display segments A-G,DP→ Uno D6, D7, D8, D9, D10, D11, D12, D13
//  ESP32 TX (GPIO 33)     → Uno A0  (SoftwareSerial RX)
//  Common GND between ESP32 and Uno
//
//  LIBRARY
//  ───────
//  SevSeg by Dean Reading  (Arduino Library Manager)
// =============================================================

#include <SevSeg.h>
#include <SoftwareSerial.h>

// ── USER CONFIG ──────────────────────────────────────────────
#define HARDWARE_CONFIG     COMMON_ANODE   // Change to COMMON_CATHODE if needed
#define DISPLAY_BRIGHTNESS  90             // 0–100

// Digit select pins (left = digit 1, right = digit 4)
const byte DIGIT_PINS[4]   = { 2, 3, 4, 5 };

// Segment pins: A, B, C, D, E, F, G, DP
const byte SEGMENT_PINS[8] = { 6, 7, 8, 9, 10, 11, 12, 13 };

// SoftwareSerial — ESP32 TX (GPIO 33) → Uno A0
#define ESP_RX_PIN   A0
#define ESP_TX_PIN   A1    // Unused (Uno does not send back)
#define BAUD_RATE    9600  // Must match UNO_BAUD in ESP1 and UNO_BAUD in ESP2

// ── GLOBALS ──────────────────────────────────────────────────
SevSeg         sevseg;
SoftwareSerial espSerial(ESP_RX_PIN, ESP_TX_PIN);
char           dispChars[5] = "----";   // 4 display chars + null terminator


// ════════════════════════════════════════════════════════════
//  DISPLAY HELPERS
// ════════════════════════════════════════════════════════════

// Push the current dispChars buffer to the SevSeg driver
void pushDisplay() {
    sevseg.setChars(dispChars);
}

// Set one digit (1-based) and refresh driver
void setDigit(uint8_t pos, char ch) {
    if(pos < 1 || pos > 4) return;
    dispChars[pos - 1] = ch;
    pushDisplay();
}


// ════════════════════════════════════════════════════════════
//  SERIAL COMMAND PARSER  —  reads ESP32 stream  "P<pos><char>\n"
// ════════════════════════════════════════════════════════════

void parseCommand(const char *buf, uint8_t len) {
    // Expected: exactly 3 bytes before '\n':  'P'  pos-digit  char
    if(len == 3 && buf[0] == 'P') {
        uint8_t pos = buf[1] - '0';   // '1'–'4' → 1–4
        char    ch  = buf[2];
        setDigit(pos, ch);
    }
    // Silently discard malformed packets
}

// Reads from SoftwareSerial (ESP32 link)
void readESPCommands() {
    static char    buf[8];
    static uint8_t idx = 0;
    while(espSerial.available()) {
        char c = espSerial.read();
        if(c == '\n') {
            parseCommand(buf, idx);
            idx = 0;
        } else if(idx < (uint8_t)(sizeof(buf) - 1)) {
            buf[idx++] = c;
        }
        // Buffer overflow (malformed): next '\n' resets it automatically
    }
}

// Reads from USB Serial — same protocol, for bench testing without ESP32
void readSerialTester() {
    static char    buf[8];
    static uint8_t idx = 0;
    while(Serial.available()) {
        char c = Serial.read();
        if(c == '\n') {
            // Check for test command 'T' before normal parse
            if(idx == 1 && buf[0] == 'T') {
                runSerialTest();
            } else {
                parseCommand(buf, idx);
            }
            idx = 0;
        } else if(idx < (uint8_t)(sizeof(buf) - 1)) {
            buf[idx++] = c;
        }
    }
}


// ════════════════════════════════════════════════════════════
//  SERIAL TEST  —  called when 'T\n' received over USB Serial
//  Tests every named display state the ESP race game uses.
// ════════════════════════════════════════════════════════════

void runSerialTest() {
    Serial.println(F("=========== UNO DISPLAY TEST START ==========="));

    auto showAndWait = [](const char *label, const char *chars, int ms) {
        Serial.print(F("[TEST] "));
        Serial.println(label);
        for(uint8_t p=1; p<=4; p++) setDigit(p, chars[p-1]);
        // Busy-wait while refreshing (sevseg.refreshDisplay() must run continuously)
        unsigned long t = millis();
        while(millis() - t < (unsigned long)ms)
            sevseg.refreshDisplay();
    };

    // Idle  "0--0"
    showAndWait("IDLE    → '0--0'",   "0--0", 1000);

    // Countdown  " 3 3"  " 2 2"  " 1 1"
    showAndWait("COUNT 3 → ' 33 '",   " 33 ", 700);
    showAndWait("COUNT 2 → ' 22 '",   " 22 ", 700);
    showAndWait("COUNT 1 → ' 11 '",   " 11 ", 700);

    // Go  " Go "
    showAndWait("GO      → ' Go '",   " Go ", 700);

    // Laps  "1234"
    showAndWait("LAPS    → '1234'",   "1234", 800);

    // Win  "OvEr"
    showAndWait("OVER    → 'OvEr'",   "OvEr", 1000);

    // Defeat  "dEAd"
    showAndWait("DEAD    → 'dEAd'",   "dEAd", 1000);

    // All segments on  "8888"
    showAndWait("ALL ON  → '8888'",   "8888", 800);

    // Blank  "    "
    showAndWait("BLANK   → '    '",   "    ", 500);

    // Restore idle
    for(uint8_t p=1; p<=4; p++) setDigit(p, "0--0"[p-1]);
    pushDisplay();

    Serial.println(F("=========== UNO DISPLAY TEST END ==========="));
    Serial.println(F("Commands: P1A, P2B, P3-, P4C  or 'T' to rerun"));
}


// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════

void setup() {
    sevseg.begin(
        HARDWARE_CONFIG,
        4,
        (byte*)DIGIT_PINS,
        (byte*)SEGMENT_PINS,
        false,   // resistorsOnSegments
        false,   // updateWithDelays
        false,   // leadingZeros
        false    // disableDecPoint
    );
    sevseg.setBrightness(DISPLAY_BRIGHTNESS);
    pushDisplay();   // Show "----" at boot

    espSerial.begin(BAUD_RATE);

    Serial.begin(9600);
    Serial.println(F("UNO DISPLAY READY  (BAUD 9600)"));
    Serial.println(F("Commands: P1A P2B P3- P4C  or 'T' to run test"));
}


// ════════════════════════════════════════════════════════════
//  MAIN LOOP
//  sevseg.refreshDisplay() must be called thousands of times
//  per second for flicker-free multiplexing — keep this lean.
// ════════════════════════════════════════════════════════════

void loop() {
    readESPCommands();    // ESP32 link (SoftwareSerial)
    readSerialTester();   // USB Serial tester / debug
    sevseg.refreshDisplay();
}
