#include <WiFi.h>
#include <SPI.h>
#include <ArduinoJson.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

//////////////////////////////////////////////////////
// WIFI
//////////////////////////////////////////////////////

const char* WIFI_SSID = "PolakPC_WiFi";
const char* WIFI_PASS = "KupSiData";

const char* BRIDGE_HOST = "192.168.137.1";
const uint16_t BRIDGE_PORT = 8765;

WiFiClient client;

//////////////////////////////////////////////////////
// PINY
//////////////////////////////////////////////////////

#define TFT_CS 27
#define TFT_DC 25
#define TFT_RST 26
#define TFT_LED 12

#define TOUCH_CS 33
#define TOUCH_IRQ 32

#define SPI_SCK 14
#define SPI_MOSI 13
#define SPI_MISO 35

//////////////////////////////////////////////////////
// DISPLAY + TOUCH
//////////////////////////////////////////////////////

SPIClass spi(VSPI);
Adafruit_ILI9341 tft = Adafruit_ILI9341(&spi, TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

//////////////////////////////////////////////////////
// TOUCH KALIBRACE
//////////////////////////////////////////////////////

int TOUCH_MIN_X = 300;
int TOUCH_MAX_X = 3800;
int TOUCH_MIN_Y = 300;
int TOUCH_MAX_Y = 3800;

//////////////////////////////////////////////////////
// UI MODEL
//////////////////////////////////////////////////////

struct ButtonDef {
  String label;
  String action;
  int x;
  int y;
  int w;
  int h;
  bool visible;
  bool pressed;
  uint16_t accent;
};

static const int BUTTON_COUNT = 6;
ButtonDef buttons[BUTTON_COUNT];

String currentTitle = "Connecting...";
String currentProfile = "";
String currentPageId = "";
bool currentActive = false;

unsigned long lastReconnectAttempt = 0;
unsigned long lastPingMs = 0;
unsigned long toastUntil = 0;
String toastText = "";

const int SCREEN_W = 320;
const int SCREEN_H = 240;

const int TOP_BAR_H = 34;
const int FOOTER_H = 34;
const int BTN_GAP = 10;
const int GRID_COLS = 3;
const int GRID_ROWS = 2;
const int BTN_RADIUS = 12;

static const int MAX_PAGES = 8;
String pageIds[MAX_PAGES];
String pageTitles[MAX_PAGES];
int pageCount = 0;

//////////////////////////////////////////////////////
// BARVY
//////////////////////////////////////////////////////

#define C_BG             0x10A2
#define C_PANEL          0x18E3
#define C_PANEL_2        0x2124
#define C_TEXT           ILI9341_WHITE
#define C_MUTED          0xAD55
#define C_ACCENT         0x05FF
#define C_SUCCESS        0x3666
#define C_WARNING        0xFD20
#define C_DANGER         0xF104
#define C_BUTTON         0x2965
#define C_BUTTON_BORDER  0x5AEB
#define C_BUTTON_PRESS   0x3A49
#define C_INACTIVE       0x2945
#define C_TAB_ACTIVE     0x05FF
#define C_TAB_IDLE       0x39E7
#define C_TOAST_BG       0x18E3
#define C_TOAST_TEXT     ILI9341_WHITE

//////////////////////////////////////////////////////
// Ztmavení a vypnutí displaye
//////////////////////////////////////////////////////
unsigned long lastInteractionMs = 0;

const unsigned long DIM_TIMEOUT_MS = 60000;      // 1 minuta
const unsigned long SLEEP_TIMEOUT_MS = 300000;   // 5 minut

bool isDimmed = false;
bool isSleeping = false;

void handleIdleDisplay() {
  unsigned long now = millis();
  unsigned long idle = now - lastInteractionMs;

  if (!isSleeping && idle >= SLEEP_TIMEOUT_MS) {
    sleepDisplay();
    return;
  }

  if (!isDimmed && idle >= DIM_TIMEOUT_MS) {
    dimDisplay();
  }
}

//////////////////////////////////////////////////////
// DISPLAY / TOUCH
//////////////////////////////////////////////////////

void displayInit() {
  spi.begin(SPI_SCK, SPI_MISO, SPI_MOSI, TFT_CS);

  tft.begin();
  tft.setRotation(3);
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  ts.begin(spi);
  ts.setRotation(3);
}

void displayClear() {
  tft.fillScreen(C_BG);
}

bool readTouch(int &x, int &y) {
  if (!ts.touched()) return false;

  TS_Point p = ts.getPoint();

  x = map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 319, 0);
  y = map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 239, 0);

  if (x < 0) x = 0;
  if (x > 319) x = 319;
  if (y < 0) y = 0;
  if (y > 239) y = 239;

  return true;
}

