
// ================================================================
//  REMOTE SENDER — copy into a separate .ino on a second ESP32
// ================================================================
 
//ESP2
#include <esp_now.h>
#include <WiFi.h>

// ← Paste the MAC address printed by main ESP32 (DEBUG 1 at boot):
uint8_t RECEIVER_MAC[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};

#define REMOTE_PIN_P1  12
#define REMOTE_PIN_P2  14
#define REMOTE_PIN_P3  26
#define REMOTE_PIN_P4  27

typedef struct { uint8_t buttons; } EspNowBtnPacket;
esp_now_peer_info_t peerInfo;
EspNowBtnPacket     packet;
uint8_t             lastPacket = 0xFF;

void onSent(const uint8_t *mac, esp_now_send_status_t status) {}

void setup() {
    pinMode(REMOTE_PIN_P1, INPUT_PULLUP);
    pinMode(REMOTE_PIN_P2, INPUT_PULLUP);
    pinMode(REMOTE_PIN_P3, INPUT_PULLUP);
    pinMode(REMOTE_PIN_P4, INPUT_PULLUP);
    WiFi.mode(WIFI_STA);
    esp_now_init();
    WiFi.setSleep(false);
    esp_now_register_send_cb(onSent);
    memcpy(peerInfo.peer_addr, RECEIVER_MAC, 6);
    peerInfo.channel = 0; peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
}

void loop() {
    uint8_t bits = 0;
    if (digitalRead(REMOTE_PIN_P1) == LOW) bits |= (1<<0);
    if (digitalRead(REMOTE_PIN_P2) == LOW) bits |= (1<<1);
    if (digitalRead(REMOTE_PIN_P3) == LOW) bits |= (1<<2);
    if (digitalRead(REMOTE_PIN_P4) == LOW) bits |= (1<<3);
    if (bits != lastPacket) {
        packet.buttons = bits;
        esp_now_send(RECEIVER_MAC, (uint8_t*)&packet, sizeof(packet));
        lastPacket = bits;
    }
    delay(10);
}
 
