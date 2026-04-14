/*
 ============================================================
 
  | | | |  / \  / ___|| | | |_   _| / \   / ___|
  | |_| | / _ \ \___ \| |_| | | |  / _ \ | |  _ 
  |  _  |/ ___ \ ___) |  _  | | | / ___ \| |_| |
  |_| |_/_/   \_\____/|_| |_| |_|/_/   \_\\____|
 
 
  Name: Aniket Chowdhury [Hashtag]
  Email: micro.aniket@gmail.com
  GitHub: https://github.com/itzzhashtag
  Instagram: https://instagram.com/itzz_hashtag
  LinkedIn: https://www.linkedin.com/in/itzz-hashtag/
  
 Wokwi Simulation - "https://wokwi.com/projects/461126131423519745"
 ============================================================
 
// ================================================================
//  1D RGB Led - Strip Racing 
//  Arduino Uno Edition  v2.7.2
//  A button-mashing LED strip racing game for 2 players!
//  Press your button as fast as you can to move your car around
//  the LED strip. First to complete all the laps wins!
//
//  Hardware you need:
//    ? Arduino Uno
//    ? WS2812B LED strip  (any length, see NUM_LEDS below)
//    ? TM1637 4-digit 7-segment display (lap counter)
//    ? 2 push buttons (one per player)
//    ? 1 passive buzzer / speaker (for sounds)
//    ? 5V power supply for the LED strip (separate from Uno!)
//
//  Wiring (Uno pins ? component):
//    Pin 6  ? NeoPixel data-in (+ 330? resistor on the wire)
//    Pin 8  ? Buzzer positive (+)
//    Pin 2  ? Player 1 button  (other leg to GND)
//    Pin 3  ? Player 2 button  (other leg to GND)
//    Pin 10 ? TM1637 CLK
//    Pin 11 ? TM1637 DIO
//    5V/GND ? TM1637 VCC/GND
//
//  Libraries needed (install via Arduino Library Manager):
//    ? Adafruit NeoPixel   (by Adafruit)
//    ? TM1637Display       (by Avishay Orpaz)
//
// ================================================================
*/


#include <Adafruit_NeoPixel.h>
#include <TM1637Display.h>


// ================================================================
//  ???  SETTINGS ? change these to customise your game  ???
// ================================================================


// ?? Track ????????????????????????????????????????????????????????
// How many LEDs are in your strip?
// Uno RAM tip: each LED uses 3 bytes. 60 LEDs = 180 bytes (safe).
//              Max comfortable on Uno ? 150 LEDs.
#define NUM_LEDS        120


// Which LED (by index) is the start / finish line?
// 0 = the very first LED in the strip.
#define LED_STARTLINE   0


// ?? Players ??????????????????????????????????????????????????????
#define NUM_PLAYERS     2   // ? EDIT: 2 players (Uno only has 2 button pins set up)
#define TOTAL_LAPS      2   // ? EDIT: how many laps before someone wins


// ?? Pins ?????????????????????????????????????????????????????????
#define PIN_LED         6   // ? EDIT: NeoPixel data pin
#define PIN_AUDIO       8   // ? EDIT: buzzer pin
#define PIN_P1          2   // ? EDIT: Player 1 button
#define PIN_P2          3   // ? EDIT: Player 2 button


#define PIN_TM_CLK     10   // ? EDIT: TM1637 clock pin
#define PIN_TM_DIO     11   // ? EDIT: TM1637 data pin
#define TM_BRIGHTNESS   7   // ? EDIT: display brightness 0(dim) ? 7(bright)
#define SR_BRIGNTNESS 200   // ? EDIT: 0?255 strip brightness


// ?? Speed & feel ?????????????????????????????????????????????????
// Every time you press the button your car gets faster by ACCEL.
// Every tick (TICK_MS ms) your car loses FRICTION fraction of its speed.
// Bigger ACCEL  = snappier / faster cars.
// Bigger FRICTION = cars slow down more quickly between presses.
#define ACCEL          0.20f   // ? EDIT: speed boost per button press
#define FRICTION       0.015f  // ? EDIT: 0.01 = slippery | 0.05 = sticky
#define TICK_MS        5       // ? EDIT: ms per physics/render frame


