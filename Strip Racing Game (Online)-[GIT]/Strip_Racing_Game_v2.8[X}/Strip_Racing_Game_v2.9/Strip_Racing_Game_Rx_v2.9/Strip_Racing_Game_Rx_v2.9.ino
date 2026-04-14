// =============================================================
//  LED RACE  —  REMOTE CONTROLLER  (ESP32 #2)   v9
//
//  ► Sends button presses to ESP1 via ESP-NOW  (PktButtons)
//  ► Receives game state from ESP1 via ESP-NOW  (PktState)
//  ► Mirrors display to own Uno over UART  ("P<pos><char>\n")
//  ► 4× NeoPixel: rainbow idle, player colour racing, win/defeat
//  ► Buzzer: same countdown beeps, win jingle, defeat melody
//  ► Vibration motor: triggered on WIN, DEFEAT
//  ► Serial test: send 'T' + Enter from USB Serial Monitor
/*
ESP32 TX (33)
   |
  1kΩ
   |
   +----> Uno RX (D0)
   |
  2kΩ
   |
  GND
*/
// =============================================================
#include <Adafruit_NeoPixel.h>
#include <esp_now.h>
#include <WiFi.h>
 
#define DEBUG 1
#if DEBUG
  #define DBG(x)    Serial.println(x)
  #define DBGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGF(...)
#endif

// ── USER CONFIG ──────────────────────────────────────────────
// Remote button pins
#define REMOTE_PIN_P1    12
#define REMOTE_PIN_P2    14
#define REMOTE_PIN_P3    27
#define REMOTE_PIN_P4    26

// NeoPixel strip (4 LEDs, one per player)
#define PIN_NEOPIXEL     13
#define NUM_REMOTE_LEDS   4

// Buzzer
#define PIN_AUDIO        25

// Vibration motor  (drive via NPN transistor: base → pin, emitter → GND, collector → motor)
#define PIN_VIBRO         4

// Uno display UART
#define UNO_TX_PIN       33 
#define UNO_RX_PIN       34     // Input-only, tied to nothing
#define UNO_BAUD         15200  // Must match Uno BAUD_RATE

// Timing
#define TICK_MS           5
#define RAINBOW_STEP    512
#define LED_FLASH_TICKS  40   // 40 × 5 ms = 200 ms button-press flash

// ── MAC ADDRESS ──────────────────────────────────────────────
// Step 1: Flash ESP1 with DEBUG=1, open its Serial monitor → it prints its MAC
// Step 2: Paste that MAC here, reflash ESP2
uint8_t ESP1_MAC[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}; // ← REPLACE WITH ESP1 MAC

// ── PLAYER COLOURS  (0x00RRGGBB) — must match ESP1 ──────────
const uint32_t PLAYER_COLOR[4] = {
    0x00FF4500,   // P1 Orange-Red
    0x00007FFF,   // P2 Azure Blue
    0x00FFFF00,   // P3 Yellow
    0x00FF00FF,   // P4 Pink
};

// ════════════════════════════════════════════════════════════
//  SHARED ESP-NOW PROTOCOL
//  !! Must be character-for-character identical to ESP1 block !!
// ════════════════════════════════════════════════════════════
#define PKT_BUTTONS  0x01
#define PKT_STATE    0x02

#define SND_NONE     0
#define SND_BEAT3    1
#define SND_BEAT2    2
#define SND_BEAT1    3
#define SND_GO       4
#define SND_LAP      5
#define SND_WIN      6
#define SND_DEFEAT   7

#define GS_IDLE      0
#define GS_COUNTDOWN 1
#define GS_RACING    2
#define GS_WIN       3
#define GS_DEFEAT    4

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t buttons;
} PktButtons;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t gameState;
    uint8_t winner;
    uint8_t soundEvent;
    uint8_t lapPlayer;
    char    disp[4];
} PktState;
// ════════════════════════════════════════════════════════════

