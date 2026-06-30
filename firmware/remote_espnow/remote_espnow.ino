// TODO: add deep-sleep-on-idle + button wake for battery life.

#include <U8g2lib.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>


#define ESPNOW_CHANNEL 1
#define MUSICPI_VERSION "1.0.0"



// ---- OLED: 2.42" SSD1309 128x64, 4-wire SPI 9(Change for wtv screen you are running, this is the one I had)
U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI u8g2(U8G2_R0, D13, D11, D10, D9, D8);




// ---------STRUCTS---------
struct Button
{
  uint8_t pin;
  const char *cmd;
  bool last;
  uint32_t tLast;
};

struct Service
{
  const char *label;
  const char *cmd;
  bool destructive;
};
// ---------------------------


// ----------CONSTANT VARIABLES------------
Button buttons[] = {
    {D2, "PLAY_PAUSE", HIGH, 0},
    {D3, "NEXT", HIGH, 0},
    {D4, "PREV", HIGH, 0},
    {D5, "VOL_UP", HIGH, 0},
    {D6, "VOL_DOWN", HIGH, 0},
    {D7, "BT_CONNECT", HIGH, 0},
};

const int NUM_BUTTONS = sizeof(buttons) / sizeof(buttons[0]);
const uint32_t DEBOUNCE_MS = 40;

//next conts
const uint32_t NEXT_WINDOW_MS = 500;
const int NEXT_TRIPLE = 3;

// prev consts
const uint32_t PREV_WINDOW_MS = 500;
const int PREV_TRIPLE = 3;

// service menu consts
const Service SERVICES[] = {
    {"Reconnect BT", "SVC_BT", false},
    {"Toggle Screen turn off", "SAO", false},
    {"Restart Spotify", "SVC_SPOTIFY", false},
    {"Restart Radio", "SVC_RADIO", false},
    {"Bounce Wi-Fi", "SVC_WIFI", false},
    {"Shutdown", "SVC_SHUTDOWN", true},
    {"Reboot", "SVC_REBOOT", true},
    {"Test buttons", "TEST", false}
};
const int NUM_SERVICES = sizeof(SERVICES) / sizeof(SERVICES[0]);

// display consts
const int MENU_VISIBLE = 4;         // rows that fit under the header
const uint32_t MENU_TIMEOUT = 8000; // ms of inactivity before auto-close
const int TEXT_W = 104;       // px available for title / artist
const uint32_t FRAME_MS = 40; // animation step (marquee + intros)

// display/intro consts
const uint32_t INTRO_MS = 2000; // how long the intro plays
const uint32_t DISPLAY_TIMEOUT = 15000;
const uint32_t TEST_TIMEOUT = 10000;

// esp32 communication consts
const uint32_t HELLO_INTERVAL = 3000; // ms between HELLO keepalives
const uint32_t STALE_MS = 6000; // no status for this long -> "offline"
// ---------------------------


// ---------ENUMS-----------

enum UiMode
{
  UI_NOWPLAYING,
  UI_MENU,
  UI_CONFIRM,
  UI_INTRO,
  UI_STARTUP,
  UI_TEST
};

enum Theme
{
  TH_JAZZ,
  TH_ROCK,
  TH_ELECTRO,
  TH_GROOVE,
  TH_REGGAE,
  TH_WORLD,
  TH_DEFAULT
};
// ---------------------------




//-------GLOBAL VARIABLES----------
uint32_t lastButtonPress;

//next vars
int nextCount = 0;
uint32_t nextDeadline = 0;

//prev vars
int prevCount = 0;
uint32_t prevDeadline = 0;

// service menu 
UiMode ui = UI_NOWPLAYING;
int menuSel = 0;                    // highlighted service
int menuTop = 0;                    // first visible row (scroll offset)
uint32_t menuDeadline = 0;          // auto-close time

// dongle <-> remote communication variables
static uint8_t dongle_mac[6];
static bool have_dongle = false;
static uint8_t rxSrc[6];              // sender MAC of the last frame
uint32_t lastHelloMs = 0;
uint32_t lastStatusMs = 0;      // 0 = never heard from the Pi
bool wasOnline = false;
static uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static volatile bool rxReady = false;
static char rxData[210];
static volatile size_t rxLen = 0;