// ?? Countdown ????????????????????????????????????????????????????
// Each beat of the 3-2-1-GO countdown lasts exactly this many ms.
// The sweep animation auto-scales to always fit inside one beat,
// no matter how long your LED strip is.
#define COUNTDOWN_BEAT_MS        1000UL  // ? EDIT: beat length (1000 = 1 second)
#define COUNTDOWN_SWEEP_FRAMES   35      // ? EDIT: animation smoothness (20?50)


// ?? Idle rainbow ?????????????????????????????????????????????????
// While waiting for a player to press, the strip cycles rainbow colours.
// Larger value = faster cycling.
#define RAINBOW_STEP    512   // ? EDIT


// ?? Inactivity timeout ???????????????????????????????????????????
// If nobody presses a button for this many ms during a race,
// the game gives up and plays a sad tune, then returns to idle.
#define RACING_INACTIVITY_MS  60000UL   // ? EDIT: 60 000 ms = 60 seconds


// ?? Start-line width ?????????????????????????????????????????????
// ? EDIT: how many pixels wide the start/finish marker is.
//   1 = single pixel, 3 = centre + one either side (default), 5 = wider
#define STARTLINE_WIDTH  3


// ?? Player colours ? format 0x00RRGGBB ???????????????????????????
const uint32_t PLAYER_COLOR[2] = {
    0x00FF0000,   // P1 ? Red
    0x000000FF,   // P2 ? Blue
};


// ?? Audio sequences ??????????????????????????????????????????????
// Each number is a frequency in Hz. 0 = silence (rest).
// delay between notes is set in the playWinJingle() function below.
const int WIN_NOTES[]      = { 0,2637,2637,0,2637,0,2093,2637,0,3136,0,0,0,1568 };
const int WIN_NOTES_LEN    = sizeof(WIN_NOTES)    / sizeof(int);


const int DEFEAT_NOTES[]   = { 523, 494, 466, 440, 415, 392 };
const int DEFEAT_NOTES_LEN = sizeof(DEFEAT_NOTES) / sizeof(int);


// ================================================================
//  HARDWARE OBJECTS
//  These lines create the software "driver" for each component.
// ================================================================


// NeoPixel strip driver
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);


// TM1637 display driver
TM1637Display tmDisplay(PIN_TM_CLK, PIN_TM_DIO);



// ================================================================
//  GAME STATE
//  The game is always in one of four states.
// ================================================================


// An "enum" is like a named list of options.
// gameState holds which mode we're currently in.
enum GameState {
    STATE_IDLE,        // waiting at the rainbow screen
    STATE_COUNTDOWN,   // 3-2-1-GO animation playing
    STATE_RACING,      // race is live!
    STATE_WIN          // winner animation playing
};
GameState gameState = STATE_IDLE;   // start in idle


// All the info we need per player, bundled into a struct
struct PlayerState {
    float   speed;      // current movement speed (LEDs per tick)
    float   dist;       // total distance travelled (in LED widths)
    uint8_t laps;       // laps completed so far
    bool    prevBtn;    // button state last tick (used for edge detection)
};
PlayerState players[NUM_PLAYERS];   // one PlayerState for each player


// ================================================================
//  GLOBAL VARIABLES
// ================================================================
int           buzzerTicks       = 0;
uint16_t      rainbowHue        = 0;
unsigned long lastButtonPressMs = 0;
bool          displayDirty      = false;


static const uint8_t SEG_BLANK   = 0b00000000;
static const uint8_t SEG_DASH    = 0b01000000;
static const uint8_t SEG_DEAD[4] = { 0x5E, 0x79, 0x77, 0x5E };
static const uint8_t SEG_GO[4]   = { SEG_BLANK, 0x3D, 0x5C, SEG_BLANK };


// ================================================================
//  SECTION: UTILITIES
// ================================================================


bool readButton(int i) {
    if (i == 0) return digitalRead(PIN_P1) == LOW;
    if (i == 1) return digitalRead(PIN_P2) == LOW;
    return false;
}