// ── LOCAL STATE ──────────────────────────────────────────────
enum RemoteState { RS_IDLE, RS_RACING, RS_WIN, RS_DEFEAT };
RemoteState localState = RS_IDLE;

// ── HARDWARE ─────────────────────────────────────────────────
Adafruit_NeoPixel strip(NUM_REMOTE_LEDS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
HardwareSerial    unoSerial(2);

// ── ESP-NOW ──────────────────────────────────────────────────
esp_now_peer_info_t mainPeer;
volatile bool  newPacket = false;
PktState       lastStatePacket;

// ── GLOBALS ──────────────────────────────────────────────────
uint16_t rainbowHue = 0;
int      buzzerTicks = 0;

// Button tracking for local LED flash
bool    remPrevBtn[4]  = {false,false,false,false};
uint8_t ledTimer[4]    = {0,0,0,0};

// Button state for ESP-NOW send (change-on-delta)
uint8_t lastSentBits = 0xFF;

// Pending animation flags (set in ESP-NOW callback, consumed in loop)
volatile bool    pendingWin    = false;
volatile bool    pendingDefeat = false;
volatile uint8_t pendingWinner = 0;

// ── TUNES  (identical to ESP1) ───────────────────────────────
const int WIN_NOTES[]      = {2637,2637,0,2637,0,2093,2637,0,3136,0,0,0,1568};
const int WIN_NOTES_LEN    = sizeof(WIN_NOTES)/sizeof(int);
const int DEFEAT_NOTES[]   = {523,494,466,440,415,392};
const int DEFEAT_NOTES_LEN = sizeof(DEFEAT_NOTES)/sizeof(int);


// ════════════════════════════════════════════════════════════
//  DISPLAY  — identical protocol to ESP1  "P<pos><char>\n"
// ════════════════════════════════════════════════════════════

void sendChar(uint8_t pos, char ch) {
    char buf[5];
    snprintf(buf, sizeof(buf), "P%d%c\n", pos, ch);
    unoSerial.print(buf);
    DBGF("[UNO TX] %s", buf);
}

void sendDisplay(char d1, char d2, char d3, char d4) {
    DBGF("[DISP] %c%c%c%c\n", d1, d2, d3, d4);
    sendChar(1,d1); sendChar(2,d2); sendChar(3,d3); sendChar(4,d4);
}

void dispIdle()       { sendDisplay('0','-','-','0'); }
void dispCount(int n) { char d='0'+n; sendDisplay(' ',d,d,' '); }
void dispGo()         { sendDisplay(' ','G','o',' '); }
void dispOver()       { sendDisplay('O','v','E','r'); }
void dispDead()       { sendDisplay('d','E','A','d'); }


// ════════════════════════════════════════════════════════════
//  AUDIO
// ════════════════════════════════════════════════════════════

void beep(int freq, int ticks) { tone(PIN_AUDIO, freq); buzzerTicks = ticks; }
void tickBuzzer() { if(buzzerTicks>0 && --buzzerTicks==0) noTone(PIN_AUDIO); }

void playWinJingle() {
    for(int n=0; n<WIN_NOTES_LEN; n++) {
        if(WIN_NOTES[n]) tone(PIN_AUDIO, WIN_NOTES[n], 200);
        delay(230); noTone(PIN_AUDIO);
    }
}

void playDefeatMelody() {
    for(int n=0; n<DEFEAT_NOTES_LEN; n++) {
        tone(PIN_AUDIO, DEFEAT_NOTES[n], 280); delay(310);
    }
    noTone(PIN_AUDIO);
}


// ════════════════════════════════════════════════════════════
//  VIBRATION
// ════════════════════════════════════════════════════════════

void vibrateOn()  { digitalWrite(PIN_VIBRO, HIGH); }
void vibrateOff() { digitalWrite(PIN_VIBRO, LOW);  }


// ════════════════════════════════════════════════════════════
//  NEOPIXEL HELPERS
// ════════════════════════════════════════════════════════════

uint32_t dimColor(uint32_t c, float f) {
    return strip.Color(((c>>16)&0xFF)*f, ((c>>8)&0xFF)*f, (c&0xFF)*f);
}

// Rainbow across 4 LEDs — advances rainbowHue
void drawRemoteRainbow() {
    for(int i=0; i<NUM_REMOTE_LEDS; i++) {
        uint16_t hue = rainbowHue + (uint16_t)(i * (65536 / NUM_REMOTE_LEDS));
        strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue)));
    }
    strip.show();
    rainbowHue += RAINBOW_STEP;
}

