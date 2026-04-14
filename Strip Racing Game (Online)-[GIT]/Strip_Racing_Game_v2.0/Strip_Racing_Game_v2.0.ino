/*  
 * ____                     _      ______ _____    _____
  / __ \                   | |    |  ____|  __ \  |  __ \               
 | |  | |_ __   ___ _ __   | |    | |__  | |  | | | |__) |__ _  ___ ___ 
 | |  | | '_ \ / _ \ '_ \  | |    |  __| | |  | | |  _  // _` |/ __/ _ \
 | |__| | |_) |  __/ | | | | |____| |____| |__| | | | \ \ (_| | (_|  __/
  \____/| .__/ \___|_| |_| |______|______|_____/  |_|  \_\__,_|\___\___|
        | |                                                             
        |_|          
 Open LED Race
 An minimalist cars race for LED strip  
  
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 3 of the License, or
 (at your option) any later version.

 
*/

                                                            
/*
 * ================================================================
 *  LED RACE  —  ESP32 Edition
 *  1-D LED-strip racing game, 2-4 players
 *
 *  Physical buttons on the main ESP32 +
 *  optional ESP-NOW remote for extra / wireless buttons.
 *
 *  Based on Open LED Race (GPL v3) by AYA Santa
 * ================================================================
 *
 *  WIRING (main ESP32)
 *  ───────────────────
 *  LED strip  DI  ←──[300-500Ω]──── GPIO PIN_LED
 *             +5V ←── 5V supply (separate, with 1000µF cap to GND)
 *             GND ←── common GND
 *
 *  Player buttons: one side → GPIO PIN_Px,  other side → GND
 *  Buzzer (passive): PIN_AUDIO → buzzer+ , buzzer– → GND
 *
 *  REMOTE ESP-NOW SENDER (second ESP32)
 *  ─────────────────────────────────────
 *  See the companion sketch at the bottom of this file (commented).
 *  The remote sends a single EspNowBtnPacket byte whose bits map to
 *  player buttons (bit 0 = P1, bit 1 = P2, …).
 *  Register the main ESP32's MAC address in the sender sketch.
 */

#include <Adafruit_NeoPixel.h>
#include <esp_now.h>
#include <WiFi.h>

// ================================================================
//  CONFIG  —  everything you need to change is right here
// ================================================================

#define NUM_PLAYERS     2       // Active players: 2, 3 or 4
#define TOTAL_LAPS      5       // Laps to win

#define NUM_LEDS        120     // LEDs on your strip
#define PIN_LED         2       // Strip data pin
#define PIN_AUDIO       4       // Passive buzzer pin

// Physical button pins on the main ESP32 (button → pin + GND)
#define PIN_P1          12
#define PIN_P2          14
// #define PIN_P3       26      // Uncomment for a 3rd local player
// #define PIN_P4       27      // Uncomment for a 4th local player

// Physics
#define ACCEL           0.20f   // Speed gained each time a button is pressed
#define FRICTION        0.015f  // Speed lost per tick  (fraction of current speed)
#define TICK_MS         5       // Main loop period in ms

// ================================================================
//  PLAYER COLORS  (0x00RRGGBB)  — change freely
// ================================================================
const uint32_t PLAYER_COLOR[4] = {
    0x00FF0000,   // P1 — Red
    0x000000FF,   // P2 — Blue
    0x0000FF00,   // P3 — Green
    0x00FF8000,   // P4 — Orange
};

// ================================================================
//  ESP-NOW PACKET  (shared struct between sender and receiver)
// ================================================================
typedef struct {
    uint8_t buttons;   // Bit field: bit 0 = P1, bit 1 = P2, …
} EspNowBtnPacket;

// ================================================================
//  INTERNAL TYPES
// ================================================================

/** All state for one player. */
struct PlayerState {
    float   speed;          // Current speed (LEDs/tick)
    float   dist;           // Total distance covered
    uint8_t laps;           // Completed laps
    bool    prevBtn;        // Button state last tick (edge detection)
    bool    remoteBtn;      // Latest state received via ESP-NOW
};

// ================================================================
//  GLOBALS
// ================================================================
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);

PlayerState players[NUM_PLAYERS];

// Non-blocking buzzer
int buzzerTicks = 0;

// Win jingle (Hz; 0 = rest)
const int WIN_NOTES[]   = {2637, 2637, 0, 2637, 0, 2093, 2637, 0, 3136};
const int WIN_NOTES_LEN = sizeof(WIN_NOTES) / sizeof(int);

// ================================================================
//  UTILITY HELPERS
// ================================================================

/**
 * Returns true if player i's button is currently pressed —
 * either the physical pin OR the ESP-NOW remote bit.
 */
bool readButton(int i) {
    bool physical = false;
    switch (i) {
        case 0: physical = (digitalRead(PIN_P1) == LOW); break;
        case 1: physical = (digitalRead(PIN_P2) == LOW); break;
        // case 2: physical = (digitalRead(PIN_P3) == LOW); break;
        // case 3: physical = (digitalRead(PIN_P4) == LOW); break;
        default: break;
    }
    return physical || players[i].remoteBtn;
}