bool risingEdge(int i) {
    bool now  = readButton(i);
    bool edge = now && !players[i].prevBtn;
    players[i].prevBtn = now;
    return edge;
}


uint32_t dimColor(uint32_t c, float f) {
    uint8_t r = ((c >> 16) & 0xFF) * f;
    uint8_t g = ((c >>  8) & 0xFF) * f;
    uint8_t b = ( c        & 0xFF) * f;
    return strip.Color(r, g, b);
}


void beep(int freq, int ticks) { tone(PIN_AUDIO, freq); buzzerTicks = ticks; }


void tickBuzzer() {
    if (buzzerTicks > 0) {
        buzzerTicks--;
        if (buzzerTicks == 0) noTone(PIN_AUDIO);
    }
}


// ================================================================
//  SECTION: TM1637 DISPLAY
// ================================================================


void updateDisplay() {
    uint8_t segs[4];
    segs[0] = tmDisplay.encodeDigit(players[0].laps);
    segs[1] = SEG_DASH;
    segs[2] = SEG_DASH;
    segs[3] = tmDisplay.encodeDigit(players[1].laps);
    tmDisplay.setSegments(segs, 4, 0);
}


void displayIdle() {
    uint8_t s[4] = { SEG_DASH, SEG_DASH, SEG_DASH, SEG_DASH };
    tmDisplay.setSegments(s, 4, 0);
}


void displayCount(int n) {
    uint8_t d = tmDisplay.encodeDigit(n);
    uint8_t s[4] = { SEG_BLANK, d, d, SEG_BLANK };
    tmDisplay.setSegments(s, 4, 0);
}


void displayWinner(int w) {
    uint8_t segs[4] = { SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK };
    uint8_t pos = (w == 1) ? 3 : 0;
    for (int f = 0; f < 5; f++) {
        segs[pos] = tmDisplay.encodeDigit(players[w].laps);
        tmDisplay.setSegments(segs, 4, 0);
        delay(300);
        segs[pos] = SEG_BLANK;
        tmDisplay.setSegments(segs, 4, 0);
        delay(200);
    }
}


// ================================================================
//  SECTION: LED RENDERING
// ================================================================


/*
 * drawStartLine()
 * Paints STARTLINE_WIDTH white pixels centred on LED_STARTLINE.
 * The half-width is calculated so it's always symmetric.
 *
 * Example with STARTLINE_WIDTH 3, LED_STARTLINE 0:
 *   pixels painted: NUM_LEDS-1 (dim), 0 (bright), 1 (dim)
 *
 * ? EDIT STARTLINE_WIDTH at the top to make it wider or narrower.
 * ? EDIT the colours below:
 *   centre pixel  ? 0x00FFFFFF (white)
 *   side pixels   ? 0x00999999 (dimmer white)
 */
void drawStartLine() {
    int half = STARTLINE_WIDTH / 2;  // e.g. 3/2 = 1


    for (int offset = -half; offset <= half; offset++) {
        int pos = (LED_STARTLINE + offset + NUM_LEDS) % NUM_LEDS;


        if (offset == 0) {
            // Centre pixel ? full white
            strip.setPixelColor(pos, 0x00FFFFFF);
        } else {
            // Side pixels ? dimmer white so the centre stands out
            strip.setPixelColor(pos, 0x00999999);
        }
    }
}


/*
 * drawCars()
 * Clears the strip, draws the start-line marker, then draws each
 * car (bright head + dim tail). Cars drawn on top will override the
 * start-line pixels when crossing ? that momentary flash looks great.
 */
void drawCars() {
    strip.clear();


    // ?? ? Start / finish line ????????????????????????????????????
    drawStartLine();


    // ?? ? Cars (drawn on top of start-line pixels) ???????????????
    bool flip = (millis() / 1000) & 1;
    for (int n = 0; n < NUM_PLAYERS; n++) {
        int i   = flip ? (NUM_PLAYERS - 1 - n) : n;
        int pos = (int)players[i].dist % NUM_LEDS;
        strip.setPixelColor(pos, PLAYER_COLOR[i]);
        int tail = (pos - 1 + NUM_LEDS) % NUM_LEDS;
        strip.setPixelColor(tail, dimColor(PLAYER_COLOR[i], 0.15f));
    }
}