// Racing: dim player colour, bright on button-press flash
void drawPlayerLEDs() {
    for(int i=0; i<NUM_REMOTE_LEDS; i++) {
        float br = (ledTimer[i] > 0) ? 1.0f : 0.15f;
        strip.setPixelColor(i, dimColor(PLAYER_COLOR[i], br));
        if(ledTimer[i] > 0) ledTimer[i]--;
    }
    strip.show();
}

// All 4 LEDs crossfade from one solid colour to another (blocking)
void remoteCrossFade(uint32_t from, uint32_t to, int steps, int stepMs) {
    float fr=(from>>16)&0xFF, fg=(from>>8)&0xFF, fb=from&0xFF;
    float tr=(to>>16)  &0xFF, tg=(to>>8)  &0xFF, tb=to  &0xFF;
    for(int s=0; s<=steps; s++) {
        float t = (float)s / steps;
        uint32_t c = strip.Color(
            (uint8_t)(fr+(tr-fr)*t),
            (uint8_t)(fg+(tg-fg)*t),
            (uint8_t)(fb+(tb-fb)*t));
        for(int i=0; i<NUM_REMOTE_LEDS; i++) strip.setPixelColor(i, c);
        strip.show(); delay(stepMs);
    }
}

// Smooth fade from solid winColor into the live rainbow (blocking)
// Naturally transitions into loopRemoteIdle() rainbow without a snap
void fadeWinToRainbow(uint32_t winColor, int steps, int stepMs) {
    float wr=(winColor>>16)&0xFF, wg=(winColor>>8)&0xFF, wb=winColor&0xFF;
    for(int s=0; s<=steps; s++) {
        float t = (float)s / steps;
        for(int i=0; i<NUM_REMOTE_LEDS; i++) {
            uint16_t hue = rainbowHue + (uint16_t)(i * (65536 / NUM_REMOTE_LEDS));
            uint32_t rc  = strip.gamma32(strip.ColorHSV(hue));
            float rr=(rc>>16)&0xFF, rg=(rc>>8)&0xFF, rb=rc&0xFF;
            strip.setPixelColor(i, strip.Color(
                (uint8_t)(wr + (rr-wr)*t),
                (uint8_t)(wg + (rg-wg)*t),
                (uint8_t)(wb + (rb-wb)*t)));
        }
        rainbowHue += RAINBOW_STEP * 2;   // keep rainbow moving during fade
        strip.show(); delay(stepMs);
    }
}

// Quick blocking flash used for countdown beats
void countdownFlash(uint32_t color, int freq) {
    for(int i=0; i<NUM_REMOTE_LEDS; i++) strip.setPixelColor(i, color);
    strip.show();
    tone(PIN_AUDIO, freq); delay(160); noTone(PIN_AUDIO);
    delay(80);
    strip.clear(); strip.show();
}


// ════════════════════════════════════════════════════════════
//  WIN ANIMATION  (blocking ~7 s then fades into rainbow)
// ════════════════════════════════════════════════════════════