// display 
uint32_t lastFrameMs = 0;
int scroll1 = 0, scroll2 = 0;            // marquee offsets for title / artist
bool scrolling = false;                  // any line currently overflowing?
String shownLine1 = "", shownLine2 = ""; // text the marquee is tracking
bool displayOn = true;
bool displayHidden = false;
bool displayAlwaysOn = false;
bool startupDone = false;


// theme and animation playing for radio stations
Theme introTheme = TH_DEFAULT;
uint32_t introStart = 0;
String lastRadioStation = "";   // last station we animated for

//status received from the Pi
String mode = "SPOTIFY", state = "PAUSED", line1 = "", line2 = "", vol = "?";
bool dirty = true;

//test
bool testing = false;
bool play = false;
bool next = false;
bool prev = false;
bool volU = false;
bool volD = false;
uint32_t testStart;

// ---------------------------





// compatibale with both versions of esp arduino versions
// if statement creates correct onRecv() for version
//* both versions define src, src = MAC address of the device that sent ESP-NOW
// MAC Address is 6 bytes, 6 bytes are saved to rxSRC
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  const uint8_t *src = info->src_addr;
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len)
{
  const uint8_t *src = mac;
#endif
  memcpy(rxSrc, src, 6);

  // checks if there is already an unread message, if so then ignore the one just recieved 
  if (rxReady)
    return;

  // stores data inside rxData safely
  size_t n = (len < (int)sizeof(rxData) - 1) ? len : sizeof(rxData) - 1;
  memcpy(rxData, data, n);
  rxData[n] = '\0';

  // store length of message and say that it is ready to recieve new message
  rxLen = n;
  rxReady = true;
}

// sends a cmd over espnow using built in function: esp_now_send()
// sends to everyone if doesn't know dongle MAC Address
void sendCmd(const char *cmd)
{
  const uint8_t *dst = have_dongle ? dongle_mac : BROADCAST;
  esp_now_send(dst, (const uint8_t *)cmd, strlen(cmd));
}

//starts up the remote
//serial port at 115200 baud
//sets buttons as input pullup
//puts esp32 in station mode for espnow
//sets espnow channel, 1
void setup()
{
  Serial.begin(115200);
  for (int i = 0; i < NUM_BUTTONS; i++)
    pinMode(buttons[i].pin, INPUT_PULLUP);

  // ESP-NOW radio
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE); //* disables wifi power saving, this is optional, makes the signal more reliable and faster but also eats more power, if using battery turn this on, but for now its fine
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("esp_now_init failed");
  }
  esp_now_register_recv_cb(onRecv); // on recieve message run onRecv()
  
  // sets up broadcast peer, adds the broadcast address as an espnow peer, [0xFF, 0xFF...]
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  //starts up the oled
  u8g2.begin();
  u8g2.setBusClock(2000000);
  drawScreen();
}



void loop()
{
  readButtons();
  uint32_t now = millis(); // async delay()s basically along with below code

  if (testing)
  {
    if (now - testStart > TEST_TIMEOUT)
    {
      testing = false;
      ui = UI_NOWPLAYING;
    }
  }

  if (!startupDone)
    if (now < 3000)
    {
      ui = UI_STARTUP;
      drawScreen();
      return;
    }
    else 
    {
      startupDone = true;
      ui=UI_NOWPLAYING;
      dirty = true;
    }

  if (nextDeadline && now >= nextDeadline)
    flushNext();
  if (prevDeadline && now >= prevDeadline)
    flushPrev();

  // autoclose service window
  if (ui != UI_NOWPLAYING && menuDeadline && now >= menuDeadline)
    closeMenu();

  // hello unicast
  if (now - lastHelloMs >= HELLO_INTERVAL)
  {
    sendCmd("HELLO");
    lastHelloMs = now;
  }

  if (rxReady)
  {
    String s = String(rxData);
    if (s.startsWith("ST|"))
    { // only trust real status frames
      if (!have_dongle)
        learnDongle();
      parseStatus(s);
      lastStatusMs = now;
      recomputeScroll();

      if (mode == "RADIO" && line1 != lastRadioStation)
      {
        lastRadioStation = line1;
        if (ui == UI_NOWPLAYING || ui == UI_INTRO)
        {
          introTheme = themeFor(line1);
          introStart = now;
          ui = UI_INTRO;
        }
      }
      else if (mode != "RADIO")
      {
        lastRadioStation = "";
      }
    }
    rxReady = false;
  }

  bool animating = (ui == UI_INTRO) || (ui == UI_NOWPLAYING && scrolling);
  if (animating && now - lastFrameMs >= FRAME_MS)
  {
    lastFrameMs = now;
    if (ui == UI_NOWPLAYING)
    {
      scroll1++;
      scroll2++;
    }
    dirty = true;
  }
  if (ui == UI_INTRO && now - introStart >= INTRO_MS)
  {
    ui = UI_NOWPLAYING;
    dirty = true;
  }

  bool on = isOnline();
  if (on != wasOnline)
  {
    wasOnline = on;
    dirty = true;
  }

  if (dirty)
  {
    drawScreen();
    dirty = false;
  }

  if (now-lastButtonPress > DISPLAY_TIMEOUT)
  {
    if (!displayAlwaysOn)
    {
      displayHidden = true;
      displayOn = false;
      u8g2.setPowerSave(1);
    }
  }
  else
  {
    if (displayHidden){
      displayHidden = false;
      displayOn = true;
      u8g2.setPowerSave(0);
      dirty = true;
    }
  }
}

