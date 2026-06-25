// dongle_espnow_bridge.ino
// ESP32-C3 Super Mini -- transparent ESP-NOW <-> USB-serial bridge.
//
// Lives plugged into the Raspberry Pi
//
// Wireless protocol:
//   Remote -> dongle : command strings ("NEXT", "PLAY_PAUSE", ...) via ESP-NOW BROADCAST
//   dongle -> Remote : "ST|..." status lines, UNICAST back to whoever last sent a command
//
//
// Build settings (Arduino IDE / arduino-cli), board "ESP32C3 Dev Module":
//   - USB CDC On Boot: "Enabled"  
//   - Upload speed: 115200

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define ESPNOW_CHANNEL 1
#define LED_PIN 8         

static uint8_t remote_mac[6];    
static bool    have_remote = false;

static char    out_line[260];     
static size_t  out_len = 0;

static unsigned long led_off_at = 0;

static void led(bool on) { digitalWrite(LED_PIN, on ? LOW : HIGH); }  // active LOW

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *src = info->src_addr;
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  const uint8_t *src = mac;
#endif
  memcpy(remote_mac, src, 6);
  if (!have_remote) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, remote_mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    have_remote = true;
  }
  Serial.write(data, len);
  if (len == 0 || data[len - 1] != '\n') Serial.write('\n');

  led(true);
  led_off_at = millis() + 30;     // brief activity flash
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  led(false);
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    while (true) { led(true); delay(100); led(false); delay(100); }  // SOS blink
  }
  esp_now_register_recv_cb(onRecv);
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (out_len > 0 && have_remote) {
        esp_now_send(remote_mac, (const uint8_t *)out_line, out_len);
      }
      out_len = 0;
    } else if (out_len < sizeof(out_line) - 1) {
      out_line[out_len++] = c;
    }
  }

  if (led_off_at && millis() > led_off_at) { led(false); led_off_at = 0; }
}