void runRemoteWin(uint8_t winner) {
    localState = RS_WIN;
    noTone(PIN_AUDIO); vibrateOff();

    // Freeze frame: show all 4 player colours briefly
    for(int i=0; i<NUM_REMOTE_LEDS; i++)
        strip.setPixelColor(i, dimColor(PLAYER_COLOR[i], 0.8f));
    strip.show(); delay(400);

    remoteCrossFade(0x00000000, 0x0000FF00, 80, 10);   // black → green  (~800 ms)
    delay(250);
    remoteCrossFade(0x0000FF00, PLAYER_COLOR[winner], 100, 12); // green → winner (~1200 ms)

    // Win jingle + vibration together
    vibrateOn();
    playWinJingle();    // ~3 s
    vibrateOff();

    delay(2000);

    // Smooth fade winner colour → rainbow  (no snap into idle)
    fadeWinToRainbow(PLAYER_COLOR[winner], 80, 15);    // ~1200 ms

    localState = RS_IDLE;
    dispIdle();
    DBG("[REMOTE] Win animation done → IDLE");
}


// ════════════════════════════════════════════════════════════
//  DEFEAT ANIMATION  (blocking ~3 s then fades into rainbow)
// ════════════════════════════════════════════════════════════

void runRemoteDefeat() {
    localState = RS_DEFEAT;
    noTone(PIN_AUDIO); vibrateOff();

    // Deep red on all LEDs
    for(int i=0; i<NUM_REMOTE_LEDS; i++) strip.setPixelColor(i, 0x00200000);
    strip.show();

    // Defeat melody + vibration together
    vibrateOn();
    playDefeatMelody();  // ~1.9 s
    vibrateOff();

    delay(600);

    // Smooth fade dark-red → rainbow
    fadeWinToRainbow(0x00200000, 60, 15);              // ~900 ms

    localState = RS_IDLE;
    dispIdle();
    DBG("[REMOTE] Defeat animation done → IDLE");
}


// ════════════════════════════════════════════════════════════
//  STATE LOOP HELPERS
// ════════════════════════════════════════════════════════════

void loopRemoteIdle() {
    drawRemoteRainbow();
    delay(TICK_MS);
}

void loopRemoteRacing() {
    // Local button detection for LED flash (send handled in main loop separately)
    for(int i=0; i<4; i++) {
        bool pins[4] = {
            digitalRead(REMOTE_PIN_P1)==LOW,
            digitalRead(REMOTE_PIN_P2)==LOW,
            digitalRead(REMOTE_PIN_P3)==LOW,
            digitalRead(REMOTE_PIN_P4)==LOW,
        };
        bool edge = pins[i] && !remPrevBtn[i];
        remPrevBtn[i] = pins[i];
        if(edge) ledTimer[i] = LED_FLASH_TICKS;
    }
    drawPlayerLEDs();
    tickBuzzer();
    delay(TICK_MS);
}


// ════════════════════════════════════════════════════════════
//  PROCESS INCOMING STATE PACKET FROM ESP1
// ════════════════════════════════════════════════════════════

void processStatePacket(const PktState *pkt) {
    // Always mirror display immediately
    sendDisplay(pkt->disp[0], pkt->disp[1], pkt->disp[2], pkt->disp[3]);

    // Handle sound events
    switch(pkt->soundEvent) {
        case SND_BEAT3:
            countdownFlash(0x00FF0000, 400);
            break;
        case SND_BEAT2:
            countdownFlash(0x00FFAA00, 600);
            break;
        case SND_BEAT1:
            countdownFlash(0x0000FF00, 900);
            break;
        case SND_GO:
            countdownFlash(0x00FFFFFF, 1800);
            localState = RS_RACING;
            memset(ledTimer, 0, sizeof(ledTimer));
            memset(remPrevBtn, 0, sizeof(remPrevBtn));
            break;
        case SND_LAP:
            // Non-blocking lap beep, distinct per player
            if(localState == RS_RACING)
                beep(600 + pkt->lapPlayer * 100, 3);
            break;
        case SND_WIN:
            // Guard against re-triggering while already animating
            if(localState != RS_WIN && localState != RS_DEFEAT) {
                pendingWinner = pkt->winner;
                pendingWin    = true;
            }
            break;
        case SND_DEFEAT:
            if(localState != RS_WIN && localState != RS_DEFEAT) {
                pendingDefeat = true;
            }
            break;
    }

    // State transitions (secondary safety, primary is via sound events above)
    if(pkt->soundEvent == SND_NONE) {
        if(pkt->gameState == GS_RACING && localState == RS_IDLE) {
            // Race started (missed GO packet)
            localState = RS_RACING;
            memset(ledTimer, 0, sizeof(ledTimer));
            memset(remPrevBtn, 0, sizeof(remPrevBtn));
        }
        if(pkt->gameState == GS_IDLE && localState == RS_RACING) {
            // Race ended without us catching WIN/DEFEAT
            localState = RS_IDLE;
            dispIdle();
        }
    }
}