// ================================================================
//  SECTION: COUNTDOWN
// ================================================================


void symmetricSweep(uint32_t color, unsigned long sweepMs) {
    int  half    = NUM_LEDS / 2;
    long frameMs = (long)sweepMs / COUNTDOWN_SWEEP_FRAMES;
    if (frameMs < 1) frameMs = 1;


    for (int f = 0; f < COUNTDOWN_SWEEP_FRAMES; f++) {
        unsigned long frameStart = millis();


        int progress = (int)((long)f * half / COUNTDOWN_SWEEP_FRAMES);
        int lp = half - progress;
        int rp = (half + progress) % NUM_LEDS;


        strip.clear();
        strip.setPixelColor(lp, color);
        strip.setPixelColor(rp, color);
        strip.setPixelColor((lp + 1) % NUM_LEDS,            dimColor(color, 0.35f));
        strip.setPixelColor((lp + 2) % NUM_LEDS,            dimColor(color, 0.12f));
        strip.setPixelColor((rp - 1 + NUM_LEDS) % NUM_LEDS, dimColor(color, 0.35f));
        strip.setPixelColor((rp - 2 + NUM_LEDS) % NUM_LEDS, dimColor(color, 0.12f));
        strip.show();


        while ((long)(millis() - frameStart) < frameMs) {}
    }
    strip.clear();
    strip.show();
}


void runCountdown() {
    gameState = STATE_COUNTDOWN;


    struct Beat { int count; uint32_t color; int freq; };
    Beat beats[3] = {
        { 3, 0x00FF0000, 400 },
        { 2, 0x00FFAA00, 600 },
        { 1, 0x0000FF00, 900 },
    };


    strip.clear(); strip.show();
    delay(300);


    for (int b = 0; b < 3; b++) {
        unsigned long beatStart = millis();
        displayCount(beats[b].count);
        tone(PIN_AUDIO, beats[b].freq);
        delay(160);
        noTone(PIN_AUDIO);


        long elapsed = (long)(millis() - beatStart);
        long sweepMs = (long)COUNTDOWN_BEAT_MS - elapsed;
        if (sweepMs > 0) symmetricSweep(beats[b].color, (unsigned long)sweepMs);


        while ((long)(millis() - beatStart) < (long)COUNTDOWN_BEAT_MS) {}
        strip.clear(); strip.show();
    }


    {
        unsigned long beatStart = millis();
        tmDisplay.setSegments(SEG_GO, 4, 0);
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x00FFFFFF);
        strip.show();
        tone(PIN_AUDIO, 1800);
        delay(160);
        noTone(PIN_AUDIO);
        while ((long)(millis() - beatStart) < (long)COUNTDOWN_BEAT_MS) {}
        strip.clear(); strip.show();
    }


    Serial.println("GO!");
    for (int i = 0; i < NUM_PLAYERS; i++) {
        players[i].speed = 0; players[i].dist = 0;
        players[i].laps  = 0; players[i].prevBtn = false;
    }
    lastButtonPressMs = millis();
    updateDisplay();
    gameState = STATE_RACING;
}


// ================================================================
//  SECTION: WIN ANIMATION
// ================================================================


void crossFadeStrip(uint32_t fromColor, uint32_t toColor,
                    int steps, int stepDelayMs) {
    float fr = (fromColor >> 16) & 0xFF, fg = (fromColor >> 8) & 0xFF, fb = fromColor & 0xFF;
    float tr = (toColor   >> 16) & 0xFF, tg = (toColor   >> 8) & 0xFF, tb = toColor  & 0xFF;
    for (int s = 0; s <= steps; s++) {
        float t = (float)s / steps;
        uint8_t r = fr+(tr-fr)*t, g = fg+(tg-fg)*t, b = fb+(tb-fb)*t;
        for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(r,g,b));
        strip.show();
        delay(stepDelayMs);
    }
}