void learnDongle()
{
  memcpy(dongle_mac, rxSrc, 6);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, dongle_mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  have_dongle = true;
}

bool isOnline()
{
  return lastStatusMs != 0 && (millis() - lastStatusMs) < STALE_MS;
}

void readButtons()
{
  uint32_t now = millis();
  for (int i = 0; i < NUM_BUTTONS; i++)
  {
    bool s = digitalRead(buttons[i].pin);
    if (s != buttons[i].last && (now - buttons[i].tLast) > DEBOUNCE_MS)
    {
      buttons[i].tLast = now;
      if (s == LOW)
        handlePress(buttons[i].cmd); // pressed
      buttons[i].last = s;
    }
  }
}

void flushNext()
{
  for (int i = 0; i < nextCount; i++)
  {
    sendCmd("NEXT");
    delay(10); // small gap so back-to-back sends don't drop
  }
  nextCount = 0;
  nextDeadline = 0;
}

void toggleDisplay()
{
  displayOn = !displayOn;

  if (displayOn)
  {
    u8g2.setPowerSave(0); // wake OLED
    dirty = true;
  }
  else
  {
    u8g2.clearBuffer();
    u8g2.sendBuffer();
    u8g2.setPowerSave(1); // sleep OLED
  }
}

void flushPrev()
{
  for (int i = 0; i < prevCount; i++)
  {
    sendCmd("PREV");
    delay(10);
  }
  prevCount = 0;
  prevDeadline = 0;
}

void openMenu()
{
  ui = UI_MENU;
  menuSel = 0;
  menuTop = 0;
  menuDeadline = millis() + MENU_TIMEOUT;
  dirty = true;
}

void closeMenu()
{
  ui = UI_NOWPLAYING;
  menuDeadline = 0;
  dirty = true; // redraw the now-playing screen
}

void adjustScroll()
{
  if (menuSel < menuTop)
    menuTop = menuSel;
  if (menuSel >= menuTop + MENU_VISIBLE)
    menuTop = menuSel - MENU_VISIBLE + 1;
}