// ════════════════════════════════════════════════════════════
//  BUTTON SEND TO ESP1
// ════════════════════════════════════════════════════════════

void sendButtonsToMain() {
    uint8_t bits = 0;
    if(digitalRead(REMOTE_PIN_P1)==LOW) bits |= (1<<0);
    if(digitalRead(REMOTE_PIN_P2)==LOW) bits |= (1<<1);
    if(digitalRead(REMOTE_PIN_P3)==LOW) bits |= (1<<2);
    if(digitalRead(REMOTE_PIN_P4)==LOW) bits |= (1<<3);

    if(bits != lastSentBits) {
        PktButtons pkt;
        pkt.type    = PKT_BUTTONS;
        pkt.buttons = bits;
        esp_now_send(ESP1_MAC, (uint8_t*)&pkt, sizeof(pkt));
        lastSentBits = bits;
        DBGF("[BTN TX] 0x%02X\n", bits);
    }
}


// ════════════════════════════════════════════════════════════
//  SERIAL TEST   →  send 'T' + Enter from USB Serial Monitor
// ════════════════════════════════════════════════════════════

void runSerialTest() {
    Serial.println(F("=========== ESP2 TEST START ==========="));

    Serial.println(F("[1] Display: IDLE"));
    dispIdle(); delay(800);

    Serial.println(F("[2] Display: Countdown 3 → 1"));
    for(int n=3; n>=1; n--) { dispCount(n); delay(700); }

    Serial.println(F("[3] Display: GO"));
    dispGo(); delay(700);

    Serial.println(F("[4] Display: OvEr"));
    dispOver(); delay(800);

    Serial.println(F("[5] Display: dEAd"));
    dispDead(); delay(800);

    Serial.println(F("[6] NeoPixel: Player colours"));
    for(int i=0; i<NUM_REMOTE_LEDS; i++) strip.setPixelColor(i, PLAYER_COLOR[i]);
    strip.show(); delay(1500);

    Serial.println(F("[7] NeoPixel: Rainbow 2 sec"));
    for(int i=0; i<200; i++) { drawRemoteRainbow(); delay(TICK_MS); }

    Serial.println(F("[8] NeoPixel: Win animation (P1)"));
    for(int i=0; i<NUM_REMOTE_LEDS; i++)
        strip.setPixelColor(i, dimColor(PLAYER_COLOR[i], 0.8f));
    strip.show(); delay(400);
    remoteCrossFade(0x00000000, 0x0000FF00, 40, 10);
    delay(150);
    remoteCrossFade(0x0000FF00, PLAYER_COLOR[0], 50, 12);
    delay(500);
    fadeWinToRainbow(PLAYER_COLOR[0], 60, 15);

    Serial.println(F("[9] Audio: Countdown beeps"));
    const int beatFreqs[] = {400,600,900,1800};
    for(int f : beatFreqs) {
        tone(PIN_AUDIO, f); delay(160); noTone(PIN_AUDIO); delay(350);
    }

    Serial.println(F("[10] Audio: Win jingle"));
    playWinJingle();

    Serial.println(F("[11] Audio: Defeat melody"));
    playDefeatMelody();

    Serial.println(F("[12] Vibration: 1 second pulse"));
    vibrateOn(); delay(1000); vibrateOff();

    Serial.println(F("[13] NeoPixel: Defeat animation"));
    for(int i=0; i<NUM_REMOTE_LEDS; i++) strip.setPixelColor(i, 0x00200000);
    strip.show(); delay(600);
    fadeWinToRainbow(0x00200000, 50, 15);

    Serial.println(F("=========== ESP2 TEST END ==========="));
    dispIdle();
}