void setBacklightPercent(int percent) {
  percent = constrain(percent, 0, 100);

  // ESP32 analogWrite funguje v Arduino core, pokud by ne, přejdeme na LEDC
  int pwm = map(percent, 0, 100, 0, 255);
  analogWrite(TFT_LED, pwm);
}

void wakeDisplay() {
  if (isSleeping) {
    setBacklightPercent(100);
    tft.writeCommand(ILI9341_SLPOUT);
    delay(120);
    tft.writeCommand(ILI9341_DISPON);
    delay(20);

    isSleeping = false;
    renderUI();
  }

  if (isDimmed) {
    setBacklightPercent(100);
    isDimmed = false;
  }

  lastInteractionMs = millis();
}

void dimDisplay() {
  if (!isSleeping && !isDimmed) {
    setBacklightPercent(50);
    isDimmed = true;
  }
}

void sleepDisplay() {
  if (!isSleeping) {
    setBacklightPercent(0);
    tft.writeCommand(ILI9341_DISPOFF);
    tft.writeCommand(ILI9341_SLPIN);
    isSleeping = true;
  }
}

//////////////////////////////////////////////////////
// LAYOUT HELPERS
//////////////////////////////////////////////////////

void computeButtonRects() {
  int usableY = TOP_BAR_H + BTN_GAP;
  int usableH = SCREEN_H - TOP_BAR_H - FOOTER_H - BTN_GAP * 2;
  int btnW = (SCREEN_W - BTN_GAP * (GRID_COLS + 1)) / GRID_COLS;
  int btnH = (usableH - BTN_GAP * (GRID_ROWS - 1)) / GRID_ROWS;

  for (int i = 0; i < BUTTON_COUNT; i++) {
    int row = i / GRID_COLS;
    int col = i % GRID_COLS;

    buttons[i].x = BTN_GAP + col * (btnW + BTN_GAP);
    buttons[i].y = usableY + row * (btnH + BTN_GAP);
    buttons[i].w = btnW;
    buttons[i].h = btnH;
  }
}

uint16_t accentForAction(const String& action) {
  if (action == "cut") return C_DANGER;
  if (action == "play_pause") return C_ACCENT;
  if (action == "add_marker") return C_WARNING;
  if (action == "undo" || action == "redo") return C_TAB_IDLE;
  if (action == "next_node" || action == "prev_node") return C_ACCENT;
  if (action == "bypass_color") return C_WARNING;
  return C_BUTTON_BORDER;
}

void resetButtons() {
  for (int i = 0; i < BUTTON_COUNT; i++) {
    buttons[i].label = "—";
    buttons[i].action = "noop";
    buttons[i].visible = true;
    buttons[i].pressed = false;
    buttons[i].accent = C_BUTTON_BORDER;
  }
}

void showToast(const String& text, unsigned long durationMs = 900) {
  toastText = text;
  toastUntil = millis() + durationMs;
}

//////////////////////////////////////////////////////
// DRAW
//////////////////////////////////////////////////////

void drawStatusDot(int x, int y, uint16_t color) {
  tft.fillCircle(x, y, 4, color);
}

void drawSingleButton(int i) {
  uint16_t bg;
  uint16_t border;

  if (!currentActive) {
    bg = C_INACTIVE;
    border = C_TAB_IDLE;
  } else if (buttons[i].pressed) {
    bg = C_BUTTON_PRESS;
    border = buttons[i].accent;
  } else {
    bg = C_BUTTON;
    border = buttons[i].accent;
  }

  tft.fillRoundRect(buttons[i].x, buttons[i].y, buttons[i].w, buttons[i].h, BTN_RADIUS, bg);
  tft.drawRoundRect(buttons[i].x, buttons[i].y, buttons[i].w, buttons[i].h, BTN_RADIUS, border);

  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT, bg);

  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(buttons[i].label, 0, 0, &x1, &y1, &tw, &th);

  int tx = buttons[i].x + (buttons[i].w - (int)tw) / 2;
  int ty = buttons[i].y + (buttons[i].h - (int)th) / 2;

  if (tx < buttons[i].x + 8) tx = buttons[i].x + 8;
  if (ty < buttons[i].y + 8) ty = buttons[i].y + 8;

  tft.setCursor(tx, ty);
  tft.print(buttons[i].label);
}

