// =============================================================
//  LED RACE  —  MAIN CONTROLLER  (ESP32 #1)   v9
//  Changes from v8:
//    • Bidirectional ESP-NOW  →  sends PktState to ESP2 remote
//    • Shared protocol block  (must be identical in ESP2)
//    • lastDisp[] tracks current display for remote mirroring
//    • sendStateToRemote() called at every state / sound event
//    • Serial test command  ('T' over USB at 115200)
//    • UNO_BAUD fixed to 9600 (was mismatched with Uno sketch)
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
#define TOTAL_LAPS            5
#define NUM_LEDS              120
#define PIN_LED               23
#define PIN_AUDIO             25

#define PIN_P1                16
#define PIN_P2                17
#define PIN_P3                5
#define PIN_P4                18

#define UNO_TX_PIN            33
#define UNO_RX_PIN            34      // Input-only, tied to nothing
#define UNO_BAUD              115200    // Must match Uno BAUD_RATE

#define ACCEL                 0.20f
#define FRICTION              0.015f
#define TICK_MS               5
#define RAINBOW_STEP          512
#define COUNTDOWN_BEAT_MS     1000UL
#define RACING_INACTIVITY_MS  60000UL

// ── MAC ADDRESSES ────────────────────────────────────────────
// Step 1: Flash ESP2 with DEBUG=1, open its Serial monitor → it prints its MAC
// Step 2: Paste that MAC here, reflash ESP1
uint8_t ESP2_MAC[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}; // ← REPLACE WITH ESP2 MAC

// ── PLAYER COLOURS  (0x00RRGGBB) ────────────────────────────
const uint32_t PLAYER_COLOR[4] = {
    0x00FF4500,   // P1 Orange-Red
    0x00007FFF,   // P2 Azure
    0x00FFFF00,   // P3 Yellow
    0x00FF00FF,   // P4 Pink
};

// ════════════════════════════════════════════════════════════
//  SHARED ESP-NOW PROTOCOL
//  !! This block must be character-for-character identical in ESP2 !!
// ════════════════════════════════════════════════════════════
#define PKT_BUTTONS  0x01   // direction: ESP2 → ESP1
#define PKT_STATE    0x02   // direction: ESP1 → ESP2

// soundEvent codes (field in PktState)
#define SND_NONE     0
#define SND_BEAT3    1   // Countdown digit 3
#define SND_BEAT2    2   // Countdown digit 2
#define SND_BEAT1    3   // Countdown digit 1
#define SND_GO       4   // GO!
#define SND_LAP      5   // Lap complete   → lapPlayer field valid
#define SND_WIN      6   // Race won       → winner    field valid
#define SND_DEFEAT   7   // Inactivity timeout

// gameState codes (field in PktState)
#define GS_IDLE      0
#define GS_COUNTDOWN 1
#define GS_RACING    2
#define GS_WIN       3
#define GS_DEFEAT    4

typedef struct __attribute__((packed)) {
    uint8_t type;      // PKT_BUTTONS
    uint8_t buttons;   // bit0=P1  bit1=P2  bit2=P3  bit3=P4
} PktButtons;

typedef struct __attribute__((packed)) {
    uint8_t type;       // PKT_STATE
    uint8_t gameState;  // GS_* code
    uint8_t winner;     // 0-3  (valid when soundEvent == SND_WIN)
    uint8_t soundEvent; // SND_* code
    uint8_t lapPlayer;  // 0-3  (valid when soundEvent == SND_LAP)
    char    disp[4];    // Current 4-char display content (NOT null-terminated)
} PktState;
// ════════════════════════════════════════════════════════════

// ── GAME STATE ───────────────────────────────────────────────
enum GameState { STATE_IDLE, STATE_COUNTDOWN, STATE_RACING, STATE_WIN };
GameState gameState = STATE_IDLE;

struct PlayerState {
    float   speed, dist;
    uint8_t laps;
    bool    prevBtn, remoteBtn;
};
PlayerState players[4];

// ── HARDWARE ─────────────────────────────────────────────────
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);
HardwareSerial    unoSerial(2);