// ════════════════════════════════════════════════════════════
//  ESP-NOW CALLBACKS
// ════════════════════════════════════════════════════════════

// Receive state from ESP1
void onStateReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if(len < 2 || data[0] != PKT_STATE) return;
    if(len < (int)sizeof(PktState)) return;
    memcpy((void*)&lastStatePacket, data, sizeof(PktState));
    newPacket = true;
}

 
void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status){ }

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════

void setup() {
    #if DEBUG
    Serial.begin(115200);
    Serial.println(F("[BOOT] LED Race v9 — ESP2 Remote"));
    #endif

    strip.begin(); strip.clear(); strip.show();

    // Uno display UART (same wiring as ESP1 board)
    unoSerial.begin(UNO_BAUD, SERIAL_8N1, UNO_RX_PIN, UNO_TX_PIN);
    delay(100);

    // Button inputs
    pinMode(REMOTE_PIN_P1, INPUT_PULLUP); pinMode(REMOTE_PIN_P2, INPUT_PULLUP);
    pinMode(REMOTE_PIN_P3, INPUT_PULLUP); pinMode(REMOTE_PIN_P4, INPUT_PULLUP);

    // Audio + vibration outputs
    pinMode(PIN_AUDIO, OUTPUT);
    pinMode(PIN_VIBRO, OUTPUT);
    vibrateOff();

    // WiFi + ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if(esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(onStateReceived);
        esp_now_register_send_cb(onSent);
        // Register ESP1 as send peer
        memset(&mainPeer, 0, sizeof(mainPeer));
        memcpy(mainPeer.peer_addr, ESP1_MAC, 6);
        mainPeer.channel = 0;
        mainPeer.encrypt = false;
        esp_now_add_peer(&mainPeer);
        #if DEBUG
        Serial.print(F("[ESP-NOW] ESP2 MAC (give this to ESP1): "));
        Serial.println(WiFi.macAddress());
        Serial.print(F("[ESP-NOW] Targeting ESP1: "));
        for(int i=0; i<6; i++) {
            Serial.printf("%02X", ESP1_MAC[i]);
            if(i<5) Serial.print(':');
        }
        Serial.println();
        #endif
    }

    localState = RS_IDLE;
    dispIdle();
    DBG("[IDLE] Remote ready.");
}


// ════════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════════

void loop() {
    // ── Serial test  ─────────────────────────────────────────
    #if DEBUG
    if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    Serial.print("Got full input: ");
    Serial.println(input);

    if (input == "T" || input == "t") {
        runSerialTest();
    }

    int value = input.toInt();  // works for 1, 2, 10, 123 etc.

    Serial.print("Parsed int: ");
    Serial.println(value);
}
    #endif

    // ── Process pending state packet from ESP1  ───────────────
    // Consume before checking pending flags so sound events set by
    // processStatePacket are handled in this same iteration.
    if(newPacket) {
        newPacket = false;
        processStatePacket((const PktState*)&lastStatePacket);
    }

    // ── Blocking animations (race is paused; buttons don't matter)
    if(pendingWin) {
        pendingWin = false;
        runRemoteWin(pendingWinner);
        return;   // restart loop so we pick up the fresh state cleanly
    }
    if(pendingDefeat) {
        pendingDefeat = false;
        runRemoteDefeat();
        return;
    }

    // ── Send buttons to ESP1 every loop tick  ────────────────
    sendButtonsToMain();

    // ── State-specific rendering  ─────────────────────────────
    switch(localState) {
        case RS_IDLE:    loopRemoteIdle();    break;
        case RS_RACING:  loopRemoteRacing();  break;
        default: break;
    }
}