void drawButtons() {
  for (int i = 0; i < BUTTON_COUNT; i++) {
    drawSingleButton(i);
  }
}

void drawTopBar() {
  tft.fillRect(0, 0, SCREEN_W, TOP_BAR_H, C_PANEL);

  tft.setTextColor(C_TEXT, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(10, 11);
  tft.print(currentTitle);

  drawStatusDot(250, 17, WiFi.isConnected() ? C_SUCCESS : C_DANGER);
  drawStatusDot(266, 17, client.connected() ? C_ACCENT : C_DANGER);
  drawStatusDot(282, 17, currentActive ? C_SUCCESS : C_WARNING);

  tft.setCursor(292, 11);
  tft.print(currentActive ? "ON" : "OFF");
}

void drawPageTabs() {
  if (pageCount <= 0) return;

  int y = SCREEN_H - FOOTER_H + 4;
  int h = 24;
  int gap = 6;
  int tabW = (SCREEN_W - ((pageCount + 1) * gap)) / pageCount;

  for (int i = 0; i < pageCount; i++) {
    int x = gap + i * (tabW + gap);
    bool activeTab = pageIds[i] == currentPageId;

    uint16_t bg = activeTab ? C_TAB_ACTIVE : C_PANEL_2;
    uint16_t fg = activeTab ? ILI9341_BLACK : C_TEXT;

    tft.fillRoundRect(x, y, tabW, h, 8, bg);
    tft.drawRoundRect(x, y, tabW, h, 8, activeTab ? C_TAB_ACTIVE : C_TAB_IDLE);

    tft.setTextColor(fg, bg);
    tft.setTextSize(1);

    String txt = pageTitles[i];
    if (txt.startsWith("Resolve · ")) {
      txt.replace("Resolve · ", "");
    }

    int16_t x1, y1;
    uint16_t tw, th;
    tft.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);

    int tx = x + (tabW - (int)tw) / 2;
    int ty = y + (h - (int)th) / 2;

    tft.setCursor(tx, ty);
    tft.print(txt);
  }
}

void drawFooter() {
  tft.fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, C_PANEL);
  drawPageTabs();
}

void drawToast() {
  if (millis() > toastUntil || toastText.length() == 0) return;

  int h = 24;
  int y = TOP_BAR_H + 6;
  int w = toastText.length() * 8 + 20;
  if (w > 180) w = 180;
  int x = SCREEN_W - w - 10;

  tft.fillRoundRect(x, y, w, h, 8, C_TOAST_BG);
  tft.drawRoundRect(x, y, w, h, 8, C_ACCENT);

  tft.setTextSize(1);
  tft.setTextColor(C_TOAST_TEXT, C_TOAST_BG);
  tft.setCursor(x + 10, y + 8);
  tft.print(toastText);
}

void renderUI() {
  displayClear();
  drawTopBar();
  drawButtons();
  drawFooter();
  drawToast();
}

void setButtonPressed(int idx, bool pressed) {
  if (idx < 0 || idx >= BUTTON_COUNT) return;
  buttons[idx].pressed = pressed;
  drawSingleButton(idx);
}

//////////////////////////////////////////////////////
// NETWORK
//////////////////////////////////////////////////////

void connectWifi() {
  if (WiFi.isConnected()) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("[WIFI] Connecting");
  unsigned long start = millis();
  while (!WiFi.isConnected() && millis() - start < 12000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.isConnected()) {
    Serial.print("[WIFI] Connected, IP=");
    Serial.println(WiFi.localIP());
    showToast("WiFi");
    renderUI();
  } else {
    Serial.println("[WIFI] Failed");
  }
}

void sendJson(const JsonDocument& doc) {
  if (!client.connected()) return;
  serializeJson(doc, client);
  client.print("\n");
}

void sendHello() {
  StaticJsonDocument<128> doc;
  doc["type"] = "hello";
  doc["device"] = "esp32-touchdeck";
  sendJson(doc);
}