void handlePress(const char *cmd)
{
  uint32_t now = millis();

  lastButtonPress = now;

  if (displayHidden)
  {
    return;
  }

  if (ui == UI_INTRO)
    ui = UI_NOWPLAYING; // any press dismisses the intro

  if (ui == UI_NOWPLAYING)
  {
    if (strcmp(cmd, "NEXT") == 0)
    {
      flushPrev();

      nextCount++;
      nextDeadline = now + NEXT_WINDOW_MS;

      if (nextCount >= NEXT_TRIPLE)
      { // triple-click NEXT -> menu, send no skips
        nextCount = 0;
        nextDeadline = 0;
        openMenu();
      }
      return;
    }

    if (strcmp(cmd, "PREV") == 0)
    {
      flushNext();

      prevCount++;
      prevDeadline = now + PREV_WINDOW_MS;

      if (prevCount >= PREV_TRIPLE)
      { // triple-click PREV -> display toggle
        prevCount = 0;
        prevDeadline = 0;
        toggleDisplay();
      }
      return;
    }

    flushNext();
    flushPrev();

    // when display is off, any normal button can still send commands
    sendCmd(cmd);
    return;
  }

  menuDeadline = now + MENU_TIMEOUT;

  if (ui == UI_TEST)
  {
    if (strcmp(cmd, "NEXT") == 0)
    {
      next = true;
      return;
    }
    else if (strcmp(cmd, "PREV") == 0)
    {
      prev = true;
      return;
    }
    else if (strcmp(cmd, "PLAY_PAUSE") == 0)
    {
      play = true;
      return;
    }
    else if (strcmp(cmd, "VOL_UP") == 0)
    {
      volU = true;
      return;
    }
    else if (strcmp(cmd, "VOL_DOWN") == 0)
    {
      volD = true;
      return;
    }
    else {
      return;
    }
  }
  if (ui == UI_MENU)
  {
    if (strcmp(cmd, "NEXT") == 0)
    {
      menuSel = (menuSel + 1) % NUM_SERVICES;
      adjustScroll();
      dirty = true;
    }
    else if (strcmp(cmd, "PREV") == 0)
    {
      menuSel = (menuSel - 1 + NUM_SERVICES) % NUM_SERVICES;
      adjustScroll();
      dirty = true;
    }
    else if (strcmp(cmd, "PLAY_PAUSE") == 0)
    {
      if (SERVICES[menuSel].cmd == "SAO")
      {
        displayAlwaysOn = !displayAlwaysOn;
      }
      else if (SERVICES[menuSel].cmd == "TEST")
      {
        UI = UI_TEST;
        dirty = true;
        testing = true;
        testStart = now;
        play = next = volU = volD = prev = false;
      }
      else if (SERVICES[menuSel].destructive)
      {
        ui = UI_CONFIRM;
        dirty = true;
      }
      else
      {
        sendCmd(SERVICES[menuSel].cmd);
        closeMenu();
      }
    }
    else if (strcmp(cmd, "BT_CONNECT") == 0)
    {
      closeMenu(); // cancel, howeverr bt_connect button doesnt exist, so just here for future implementation
    }

    return;
  }

  if (ui == UI_CONFIRM)
  {
    if (strcmp(cmd, "PLAY_PAUSE") == 0)
    {
      sendCmd(SERVICES[menuSel].cmd);
      closeMenu();
    }
    else
    {
      ui = UI_MENU; // any other button backs out
      dirty = true;
    }
    return;
  }
}

void parseStatus(String s)
{
  if (!s.startsWith("ST|"))
    return;
  String f[6];
  int n = 0, start = 0;
  for (int i = 0; i < (int)s.length() && n < 6; i++)
  {
    if (s[i] == '|')
    {
      f[n++] = s.substring(start, i);
      start = i + 1;
    }
  }
  if (n < 6)
    f[n++] = s.substring(start);

  mode = f[1];
  state = f[2];
  line1 = f[3];
  line2 = f[4];
  vol = f[5];
  dirty = true;
}

void recomputeScroll()
{
  if (line1 != shownLine1)
  {
    scroll1 = 0;
    shownLine1 = line1;
  }
  if (line2 != shownLine2)
  {
    scroll2 = 0;
    shownLine2 = line2;
  }
  u8g2.setFont(u8g2_font_helvB10_tf);
  bool s1 = u8g2.getStrWidth(line1.c_str()) > TEXT_W;
  u8g2.setFont(u8g2_font_6x12_tf);
  bool s2 = u8g2.getStrWidth(line2.c_str()) > TEXT_W;
  scrolling = s1 || s2;
}

void drawScrolling(int x, int y, const String &s, int maxPx, int offset)
{
  int w = u8g2.getStrWidth(s.c_str());
  if (w <= maxPx)
  {
    u8g2.drawStr(x, y, s.c_str());
    return;
  }
  const int gap = 18;
  int period = w + gap;
  int o = offset % period;
  u8g2.setClipWindow(x, 0, x + maxPx, 63);
  u8g2.drawStr(x - o, y, s.c_str());
  u8g2.drawStr(x - o + period, y, s.c_str());
  u8g2.setMaxClipWindow();
}

int parseVol(const String &v)
{
  if (v.length() == 0 || v == "?")
    return -1;
  int n = v.toInt();
  return n < 0 ? 0 : (n > 100 ? 100 : n);
}

