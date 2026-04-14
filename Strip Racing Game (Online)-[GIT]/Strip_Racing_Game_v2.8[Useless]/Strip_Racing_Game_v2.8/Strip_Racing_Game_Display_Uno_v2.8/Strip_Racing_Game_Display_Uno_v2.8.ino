/*
================================================================
    LED RACE — DISPLAY RECEIVER  (Arduino Uno)

    This sketch owns the 4-digit 7-segment display entirely.
    The ESP32 sends one character at a time over SoftwareSerial:

      Protocol:  P<pos><char>\n
      Examples:  "P1A\n"  → digit 1 = 'A'
                 "P20\n"  → digit 2 = '0'
                 "P3-\n"  → digit 3 = '-'

    pos  : '1' … '4'  (left to right)
    char : '0'-'9', 'A'-'Z', 'a'-'z', '-', ' '

    sevseg.refreshDisplay() must be called as often as possible
    (several thousand times per second) — this is why all other
    work is kept minimal and non-blocking.

    WIRING
    ──────
    Display digit commons  → Uno pins 2, 3, 4, 5
    Display segments A-G   → Uno pins 6, 7, 8, 9, 10, 11, 12
    Display decimal point  → Uno pin 13
    ESP32 TX (GPIO 33)     → Uno A0  (SoftwareSerial RX)
    Common GND between ESP32 and Uno

    LIBRARY NEEDED
    ──────────────
    SevSeg by Dean Reading  (Library Manager)

    CHANGE THESE if your hardware differs:
      HARDWARE_CONFIG  — COMMON_ANODE or COMMON_CATHODE
      digitPins[]      — which Uno pins drive each digit common
      segmentPins[]    — which Uno pins drive segments A, B, C, D, E, F, G, DP
      ESP_RX_PIN       — Uno pin receiving ESP32 TX
      BAUD_RATE        — must match ESP32 unoSerial.begin() rate
   ================================================================
*/

#include <SevSeg.h>
#include <SoftwareSerial.h>
#define ENABLE_SERIAL_TESTER 1
// ================================================================
//  ★  USER CONFIG
// ================================================================

#define HARDWARE_CONFIG   COMMON_ANODE   // Change to COMMON_CATHODE if needed
#define DISPLAY_BRIGHTNESS 90            // 0–100

// Digit select pins (left = digit 1, right = digit 4)
const byte DIGIT_PINS[4] = { 2, 3, 4, 5 };

// Segment pins in order: A, B, C, D, E, F, G, DP
const byte SEGMENT_PINS[8] = { 6, 7, 8, 9, 10, 11, 12, 13 };

// SoftwareSerial pin for receiving from ESP32
#define ESP_RX_PIN  A0    // Connect to ESP32 TX (GPIO 33)
#define ESP_TX_PIN  A1    // Unused — Uno does not send back to ESP32
#define BAUD_RATE   115200

// ================================================================
//  GLOBALS
// ================================================================
SevSeg sevseg;
SoftwareSerial espSerial(ESP_RX_PIN, ESP_TX_PIN);
char dispChars[5] = "----";     // Current 4-character display buffer, null-terminated for setChars()

// ================================================================
//  DISPLAY FUNCTIONS
// ================================================================

/**
   Push the current dispChars buffer to the SevSeg driver.
   Call this any time dispChars changes.
*/
void pushDisplay() {
  sevseg.setChars(dispChars);
}

/**
   Set one digit position (1-based) to a character and refresh.
   pos  : 1 … 4
   ch   : '0'-'9', 'A'-'Z', 'a'-'z', '-', ' ', etc.
*/
void setDigit(uint8_t pos, char ch) 
{
  if (pos < 1 || pos > 4) return;          // Ignore out-of-range positions
  dispChars[pos - 1] = ch;                  // Convert 1-based → 0-based index
  pushDisplay();
}

// ================================================================
//  SERIAL COMMAND PARSER
// ================================================================

/**
   Read and parse any waiting bytes from the ESP32.
   Expected format:  P<pos><char>\n
     'P'   — command prefix (validates this is a display command)
     <pos> — ASCII '1' to '4'
     <char>— any printable character
     '\n'  — end of command

   All parsing is non-blocking; incomplete commands wait in the buffer.
   Invalid or short packets are silently discarded.
*/
void readESPCommands()
{
  static char  buf[8];     // Accumulate bytes until newline
  static uint8_t idx = 0;
  while (espSerial.available()) 
  {
    char c = espSerial.read();
    if (c == '\n') 
    {
      // End of command — parse if length is exactly 3: P + pos + char
      if (idx == 3 && buf[0] == 'P') {
        uint8_t pos = buf[1] - '0';   // '1'-'4' → 1-4
        char    ch  = buf[2];
        setDigit(pos, ch);
      }
      idx = 0;   // Reset for next command
    } 
    else if (idx < (uint8_t)(sizeof(buf) - 1)) 
    {
      buf[idx++] = c;
    }
    // If buffer overflows (malformed packet), the next '\n' resets it
  }
}
void readSerialTester() 
{
  static char buf[8];
  static uint8_t idx = 0;
  while (Serial.available()) 
  {
    char c = Serial.read();
    if (c == '\n') {
      if (idx == 3 && buf[0] == 'P') 
      {
        uint8_t pos = buf[1] - '0';
        char ch = buf[2];
        setDigit(pos, ch);
      }
      idx = 0;
    } 
    else if (idx < sizeof(buf) - 1) 
    {
      buf[idx++] = c;
    }
  }
}
// ================================================================
//  SETUP
// ================================================================
void setup() 
{
  sevseg.begin(HARDWARE_CONFIG,4,(byte*)DIGIT_PINS,(byte*)SEGMENT_PINS,false,false,false,false);
  sevseg.setBrightness(DISPLAY_BRIGHTNESS);
  pushDisplay();
  espSerial.begin(BAUD_RATE);
  #if ENABLE_SERIAL_TESTER
    Serial.begin(9600);
    Serial.println("UNO DISPLAY TEST MODE READY");
    Serial.println("Send: P1A, P2B, P3-, P4C");
  #endif
}

// ================================================================
//  MAIN LOOP
// ================================================================
void loop() 
{
  readESPCommands();   // ESP32 inpu
  #if ENABLE_SERIAL_TESTER
      readSerialTester();  // USB Serial input
  #endif
      sevseg.refreshDisplay();
}