/*
 * fadeInToIdle()
 * Bridges the gap between win animation and idle rainbow.
 *
 * How it works:
 *   - Strip pixels are already all black after the fade-out.
 *   - We drop brightness to 0, fill rainbow colours into the buffer,
 *     then ramp brightness from 0 ? normal while advancing the hue.
 *   - The player sees a dark strip that gently brightens into the
 *     familiar rainbow ? no hard cut.
 *
 * ? EDIT fadeSteps to make the fade-in shorter (fewer steps) or
 *   longer (more steps). Each step takes ~15 ms ? 50 steps ? 750 ms.
 */
void fadeInToIdle() {
    gameState    = STATE_IDLE;
    rainbowHue   = 0;
    displayDirty = false;
    displayIdle();


    uint8_t normalBrightness = strip.getBrightness(); // remember what we had
    strip.setBrightness(0);                           // start invisible


    const int fadeSteps = 50;  // ? EDIT: ramp-up steps (each ~15 ms)


    for (int b = 0; b <= fadeSteps; b++) {
        // Map 0?fadeSteps ? 0?normalBrightness
        uint8_t brt = (uint8_t)((long)b * normalBrightness / fadeSteps);
        strip.setBrightness(brt);
        strip.rainbow(rainbowHue);
        rainbowHue += RAINBOW_STEP;
        strip.show();
        delay(15);
    }


    strip.setBrightness(normalBrightness); // restore for normal idle use
    Serial.println("Waiting for player to press a button...");
}


/*
 * runWinAnimation(winner)
 * Sequence:
 *   1. Briefly freeze cars at finish positions
 *   2. Fade strip ? green
 *   3. Cross-fade green ? winner's colour
 *   4. Display blink + win jingle play simultaneously
 *   5. Fade winner colour ? black
 *   6. Fade rainbow in from black  (no hard cut!)
 *
 * Steps 4 uses millis() timing so the display blinks and the jingle
 * notes advance independently ? no blocking delay needed.
 * Total jingle time  : 13 notes × 230 ms = ~2990 ms
 * Total display blink: 5 × (300 on + 200 off) = 2500 ms
 * ? jingle finishes last, display stays lit its full run naturally.
 */
void runWinAnimation(int winner) {
    gameState = STATE_WIN;
    noTone(PIN_AUDIO);


    Serial.print("WINNER: Player ");
    Serial.println(winner + 1);


    // Step 1 ? freeze
    drawCars(); strip.show(); delay(400);


    // Step 2 & 3 ? colour reveal
    crossFadeStrip(0x00000000, 0x0000FF00, 80, 10);
    delay(250);
    crossFadeStrip(0x0000FF00, PLAYER_COLOR[winner], 100, 12);


    // Step 4 ? display blink AND jingle together via millis() loop
    {
        uint8_t segs[4]  = { SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK };
        uint8_t pos      = (winner == 1) ? 3 : 0;
        int     noteIdx  = 0;
        bool    segOn    = false;


        unsigned long noteTimer = millis();
        unsigned long segTimer  = millis();


        // Fire the very first note and turn the digit on immediately
        if (WIN_NOTES[0] > 0) tone(PIN_AUDIO, WIN_NOTES[0], 200);
        noteIdx = 1;
        segs[pos] = tmDisplay.encodeDigit(players[winner].laps);
        tmDisplay.setSegments(segs, 4, 0);
        segOn = true;


        // Run until every note slot (230 ms each) has elapsed
        while (noteIdx <= WIN_NOTES_LEN) {
            unsigned long now = millis();


            // ?? advance jingle ??????????????????????????????????
            if (now - noteTimer >= 230UL) {
                noteTimer = now;
                noTone(PIN_AUDIO);
                if (noteIdx < WIN_NOTES_LEN) {
                    if (WIN_NOTES[noteIdx] > 0) tone(PIN_AUDIO, WIN_NOTES[noteIdx], 200);
                }
                noteIdx++;
            }


            // ?? blink display digit ?????????????????????????????
            unsigned long segInterval = segOn ? 300UL : 200UL;
            if (now - segTimer >= segInterval) {
                segTimer = now;
                segOn    = !segOn;
                segs[pos] = segOn ? tmDisplay.encodeDigit(players[winner].laps) : SEG_BLANK;
                tmDisplay.setSegments(segs, 4, 0);
            }
        }
        noTone(PIN_AUDIO);
    }


    delay(800); // short pause to let the last note breathe


    // Step 5 ? fade winner colour ? black
    crossFadeStrip(PLAYER_COLOR[winner], 0x00000000, 60, 15);  // ~900 ms


    // Step 6 ? rainbow fades in from black (no abrupt jump)
    fadeInToIdle();
}