void drawScreen()
{
  if (!startupDone){
    drawStartup();
    return;
  }

  if (!displayOn || displayHidden)
    return;

  switch (ui)
  {
  case UI_TEST:
    drawTestScreen();
    break;
  case UI_MENU:
    drawMenu();
    break;
  case UI_CONFIRM:
    drawConfirm();
    break;
  case UI_INTRO:
    drawIntro();
    break;
  case UI_STARTUP:
    drawStartup();
  default:
    drawNowPlaying();
  }
}

void drawTestScreen()
{
  String strPlay = play ? "PRESSED" : "----";
  String strNext = next ? "PRESSED" : "----";
  String strPrev = prev ? "PRESSED" : "----";
  String strVolU = volU ? "PRESSED" : "----";
  String strVolD = volD ? "PRESSED" : "----"

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont12_mf);
  u8g2.drawStr(0, 7, "Play " + strPlay.c_str());
  u8g2.drawStr(0, 14, "Next " + strNext.c_str());
  u8g2.drawStr(0, 21, "Prev " + strPrev.c_str());
  u8g2.drawStr(0, 28, "VolUp " + strVolU.c_str());
  u8g2.drawStr(0, 35, "VolDown " + strVolD.c_str());
  u8g2.sendBuffer();
}

void drawStartup()
{
  String stringInput = "Version " + String(MUSICPI_VERSION);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont12_mf);
  u8g2.drawStr(0, 20, "MusicPi");
  u8g2.drawStr(0,40,stringInput.c_str());
  u8g2.sendBuffer();
}

void drawMenu()
{
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, "SERVICE MENU");
  u8g2.drawHLine(0, 13, 128);

  for (int row = 0; row < MENU_VISIBLE; row++)
  {
    int idx = menuTop + row;
    if (idx >= NUM_SERVICES)
      break;
    int y = 25 + row * 12; // 25, 37, 49, 61
    if (idx == menuSel)
      u8g2.drawStr(0, y, ">");
    u8g2.drawStr(10, y, SERVICES[idx].label);
  }

  // scroll hints on the right edge
  if (menuTop > 0)
    u8g2.drawStr(122, 25, "^");
  if (menuTop + MENU_VISIBLE < NUM_SERVICES)
    u8g2.drawStr(122, 61, "v");

  u8g2.sendBuffer();
}

void drawConfirm()
{
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, "CONFIRM");
  u8g2.drawHLine(0, 13, 128);
  u8g2.drawStr(0, 32, SERVICES[menuSel].label);
  u8g2.drawStr(0, 48, "PLAY = confirm");
  u8g2.drawStr(0, 60, "any other cancels");
  u8g2.sendBuffer();
}

void drawNowPlaying()
{
  u8g2.clearBuffer();
  bool online = isOnline();

  // top bar: mode (left), link indicator (right)
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, mode.c_str());
  if (online)
    u8g2.drawDisc(124, 5, 3); // filled = link up
  else
    u8g2.drawCircle(124, 5, 3); // hollow = stale / no link
  u8g2.drawHLine(0, 13, 128);

  if (lastStatusMs == 0)
  {
    const char *w = "Waiting for Pi...";
    u8g2.drawStr((128 - u8g2.getStrWidth(w)) / 2, 40, w);
    u8g2.sendBuffer();
    return;
  }

  // play/pause indicator
  bool playing = (state == "PLAYING");
  u8g2.setFont(u8g2_font_open_iconic_play_2x_t);
  u8g2.drawGlyph(0, 36, playing ? 0x45 : 0x44); // play / pause icon

  // title (line1) + subtitle (line2), scrolling if too long to fit
  u8g2.setFont(u8g2_font_helvB10_tf);
  drawScrolling(22, 34, line1, TEXT_W, scroll1);
  u8g2.setFont(u8g2_font_6x12_tf);
  drawScrolling(22, 48, line2, TEXT_W, scroll2);

  // volume bar along the bottom
  int vpct = parseVol(vol);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 63, "Vol");
  const int bx = 20, bw = 108; // bar spans x = 20..128
  u8g2.drawFrame(bx, 57, bw, 6);
  if (vpct >= 0)
    u8g2.drawBox(bx + 1, 58, (bw - 2) * vpct / 100, 4);

  u8g2.sendBuffer();
}

// ---- station intro animations ----------------------------------------------