// ── GLOBALS ──────────────────────────────────────────────────
int           buzzerTicks       = 0;
uint16_t      rainbowHue        = 0;
unsigned long lastButtonPressMs = 0;
unsigned long lastDisplayMs     = 0;
char          lastDisp[4]       = {'0','-','-','0'}; // Mirrors what Uno is showing

// ── ESP-NOW ──────────────────────────────────────────────────
esp_now_peer_info_t remotePeer;

// ── TUNES ────────────────────────────────────────────────────
const int WIN_NOTES[]      = {2637,2637,0,2637,0,2093,2637,0,3136,0,0,0,1568};
const int WIN_NOTES_LEN    = sizeof(WIN_NOTES)/sizeof(int);
const int DEFEAT_NOTES[]   = {523,494,466,440,415,392};
const int DEFEAT_NOTES_LEN = sizeof(DEFEAT_NOTES)/sizeof(int);


// ════════════════════════════════════════════════════════════
//  DISPLAY  — sends "P<pos><char>\n" to Uno over UART
// ════════════════════════════════════════════════════════════

void sendChar(uint8_t pos, char ch) {
    char buf[5];
    snprintf(buf, sizeof(buf), "P%d%c\n", pos, ch);
    unoSerial.print(buf);
    DBGF("[UNO TX] %s", buf);
}

// Sends all 4 digits and tracks them in lastDisp[] for remote mirroring
void sendDisplay(char d1, char d2, char d3, char d4) {
    lastDisp[0]=d1; lastDisp[1]=d2; lastDisp[2]=d3; lastDisp[3]=d4;
    DBGF("[DISP] %c%c%c%c\n", d1, d2, d3, d4);
    sendChar(1,d1); sendChar(2,d2); sendChar(3,d3); sendChar(4,d4);
}

void dispIdle()       { sendDisplay('0','-','-','0'); }
void dispCount(int n) { char d='0'+n; sendDisplay(' ',d,d,' '); }
void dispGo()         { sendDisplay(' ','G','o',' '); }
void dispOver()       { sendDisplay('O','v','E','r'); }
void dispDead()       { sendDisplay('d','E','A','d'); }
void dispLaps()       {
    sendDisplay(
        '0'+players[0].laps, '0'+players[1].laps,
        '0'+players[2].laps, '0'+players[3].laps);
}


// ════════════════════════════════════════════════════════════
//  ESP-NOW — SEND STATE TO REMOTE
// ════════════════════════════════════════════════════════════

void sendStateToRemote(uint8_t gs, uint8_t sound,
                       uint8_t winner=0, uint8_t lapPl=0)
{
    PktState pkt;
    pkt.type       = PKT_STATE;
    pkt.gameState  = gs;
    pkt.soundEvent = sound;
    pkt.winner     = winner;
    pkt.lapPlayer  = lapPl;
    memcpy(pkt.disp, lastDisp, 4);
    esp_now_send(ESP2_MAC, (uint8_t*)&pkt, sizeof(pkt));
    DBGF("[REMOTE TX] gs=%d snd=%d win=%d disp=%c%c%c%c\n",
         gs, sound, winner,
         pkt.disp[0], pkt.disp[1], pkt.disp[2], pkt.disp[3]);
}


// ════════════════════════════════════════════════════════════
//  UTILITIES
// ════════════════════════════════════════════════════════════