// ================================================================
//  SECTION: DEFEAT
// ================================================================


void playDefeatSequence() {
    noTone(PIN_AUDIO);
    tmDisplay.setSegments(SEG_DEAD, 4, 0);
    for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0x00200000);
    strip.show();
    for (int n = 0; n < DEFEAT_NOTES_LEN; n++) {
        tone(PIN_AUDIO, DEFEAT_NOTES[n], 280);
        delay(310);
    }
    noTone(PIN_AUDIO);
    delay(600);
    strip.clear(); strip.show();
}


// ================================================================
//  SECTION: IDLE
// ================================================================


void enterIdle() {
    gameState    = STATE_IDLE;
    rainbowHue   = 0;
    displayDirty = false;
    displayIdle();
    Serial.println("Waiting for player to press a button...");
}


void loopIdle() {
    strip.rainbow(rainbowHue);
    strip.show();
    rainbowHue += RAINBOW_STEP;
    delay(TICK_MS);


    for (int i = 0; i < NUM_PLAYERS; i++) {
        if (risingEdge(i)) {
            Serial.print("Player ");
            Serial.print(i + 1);
            Serial.println(" pressed ? starting countdown!");
            gameState = STATE_COUNTDOWN;
            return;
        }
    }
}


// ================================================================
//  SECTION: RACING
// ================================================================


void loopRacing() {
    if (millis() - lastButtonPressMs >= RACING_INACTIVITY_MS) {
        Serial.println("Inactivity timeout ? game over");
        playDefeatSequence();
        enterIdle();
        return;
    }


    for (int i = 0; i < NUM_PLAYERS; i++) {
        if (risingEdge(i)) {
            players[i].speed  += ACCEL;
            lastButtonPressMs  = millis();
        }


        players[i].speed -= players[i].speed * FRICTION;
        if (players[i].speed < 0.0f) players[i].speed = 0.0f;
        players[i].dist  += players[i].speed;


        uint32_t lapThreshold = (uint32_t)NUM_LEDS * (players[i].laps + 1);
        if ((uint32_t)players[i].dist >= lapThreshold) {
            players[i].laps++;
            beep(600 + i * 100, 3);
            displayDirty = true;


            Serial.print("Player "); Serial.print(i + 1);
            Serial.print(" ? lap "); Serial.print(players[i].laps);
            Serial.print(" / ");     Serial.println(TOTAL_LAPS);


            if (players[i].laps >= TOTAL_LAPS) {
                for (int j = 0; j < NUM_PLAYERS; j++) players[j].speed = 0.0f;
                runWinAnimation(i);
                return;
            }
        }
    }


    if (displayDirty) { updateDisplay(); displayDirty = false; }


    drawCars();
    strip.show();
    tickBuzzer();
    delay(TICK_MS);
}


// ================================================================
//  SETUP
// ================================================================
void setup() {
    Serial.begin(9600);
    Serial.println("LED Race Uno v2.7.2 ? booting...");


    strip.begin();
    strip.clear();
    strip.show();
    strip.setBrightness(SR_BRIGNTNESS); 


    tmDisplay.setBrightness(TM_BRIGHTNESS);
    displayIdle();


    pinMode(PIN_P1, INPUT_PULLUP);
    pinMode(PIN_P2, INPUT_PULLUP);
    pinMode(PIN_AUDIO, OUTPUT);


    Serial.println("Setup done. Press a button to start!");
    enterIdle();
}


// ================================================================
//  LOOP
// ================================================================
void loop() {
    switch (gameState) {
        case STATE_IDLE:      loopIdle();    break;
        case STATE_COUNTDOWN: runCountdown(); break;
        case STATE_RACING:    loopRacing();  break;
        case STATE_WIN:       break;
    }
}