/**
 * Scales the R, G, B channels of a 32-bit color by `factor` (0.0–1.0).
 * Used to draw a dim trail behind each car.
 */
uint32_t dimColor(uint32_t color, float factor) {
    uint8_t r = ((color >> 16) & 0xFF) * factor;
    uint8_t g = ((color >>  8) & 0xFF) * factor;
    uint8_t b = ( color        & 0xFF) * factor;
    return strip.Color(r, g, b);
}

/**
 * Start a non-blocking beep.
 * `ticks` is in loop ticks (1 tick ≈ TICK_MS ms).
 */
void beep(int freq, int ticks) {
    tone(PIN_AUDIO, freq);
    buzzerTicks = ticks;
}

/**
 * Call once per tick; automatically silences the buzzer when time is up.
 */
void tickBuzzer() {
    if (buzzerTicks > 0 && --buzzerTicks == 0) {
        noTone(PIN_AUDIO);
    }
}

// ================================================================
//  RENDERING
// ================================================================

/**
 * Draw all cars on the strip.
 * Each car is one bright pixel + one dim trailing pixel.
 * Later-drawn players paint over earlier ones on collision —
 * drawing order alternates each second so neither is always on top.
 */
void drawCars() {
    strip.clear();

    // Alternate draw order every second to be fair on overlaps
    bool flip = (millis() / 1000) & 1;

    for (int n = 0; n < NUM_PLAYERS; n++) {
        int i = flip ? (NUM_PLAYERS - 1 - n) : n;

        int pos   = (int)players[i].dist % NUM_LEDS;
        int trail = (pos - 1 + NUM_LEDS) % NUM_LEDS;

        strip.setPixelColor(pos,   PLAYER_COLOR[i]);
        strip.setPixelColor(trail, dimColor(PLAYER_COLOR[i], 0.15f));
    }
}

// ================================================================
//  ANIMATIONS
// ================================================================

/**
 * Traffic-light countdown: Green → Yellow → Red → GO!
 */
void countdown() {
    struct Step { uint32_t color; int freq; } steps[3] = {
        {0x0000FF00, 400 },   // Green
        {0x00FFFF00, 700 },   // Yellow
        {0x00FF0000, 1200},   // Red
    };

    strip.clear();
    strip.show();
    delay(600);

    for (int s = 0; s < 3; s++) {
        // Light two pixels as countdown "bulbs"
        strip.setPixelColor(8 + s * 2,     steps[s].color);
        strip.setPixelColor(8 + s * 2 + 1, steps[s].color);
        strip.show();
        tone(PIN_AUDIO, steps[s].freq);
        delay(900);
        noTone(PIN_AUDIO);
        strip.setPixelColor(8 + s * 2,     0);
        strip.setPixelColor(8 + s * 2 + 1, 0);
        strip.show();
        delay(100);
    }

    // GO! — short high beep
    tone(PIN_AUDIO, 2000);
    delay(150);
    noTone(PIN_AUDIO);
}

/**
 * Win animation:
 *   1. Flash all LEDs green three times.
 *   2. Fill the strip with the winner's color.
 *   3. Play the win jingle.
 */
void winAnimation(int winner) {
    // Flash green
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x0000FF00);
        strip.show();
        delay(300);
        strip.clear();
        strip.show();
        delay(200);
    }

    // Hold winner's color
    for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, PLAYER_COLOR[winner]);
    strip.show();

    // Jingle
    for (int n = 0; n < WIN_NOTES_LEN; n++) {
        if (WIN_NOTES[n]) tone(PIN_AUDIO, WIN_NOTES[n], 200);
        delay(230);
        noTone(PIN_AUDIO);
    }

    Serial.printf("[RACE] *** Player %d WINS! ***\n", winner + 1);
    delay(3000);
}

// ================================================================
//  GAME CONTROL
// ================================================================

/** Zero out all player state. */
void resetGame() {
    for (int i = 0; i < NUM_PLAYERS; i++) {
        players[i] = {0.0f, 0.0f, 0, false, false};
    }
}

/** Full reset + countdown, then the race begins. */
void startRace() {
    resetGame();
    countdown();
    Serial.println("[RACE] GO!");
}