Theme themeFor(String name)
{
  name.toLowerCase();
  if (name.indexOf("jazz") >= 0)
    return TH_JAZZ;
  if (name.indexOf("rock") >= 0 || name.indexOf("metal") >= 0)
    return TH_ROCK;
  if (name.indexOf("electro") >= 0)
    return TH_ELECTRO;
  if (name.indexOf("groove") >= 0)
    return TH_GROOVE;
  if (name.indexOf("reggae") >= 0)
    return TH_REGGAE;
  if (name.indexOf("monde") >= 0 || name.indexOf("world") >= 0)
    return TH_WORLD;
  return TH_DEFAULT;
}

// music note for jazz and stuff
void drawNote(int x, int y)
{
  u8g2.drawDisc(x, y, 2);
  u8g2.drawVLine(x + 2, y - 9, 9);
  u8g2.drawHLine(x + 2, y - 9, 3);
}

void animBars(uint32_t t, int n, float speed, float spread, int maxh)
{
  int slot = 128 / n;
  for (int i = 0; i < n; i++)
  {
    float p = t / speed + i * spread;
    int h = 4 + (int)((sin(p) * 0.5 + 0.5) * maxh);
    u8g2.drawBox(i * slot + 2, 50 - h, slot - 4, h);
  }
}

void animRock(uint32_t t) { animBars(t, 14, 90.0, 1.7, 44); }
void animGroove(uint32_t t) { animBars(t, 8, 260.0, 0.7, 40); }
void animDefault(uint32_t t) { animBars(t, 10, 200.0, 0.6, 40); }

void animJazz(uint32_t t)
{
  float ph = t / 1000.0;
  for (int i = 0; i < 5; i++)
  {
    int x = 16 + i * 24 + (int)(5 * sin(ph * 2 + i));
    int y = 50 - ((int)(ph * 22 + i * 13) % 50);
    drawNote(x, y);
  }
}

void animElectro(uint32_t t)
{
  float ph = t / 140.0;
  int py = 28;
  for (int x = 0; x <= 128; x += 4)
  {
    int y = 28 + (int)(16 * sin(x / 11.0 + ph));
    if (x)
      u8g2.drawLine(x - 4, py, x, y);
    py = y;
  }
  u8g2.drawCircle(64, 28, (t / 45) % 26); // expanding pulse
}

void animReggae(uint32_t t)
{
  u8g2.drawBox(0, 0, 128, 5); // three bands (mono stand-in)
  u8g2.drawBox(0, 7, 128, 5);
  u8g2.drawBox(0, 14, 128, 5);
  int sy = 52 - (int)((t / 60) % 34); // rising sun
  u8g2.drawDisc(64, sy, 7);
  for (int a = 0; a < 360; a += 45)
  {
    float r = a * 0.0174533;
    u8g2.drawLine(64 + cos(r) * 9, sy + sin(r) * 9,
                  64 + cos(r) * 13, sy + sin(r) * 13);
  }
}

void animWorld(uint32_t t)
{
  int cx = 64, cy = 28, R = 20;
  u8g2.drawCircle(cx, cy, R);
  u8g2.drawHLine(cx - R, cy, 2 * R);
  u8g2.drawEllipse(cx, cy, R, R / 2);
  float ph = t / 350.0;
  for (int m = 0; m < 3; m++)
  { // meridians sweeping = spin
    int rx = (int)(R * fabs(sin(ph + m * 1.05)));
    if (rx > 0)
      u8g2.drawEllipse(cx, cy, rx, R);
  }
}

void drawIntro()
{
  uint32_t t = millis() - introStart;
  u8g2.clearBuffer();
  switch (introTheme)
  {
  case TH_JAZZ:
    animJazz(t);
    break;
  case TH_ROCK:
    animRock(t);
    break;
  case TH_ELECTRO:
    animElectro(t);
    break;
  case TH_GROOVE:
    animGroove(t);
    break;
  case TH_REGGAE:
    animReggae(t);
    break;
  case TH_WORLD:
    animWorld(t);
    break;
  default:
    animDefault(t);
  }
  // station name banner along the bottom
  u8g2.setFont(u8g2_font_6x12_tf);
  int nx = (128 - u8g2.getStrWidth(line1.c_str())) / 2;
  if (nx < 0)
    nx = 0;
  u8g2.drawStr(nx, 63, line1.c_str());
  u8g2.sendBuffer();
}
