// remote_espnow.ino
// Wireless music remote: ESP32 + 2.42" SSD1309 OLED + buttons.
// Talks ESP-NOW to the C3 Super Mini dongle on the Pi.
//
//   button press -> ESP-NOW BROADCAST of the command ("NEXT", "PLAY_PAUSE", ...)
//   "ST|..." status from the Pi  -> arrives via ESP-NOW, drawn on the OLED
//
//
// TODO: add deep-sleep-on-idle + button wake for battery life.

#include <U8g2lib.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define ESPNOW_CHANNEL 1   // MUST match the dongle

// ---- OLED: 2.42" SSD1309 128x64, 4-wire SPI (software SPI = any pins) ----
// args: rotation, clock(SCK), data(SDA), cs, dc, reset
U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI u8g2(U8G2_R0, D13, D11, D10, D9, D8);

struct Button { uint8_t pin; const char* cmd; bool last; uint32_t tLast; };
Button buttons[] = {
  { D2, "PLAY_PAUSE", HIGH, 0 },
  { D3, "NEXT",       HIGH, 0 },
  { D4, "PREV",       HIGH, 0 },
  { D5, "VOL_UP",     HIGH, 0 },
  { D6, "VOL_DOWN",   HIGH, 0 },
  { D7, "BT_CONNECT", HIGH, 0 },
};
const int NUM_BUTTONS = sizeof(buttons) / sizeof(buttons[0]);
const uint32_t DEBOUNCE_MS = 40;

// ---- ESP-NOW ----
static uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// status bytes received from the dongle, consumed in loop()
static volatile bool   rxReady = false;
static char            rxData[210];
static volatile size_t rxLen = 0;

// ---- status received from the Pi ----
String mode = "SPOTIFY", state = "PAUSED", line1 = "", line2 = "", vol = "?";
bool dirty = true;

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (rxReady) return;                       // previous not consumed yet -> drop
  size_t n = (len < (int)sizeof(rxData) - 1) ? len : sizeof(rxData) - 1;
  memcpy(rxData, data, n);
  rxData[n] = '\0';
  rxLen = n;
  rxReady = true;
}

void sendCmd(const char* cmd) {
  esp_now_send(BROADCAST, (const uint8_t*)cmd, strlen(cmd));
}

void setup() {
  Serial.begin(115200); 
  for (int i = 0; i < NUM_BUTTONS; i++) pinMode(buttons[i].pin, INPUT_PULLUP);

  // ESP-NOW radio
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init failed");
  }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  u8g2.begin();
  u8g2.setBusClock(2000000);
  drawScreen();
}

void loop() {
  readButtons();
  if (rxReady) { parseStatus(String(rxData)); rxReady = false; }
  if (dirty) { drawScreen(); dirty = false; }
}


void readButtons() {
  uint32_t now = millis();
  for (int i = 0; i < NUM_BUTTONS; i++) {
    bool s = digitalRead(buttons[i].pin);
    if (s != buttons[i].last && (now - buttons[i].tLast) > DEBOUNCE_MS) {
      buttons[i].tLast = now;
      if (s == LOW) sendCmd(buttons[i].cmd);   // pressed
      buttons[i].last = s;
    }
  }
}

void parseStatus(String s) {
  if (!s.startsWith("ST|")) return;
  String f[6]; int n = 0, start = 0;
  for (int i = 0; i < (int)s.length() && n < 6; i++) {
    if (s[i] == '|') { f[n++] = s.substring(start, i); start = i + 1; }
  }
  if (n < 6) f[n++] = s.substring(start);
  // f[0]="ST", f[1]=mode, f[2]=state, f[3]=line1, f[4]=line2, f[5]=vol
  mode = f[1]; state = f[2]; line1 = f[3]; line2 = f[4]; vol = f[5];
  dirty = true;
}

String fit(String s, int maxPx) {
  if (u8g2.getStrWidth(s.c_str()) <= maxPx) return s;
  while (s.length() > 1 && u8g2.getStrWidth((s + "..").c_str()) > maxPx)
    s.remove(s.length() - 1);
  return s + "..";
}

void drawScreen() {
  u8g2.clearBuffer();

  // top bar: mode (left), volume (right)
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, mode.c_str());
  String v = "Vol " + vol + "%";
  u8g2.drawStr(128 - u8g2.getStrWidth(v.c_str()), 10, v.c_str());
  u8g2.drawHLine(0, 13, 128);

  // play/pause indicator
  bool playing = (state == "PLAYING");
  u8g2.setFont(u8g2_font_open_iconic_play_2x_t);
  u8g2.drawGlyph(0, 36, playing ? 0x45 : 0x44);   // play / pause icon

  // title (line1) + subtitle (line2)
  u8g2.setFont(u8g2_font_helvB10_tf);
  u8g2.drawStr(22, 34, fit(line1, 104).c_str());
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(22, 50, fit(line2, 104).c_str());

  u8g2.sendBuffer();
}