// ================================================================
//  ESP-NOW CALLBACK
//
//  ESP32 Arduino core v3+ signature:
//    void cb(const esp_now_recv_info_t*, const uint8_t*, int)
//
//  If you are on core v2, replace the signature with:
//    void onRemoteBtn(const uint8_t *mac, const uint8_t *data, int len)
// ================================================================
void onRemoteBtn(const esp_now_recv_info_t *info,
                 const uint8_t             *data,
                 int                        len) {
    if (len < (int)sizeof(EspNowBtnPacket)) return;

    const EspNowBtnPacket *pkt = (const EspNowBtnPacket *)data;

    for (int i = 0; i < NUM_PLAYERS; i++) {
        players[i].remoteBtn = (pkt->buttons >> i) & 1;
    }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("[BOOT] LED Race starting…");

    // LED strip
    strip.begin();
    strip.clear();
    strip.show();

    // Button pins — internal pull-ups, active LOW
    pinMode(PIN_P1, INPUT_PULLUP);
    pinMode(PIN_P2, INPUT_PULLUP);
    // pinMode(PIN_P3, INPUT_PULLUP);
    // pinMode(PIN_P4, INPUT_PULLUP);

    // Buzzer
    pinMode(PIN_AUDIO, OUTPUT);

    // ESP-NOW (this ESP is the receiver; no peer registration needed)
    WiFi.mode(WIFI_STA);
    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(onRemoteBtn);
        Serial.print("[ESP-NOW] Ready. MAC: ");
        Serial.println(WiFi.macAddress());   // <-- paste this into the sender sketch
    } else {
        Serial.println("[ESP-NOW] Init failed — only physical buttons active");
    }

    startRace();
}

// ================================================================
//  MAIN LOOP
// ================================================================
void loop() {

    // ── Physics update for each player ──────────────────────────
    for (int i = 0; i < NUM_PLAYERS; i++) {

        bool btnNow = readButton(i);

        // Accelerate only on rising edge (new press, not held)
        if (btnNow && !players[i].prevBtn) {
            players[i].speed += ACCEL;
        }
        players[i].prevBtn = btnNow;

        // Friction slows the car each tick
        players[i].speed -= players[i].speed * FRICTION;
        if (players[i].speed < 0.0f) players[i].speed = 0.0f;

        // Move the car
        players[i].dist += players[i].speed;

        // ── Lap detection ────────────────────────────────────────
        uint32_t lapThreshold = (uint32_t)NUM_LEDS * (players[i].laps + 1);

        if ((uint32_t)players[i].dist >= lapThreshold) {
            players[i].laps++;
            beep(600 + i * 100, 3);   // Distinct pitch per player
            Serial.printf("[RACE] P%d — lap %d / %d\n",
                          i + 1, players[i].laps, TOTAL_LAPS);

            // ── Win check ────────────────────────────────────────
            if (players[i].laps >= TOTAL_LAPS) {
                drawCars();
                strip.show();
                winAnimation(i);
                startRace();
                return;   // Re-enter loop() cleanly after restart
            }
        }
    }

    // ── Render ──────────────────────────────────────────────────
    drawCars();
    strip.show();

    // ── Buzzer housekeeping ──────────────────────────────────────
    tickBuzzer();

    delay(TICK_MS);
}


// ================================================================
//  REMOTE SENDER SKETCH  (flash this on the second ESP32)
//  Copy this block into its own .ino file.
// ================================================================
/*

#include <esp_now.h>
#include <WiFi.h>

// ── CONFIG ──────────────────────────────────────────────────────
// Replace with the MAC address printed by the main ESP32 on boot:
uint8_t RECEIVER_MAC[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

// Remote button pins (add as many as you need)
#define REMOTE_PIN_P1   12    // Remote button for Player 1 → GND
#define REMOTE_PIN_P2   14    // Remote button for Player 2 → GND
// #define REMOTE_PIN_P3  26  // Uncomment to add P3 remote button
// ────────────────────────────────────────────────────────────────

typedef struct {
    uint8_t buttons;
} EspNowBtnPacket;

esp_now_peer_info_t peerInfo;
EspNowBtnPacket     packet;
uint8_t             lastPacket = 0xFF;   // Force first send

// ESP32 core v3+ send callback (for debugging)
void onSent(const uint8_t *mac, esp_now_send_status_t status) {
    // Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void setup() {
    Serial.begin(115200);

    pinMode(REMOTE_PIN_P1, INPUT_PULLUP);
    pinMode(REMOTE_PIN_P2, INPUT_PULLUP);
    // pinMode(REMOTE_PIN_P3, INPUT_PULLUP);

    WiFi.mode(WIFI_STA);
    esp_now_init();
    esp_now_register_send_cb(onSent);

    memcpy(peerInfo.peer_addr, RECEIVER_MAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    Serial.println("[REMOTE] Ready");
}

void loop() {
    // Build button bitmask
    uint8_t bits = 0;
    if (digitalRead(REMOTE_PIN_P1) == LOW) bits |= (1 << 0);
    if (digitalRead(REMOTE_PIN_P2) == LOW) bits |= (1 << 1);
    // if (digitalRead(REMOTE_PIN_P3) == LOW) bits |= (1 << 2);

    // Only transmit when state changes (reduces ESP-NOW traffic)
    if (bits != lastPacket) {
        packet.buttons = bits;
        esp_now_send(RECEIVER_MAC, (uint8_t *)&packet, sizeof(packet));
        lastPacket = bits;
    }

    delay(10);   // 100 Hz polling is plenty
}

*/