void sendPing() {
  StaticJsonDocument<64> doc;
  doc["type"] = "ping";
  sendJson(doc);
}

void sendButtonPress(const String& action) {
  StaticJsonDocument<160> doc;
  doc["type"] = "button_press";
  doc["action"] = action;
  doc["page_id"] = currentPageId;
  sendJson(doc);

  Serial.print("[SEND] button_press: ");
  Serial.println(action);
}

void sendChangePage(const String& pageId) {
  StaticJsonDocument<160> doc;
  doc["type"] = "change_page";
  doc["page_id"] = pageId;
  sendJson(doc);

  Serial.print("[SEND] change_page: ");
  Serial.println(pageId);
}

bool connectBridge() {
  if (client.connected()) return true;

  Serial.printf("[BRIDGE] Connecting to %s:%u\n", BRIDGE_HOST, BRIDGE_PORT);
  if (client.connect(BRIDGE_HOST, BRIDGE_PORT)) {
    Serial.println("[BRIDGE] Connected");
    sendHello();
    showToast("Bridge");
    renderUI();
    return true;
  }

  Serial.println("[BRIDGE] Connect failed");
  return false;
}

//////////////////////////////////////////////////////
// MESSAGE HANDLING
//////////////////////////////////////////////////////

void applyLayout(JsonDocument& doc) {
  currentProfile = doc["profile"] | "";
  currentPageId = doc["page_id"] | "";
  currentTitle = doc["title"] | "Resolve";
  currentActive = doc["active"] | false;

  pageCount = 0;
  if (doc["pages"].is<JsonArray>()) {
    for (JsonObject p : doc["pages"].as<JsonArray>()) {
      if (pageCount >= MAX_PAGES) break;
      pageIds[pageCount] = String((const char*)(p["page_id"] | ""));
      pageTitles[pageCount] = String((const char*)(p["title"] | ""));
      pageCount++;
    }
  }

  resetButtons();

  JsonArray arr = doc["buttons"].as<JsonArray>();
  for (JsonObject b : arr) {
    int slot = b["slot"] | -1;
    if (slot < 0 || slot >= BUTTON_COUNT) continue;

    buttons[slot].label = String((const char*)(b["label"] | "—"));
    buttons[slot].action = String((const char*)(b["action"] | "noop"));
    buttons[slot].visible = true;
    buttons[slot].accent = accentForAction(buttons[slot].action);
    buttons[slot].pressed = false;
  }

  Serial.print("[LAYOUT] title=");
  Serial.print(currentTitle);
  Serial.print(" page=");
  Serial.println(currentPageId);

  renderUI();
}

void handleActionResult(JsonDocument& doc) {
  bool ok = doc["ok"] | false;
  String action = String((const char*)(doc["action"] | ""));
  String message = String((const char*)(doc["message"] | ""));

  Serial.print("[ACTION_RESULT] ok=");
  Serial.print(ok);
  Serial.print(" action=");
  Serial.print(action);
  Serial.print(" msg=");
  Serial.println(message);

  if (ok) showToast(action, 700);
  else showToast("ERR", 1200);

  drawTopBar();
  drawFooter();
  drawToast();
}

void processLine(const String& line) {
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print("[JSON] Parse error: ");
    Serial.println(err.c_str());
    return;
  }

  String type = String((const char*)(doc["type"] | ""));

  if (type == "set_layout") {
    applyLayout(doc);
  } else if (type == "action_result") {
    handleActionResult(doc);
  } else if (type == "pong") {
    // ok
  } else {
    Serial.print("[MSG] Unknown type: ");
    Serial.println(type);
  }
}

void pollBridge() {
  static String lineBuffer = "";

  while (client.connected() && client.available()) {
    char c = (char)client.read();
    if (c == '\n') {
      processLine(lineBuffer);
      lineBuffer = "";
    } else if (c != '\r') {
      lineBuffer += c;
    }
  }
}

//////////////////////////////////////////////////////
// TOUCH / PAGE SWITCH
//////////////////////////////////////////////////////