bool readButton(int i) {
    bool phys = false;
    switch(i) {
        case 0: phys = digitalRead(PIN_P1)==LOW; break;
        case 1: phys = digitalRead(PIN_P2)==LOW; break;
        case 2: phys = digitalRead(PIN_P3)==LOW; break;
        case 3: phys = digitalRead(PIN_P4)==LOW; break;
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
void tickBuzzer() { if(buzzerTicks>0 && --buzzerTicks==0) noTone(PIN_AUDIO); }


// ════════════════════════════════════════════════════════════
//  RENDERING
// ════════════════════════════════════════════════════════════

void drawCars() {
    strip.clear();
    bool flip = (millis()/1000) & 1;
    for(int n=0; n<4; n++) {
        int i   = flip ? (3-n) : n;
        int pos = (int)players[i].dist % NUM_LEDS;
        strip.setPixelColor(pos, PLAYER_COLOR[i]);
        strip.setPixelColor((pos-1+NUM_LEDS)%NUM_LEDS, dimColor(PLAYER_COLOR[i], 0.15f));
    }
}


// ════════════════════════════════════════════════════════════
//  COUNTDOWN
// ════════════════════════════════════════════════════════════

void symmetricSweep(uint32_t color, unsigned long sweepMs) {
    int half        = NUM_LEDS / 2;
    int delayPerLed = max(1, (int)(sweepMs / (half+1)));
    for(int step=0; step<=half; step++) {
        int lp = half - step;
        int rp = (half + step) % NUM_LEDS;
        strip.clear();
        strip.setPixelColor(lp, color);
        strip.setPixelColor(rp, color);
        if(lp+1<=half) strip.setPixelColor(lp+1, dimColor(color,0.35f));
        if(lp+2<=half) strip.setPixelColor(lp+2, dimColor(color,0.12f));
        strip.setPixelColor((rp-1+NUM_LEDS)%NUM_LEDS, dimColor(color,0.35f));
        strip.setPixelColor((rp-2+NUM_LEDS)%NUM_LEDS, dimColor(color,0.12f));
        strip.show();
        delay(delayPerLed);
    }
    strip.clear(); strip.show();
}

void runCountdown() {
    gameState = STATE_COUNTDOWN;

    struct Beat { int count; uint32_t color; int freq; uint8_t snd; };
    const Beat beats[3] = {
        {3, 0x00FF0000, 400, SND_BEAT3},
        {2, 0x00FFAA00, 600, SND_BEAT2},
        {1, 0x0000FF00, 900, SND_BEAT1},
    };

    strip.clear(); strip.show();
    delay(300);

    for(int b=0; b<3; b++) {
        unsigned long beatStart = millis();
        dispCount(beats[b].count);
        sendStateToRemote(GS_COUNTDOWN, beats[b].snd);     // ← notify remote
        tone(PIN_AUDIO, beats[b].freq); delay(160); noTone(PIN_AUDIO);
        long sweepMs = (long)COUNTDOWN_BEAT_MS - (long)(millis()-beatStart);
        if(sweepMs>0) symmetricSweep(beats[b].color, (unsigned long)sweepMs);
        while((long)(millis()-beatStart) < (long)COUNTDOWN_BEAT_MS);
        strip.clear(); strip.show();
    }

    // GO beat
    {
        unsigned long beatStart = millis();
        dispGo();
        sendStateToRemote(GS_COUNTDOWN, SND_GO);           // ← notify remote
        for(int i=0; i<NUM_LEDS; i++) strip.setPixelColor(i, 0x00FFFFFF);
        strip.show();
        tone(PIN_AUDIO, 1800); delay(160); noTone(PIN_AUDIO);
        while((long)(millis()-beatStart) < (long)COUNTDOWN_BEAT_MS);
        strip.clear(); strip.show();
    }

    for(int i=0; i<4; i++) players[i] = {0,0,0,false,false};
    lastButtonPressMs = lastDisplayMs = millis();
    dispLaps();
    sendStateToRemote(GS_RACING, SND_NONE);                // ← race start
    gameState = STATE_RACING;
}


// ════════════════════════════════════════════════════════════
//  WIN ANIMATION
// ════════════════════════════════════════════════════════════

void crossFadeStrip(uint32_t from, uint32_t to, int steps, int stepMs) {
    float fr=(from>>16)&0xFF, fg=(from>>8)&0xFF, fb=from&0xFF;
    float tr=(to>>16)  &0xFF, tg=(to>>8)  &0xFF, tb=to  &0xFF;
    for(int s=0; s<=steps; s++) {
        float t = (float)s/steps;
        for(int i=0; i<NUM_LEDS; i++)
            strip.setPixelColor(i, strip.Color(
                fr+(tr-fr)*t, fg+(tg-fg)*t, fb+(tb-fb)*t));
        strip.show(); delay(stepMs);
    }
}

void runWinAnimation(int winner) {
    gameState = STATE_WIN;
    noTone(PIN_AUDIO);
    drawCars(); strip.show(); delay(400);                            // freeze
    crossFadeStrip(0x00000000, 0x0000FF00, 80, 10);                 // → green
    delay(250);
    crossFadeStrip(0x0000FF00, PLAYER_COLOR[winner], 100, 12);      // → winner
    dispOver();
    sendStateToRemote(GS_WIN, SND_WIN, (uint8_t)winner);            // ← notify remote
    for(int n=0; n<WIN_NOTES_LEN; n++) {
        if(WIN_NOTES[n]) tone(PIN_AUDIO, WIN_NOTES[n], 200);
        delay(230); noTone(PIN_AUDIO);
    }
    delay(2000);
    enterIdle();
}


// ════════════════════════════════════════════════════════════
//  DEFEAT  (1-minute inactivity)
// ════════════════════════════════════════════════════════════

void playDefeatSequence() {
    noTone(PIN_AUDIO);
    dispDead();
    sendStateToRemote(GS_DEFEAT, SND_DEFEAT);                       // ← notify remote
    for(int i=0; i<NUM_LEDS; i++) strip.setPixelColor(i, 0x00200000);
    strip.show();
    for(int n=0; n<DEFEAT_NOTES_LEN; n++) {
        tone(PIN_AUDIO, DEFEAT_NOTES[n], 280); delay(310);
    }
    noTone(PIN_AUDIO);
    delay(600);
    strip.clear(); strip.show();
}


// ════════════════════════════════════════════════════════════
//  IDLE
// ════════════════════════════════════════════════════════════

void enterIdle() {
    gameState  = STATE_IDLE;
    rainbowHue = 0;
    dispIdle();
    sendStateToRemote(GS_IDLE, SND_NONE);                           // ← notify remote
    DBG("[IDLE] Waiting...");
}

void loopIdle() {
    strip.rainbow(rainbowHue); strip.show();
    rainbowHue += RAINBOW_STEP;
    delay(TICK_MS);
    for(int i=0; i<4; i++) {
        if(risingEdge(i)) { gameState = STATE_COUNTDOWN; return; }
    }
}


// ════════════════════════════════════════════════════════════
//  RACING LOOP
// ════════════════════════════════════════════════════════════

void loopRacing() {
    // Inactivity defeat
    if(millis()-lastButtonPressMs >= RACING_INACTIVITY_MS) {
        playDefeatSequence(); enterIdle(); return;
    }

    // Physics
    for(int i=0; i<4; i++) {
        if(risingEdge(i)) {
            players[i].speed  += ACCEL;
            lastButtonPressMs  = millis();
        }
        players[i].speed -= players[i].speed * FRICTION;
        if(players[i].speed < 0.0f) players[i].speed = 0.0f;
        players[i].dist += players[i].speed;

        uint32_t threshold = (uint32_t)NUM_LEDS * (players[i].laps+1);
        if((uint32_t)players[i].dist >= threshold) {
            players[i].laps++;
            beep(600+i*100, 3);
            DBGF("[RACE] P%d lap %d/%d\n", i+1, players[i].laps, TOTAL_LAPS);

            // Send lap event immediately with updated display
            dispLaps();
            sendStateToRemote(GS_RACING, SND_LAP, 0, (uint8_t)i);
            lastDisplayMs = millis();

            if(players[i].laps >= TOTAL_LAPS) {
                for(int j=0; j<4; j++) players[j].speed = 0.0f;
                runWinAnimation(i);
                return;
            }
        }
    }

    // Periodic display refresh + state broadcast
    if(millis()-lastDisplayMs >= 120UL) {
        dispLaps();
        sendStateToRemote(GS_RACING, SND_NONE);
        lastDisplayMs = millis();
    }

    drawCars(); strip.show();
    tickBuzzer();
    delay(TICK_MS);
}


// ════════════════════════════════════════════════════════════
//  SERIAL TEST   →  send 'T' + Enter from Serial Monitor
// ════════════════════════════════════════════════════════════

void runSerialTest() {
    Serial.println(F("=========== ESP1 TEST START ==========="));

    Serial.println(F("[1] Display: IDLE"));
    dispIdle(); delay(800);

    Serial.println(F("[2] Display: Countdown 3 → 1"));
    for(int n=3; n>=1; n--) { dispCount(n); delay(700); }

    Serial.println(F("[3] Display: GO"));
    dispGo(); delay(700);

    Serial.println(F("[4] Display: Laps 1-2-3-4"));
    players[0].laps=1; players[1].laps=2;
    players[2].laps=3; players[3].laps=4;
    dispLaps(); delay(800);
    players[0].laps=players[1].laps=players[2].laps=players[3].laps=0;

    Serial.println(F("[5] Display: OvEr"));
    dispOver(); delay(800);

    Serial.println(F("[6] Display: dEAd"));
    dispDead(); delay(800);

    Serial.println(F("[7] Audio: Win jingle"));
    for(int n=0; n<WIN_NOTES_LEN; n++) {
        if(WIN_NOTES[n]) tone(PIN_AUDIO, WIN_NOTES[n], 200);
        delay(230); noTone(PIN_AUDIO);
    }

    Serial.println(F("[8] Audio: Defeat sound"));
    for(int n=0; n<DEFEAT_NOTES_LEN; n++) {
        tone(PIN_AUDIO, DEFEAT_NOTES[n], 280); delay(310);
    }
    noTone(PIN_AUDIO);

    Serial.println(F("[9] Audio: Countdown beeps"));
    int bFreqs[] = {400,600,900,1800};
    for(int f : bFreqs) { tone(PIN_AUDIO,f); delay(160); noTone(PIN_AUDIO); delay(300); }

    Serial.println(F("[10] LED: Rainbow 2 sec"));
    for(int i=0; i<200; i++) {
        strip.rainbow(rainbowHue); strip.show();
        rainbowHue += RAINBOW_STEP; delay(TICK_MS);
    }

    Serial.println(F("[11] ESP-NOW: Send IDLE state to remote"));
    sendStateToRemote(GS_IDLE, SND_NONE);

    Serial.println(F("=========== ESP1 TEST END ==========="));
    dispIdle();
}


// ════════════════════════════════════════════════════════════
//  ESP-NOW CALLBACKS
// ════════════════════════════════════════════════════════════

// Receive buttons from ESP2
void onRemoteBtn(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if(len < 2 || data[0] != PKT_BUTTONS) return;
    const PktButtons *p = (const PktButtons*)data;
    for(int i=0; i<4; i++) players[i].remoteBtn = (p->buttons >> i) & 1;
}

// Send status callback (unused, required by API)
void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status){ }


// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════

void setup() {
    #if DEBUG
    Serial.begin(115200);
    Serial.println(F("[BOOT] LED Race v9 — ESP1 Main"));
    #endif

    strip.begin(); strip.clear(); strip.show();

    // Uno display UART (UART2 remapped to custom pins)
    unoSerial.begin(UNO_BAUD, SERIAL_8N1, UNO_RX_PIN, UNO_TX_PIN);
    delay(100);   // Let Uno finish booting before first display command

    pinMode(PIN_P1, INPUT_PULLUP); pinMode(PIN_P2, INPUT_PULLUP);
    pinMode(PIN_P3, INPUT_PULLUP); pinMode(PIN_P4, INPUT_PULLUP);
    pinMode(PIN_AUDIO, OUTPUT);

    // WiFi + ESP-NOW (bidirectional)
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if(esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(onRemoteBtn);
        esp_now_register_send_cb(onSent);
        // Register ESP2 as send peer
        memset(&remotePeer, 0, sizeof(remotePeer));
        memcpy(remotePeer.peer_addr, ESP2_MAC, 6);
        remotePeer.channel = 0;
        remotePeer.encrypt = false;
        esp_now_add_peer(&remotePeer);
        #if DEBUG
        Serial.print(F("[ESP-NOW] ESP1 MAC (give this to ESP2): "));
        Serial.println(WiFi.macAddress());
        Serial.print(F("[ESP-NOW] Targeting ESP2: "));
        for(int i=0; i<6; i++) {
            Serial.printf("%02X", ESP2_MAC[i]);
            if(i<5) Serial.print(':');
        }
        Serial.println();
        #endif
    }

    enterIdle();
}


// ════════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════════

void loop() {
    // Serial test: send 'T' + Enter from USB Serial Monitor
    #if DEBUG
    if(Serial.available() && Serial.read() == 'T') runSerialTest();
    #endif

    switch(gameState) {
        case STATE_IDLE:      loopIdle();      break;
        case STATE_COUNTDOWN: runCountdown();  break;
        case STATE_RACING:    loopRacing();    break;
        case STATE_WIN:       enterIdle();     break;
    }
}