int findButtonAt(int tx, int ty) {
  for (int i = 0; i < BUTTON_COUNT; i++) {
    if (!buttons[i].visible) continue;
    bool inside =
      tx >= buttons[i].x &&
      tx <  buttons[i].x + buttons[i].w &&
      ty >= buttons[i].y &&
      ty <  buttons[i].y + buttons[i].h;
    if (inside) return i;
  }
  return -1;
}

int findTabAt(int tx, int ty) {
  if (pageCount <= 0) return -1;

  int y = SCREEN_H - FOOTER_H + 4;
  int h = 24;
  int gap = 6;
  int tabW = (SCREEN_W - ((pageCount + 1) * gap)) / pageCount;

  for (int i = 0; i < pageCount; i++) {
    int x = gap + i * (tabW + gap);
    if (tx >= x && tx < x + tabW && ty >= y && ty < y + h) return i;
  }

  return -1;
}

int findCurrentPageIndex() {
  for (int i = 0; i < pageCount; i++) {
    if (pageIds[i] == currentPageId) return i;
  }
  return -1;
}

void switchToNextPage() {
  if (pageCount <= 1) return;
  int idx = findCurrentPageIndex();
  if (idx < 0) idx = 0;
  idx = (idx + 1) % pageCount;
  sendChangePage(pageIds[idx]);
}

void switchToPrevPage() {
  if (pageCount <= 1) return;
  int idx = findCurrentPageIndex();
  if (idx < 0) idx = 0;
  idx = (idx - 1 + pageCount) % pageCount;
  sendChangePage(pageIds[idx]);
}

void handleTouchAndSwipe() {
  static bool touchActive = false;
  static int startX = 0;
  static int startY = 0;
  static int lastX = 0;
  static int lastY = 0;
  static unsigned long startMs = 0;
  static unsigned long lastTapMs = 0;

  int x = 0, y = 0;
  bool down = readTouch(x, y);

  if (down) {
    wakeDisplay();
  }

  if (down && !touchActive) {
    touchActive = true;
    startX = x;
    startY = y;
    lastX = x;
    lastY = y;
    startMs = millis();
  }

  if (down && touchActive) {
    lastX = x;
    lastY = y;
  }

  if (!down && touchActive) {
    int dx = lastX - startX;
    int dy = lastY - startY;
    unsigned long dt = millis() - startMs;

    touchActive = false;

    if (dt < 700 && abs(dx) > 70 && abs(dy) < 40) {
      if (dx > 0) switchToPrevPage();
      else switchToNextPage();
      return;
    }

    if (dt < 400 && abs(dx) < 20 && abs(dy) < 20 && millis() - lastTapMs > 90) {
      lastTapMs = millis();

      int tabIdx = findTabAt(lastX, lastY);
      if (tabIdx >= 0) {
        showToast(pageTitles[tabIdx], 500);
        sendChangePage(pageIds[tabIdx]);
        drawFooter();
        drawToast();
        return;
      }

      int idx = findButtonAt(lastX, lastY);
      if (idx >= 0) {
        Serial.print("[TOUCH] slot=");
        Serial.print(idx);
        Serial.print(" label=");
        Serial.print(buttons[idx].label);
        Serial.print(" action=");
        Serial.println(buttons[idx].action);

        if (currentActive && buttons[idx].action != "noop") {
          setButtonPressed(idx, true);
          showToast(buttons[idx].label, 500);
          drawToast();

          sendButtonPress(buttons[idx].action);

          delay(60);
          setButtonPressed(idx, false);
        }
      }
    }
  }
}

//////////////////////////////////////////////////////
// SETUP / LOOP
//////////////////////////////////////////////////////

void setup() {
  Serial.begin(115200);

  pinMode(TFT_LED, OUTPUT);
  analogWrite(TFT_LED, 255);

  computeButtonRects();
  resetButtons();

  displayInit();
  renderUI();

  lastInteractionMs = millis();

  connectWifi();
  connectBridge();
}

void loop() {
  if (!WiFi.isConnected()) {
    connectWifi();
  }

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 3000) {
      lastReconnectAttempt = now;
      connectBridge();
      renderUI();
    }
  } else {
    pollBridge();

    if (millis() - lastPingMs > 5000) {
      lastPingMs = millis();
      sendPing();
    }
  }

  handleTouchAndSwipe();

  if (toastUntil > 0 && millis() > toastUntil) {
    toastUntil = 0;
    toastText = "";
    renderUI();
  }

  handleIdleDisplay();
}