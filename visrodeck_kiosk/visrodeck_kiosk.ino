#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h> // Generic display library; this constructor targets SSD1306 128x64 I2C.
#include <ArduinoJson.h> // ArduinoJson 6 or 7
// --------------------------- Kiosk configuration --------------------------
const char *WIFI_SSID = "AICTechno@Guest";
const char *WIFI_PASSWORD = "Guest@aic#1126";
const char *SERVER_BASE_URL = "http://10.10.131.182:8000"; // Laptop's current Wi-Fi IP and server port.
// The included laptop server uses the case-sensitive ID "Bedu".
const char *PATIENT_ID = "Bedu";
const int APPROACH_DISTANCE_CM = 60;

// ------------------------------- Pinout -----------------------------------
constexpr uint8_t OLED_SDA = 21, OLED_SCL = 22;
constexpr uint8_t TRIG_PIN = 5, ECHO_PIN = 18, TOUCH_PIN = 4;
constexpr bool TOUCH_ACTIVE_HIGH = true; // TTP223 default; set false if its AHLB pad selects active-low.
constexpr bool TOUCH_LOGIN_ENABLED = false; // Demo login uses the existing YES button instead.
constexpr uint8_t ENC_CLK = 25, ENC_DT = 26, ENC_SW = 27;
constexpr uint8_t YES_PIN = 32, NO_PIN = 33;
constexpr uint8_t LED_RED = 14, LED_GREEN = 13, LED_BLUE = 19;
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint32_t AWAY_APPROACH_MS = 5000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr size_t MAX_RECORDS = 20;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R2, U8X8_PIN_NONE); // Rotate the complete OLED image 180 degrees.

struct Record {
  String date, doctor, diagnosis, notes, medicineName, dosage, frequency, duration;
  int id = -1;
  bool isDemo = false;
};
Record records[MAX_RECORDS];
size_t recordCount = 0;
bool serverUnavailable = false;

enum KioskState { STATE_IDLE, STATE_APPROACH, STATE_AUTHENTICATING, STATE_BROWSE };
KioskState state = STATE_IDLE;
bool confirmView = false;
size_t selectedItem = 0; // reports, followed by print-history and refresh items
uint32_t farSince = 0;
bool touchWasHigh = false;
bool touchRawInitialized = false;
bool touchLastRawHigh = false;
uint32_t lastDistanceRead = 0;
int filteredDistance = -1;
int lastPingCm[3] = {-1, -1, -1};
uint8_t consecutiveNoEchoSets = 0;
bool sensorHasValidReading = false;
bool sensorWarningVisible = false;

// Encoder transitions are decoded in an ISR using a complete Gray-code table.
volatile int32_t encoderSteps = 0;
volatile uint8_t encoderPrevious = 0;
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
const int8_t QUADRATURE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

struct DebouncedButton {
  uint8_t pin;
  bool stable = HIGH;
  bool lastRaw = HIGH;
  uint32_t changedAt = 0;
  void begin() { pinMode(pin, INPUT_PULLUP); stable = lastRaw = digitalRead(pin); }
  bool pressed() {
    bool raw = digitalRead(pin);
    uint32_t now = millis();
    if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
    if (raw != stable && now - changedAt >= BUTTON_DEBOUNCE_MS) {
      stable = raw;
      return stable == LOW;
    }
    return false;
  }
};
DebouncedButton yesButton{YES_PIN}, noButton{NO_PIN};

void IRAM_ATTR encoderISR() {
  uint8_t current = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  uint8_t transition = (encoderPrevious << 2) | current;
  portENTER_CRITICAL_ISR(&encoderMux);
  encoderSteps += QUADRATURE[transition];
  encoderPrevious = current;
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void setLeds(bool red, bool green, bool blue) {
  digitalWrite(LED_RED, red); digitalWrite(LED_GREEN, green); digitalWrite(LED_BLUE, blue);
}

void showLines(const String &a, const String &b = "", const String &c = "", const String &d = "") {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 10, a.c_str());
  if (b.length()) display.drawStr(0, 22, b.c_str());
  if (c.length()) display.drawStr(0, 34, c.c_str());
  if (d.length()) display.drawStr(0, 46, d.c_str());
  display.sendBuffer();
}

void showBranding() {
  display.clearBuffer();
  display.setFont(u8g2_font_ncenB14_tr);
  display.drawStr(0, 19, "VISRODECK");
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 32, "Digital Zones");
  display.drawStr(0, 48, "Press YES to login");
  display.drawStr(0, 60, "Bedu demo account");
  display.sendBuffer();
}

void showIdleScreen() {
  showLines("VISRODECK READY", "Press YES to login", "Bedu demo account");
}

void setState(KioskState next) {
  state = next;
  if (next == STATE_IDLE) {
    setLeds(false, false, false);
    display.setPowerSave(0);
    sensorWarningVisible = false;
    showIdleScreen();
    confirmView = false; selectedItem = 0; farSince = 0;
  } else {
    display.setPowerSave(0);
    sensorWarningVisible = false;
    if (next == STATE_APPROACH) { setLeds(false, false, true); showBranding(); }
    if (next == STATE_BROWSE) { setLeds(false, true, false); confirmView = false; }
  }
}

int readDistanceOnce() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10); digitalWrite(TRIG_PIN, LOW);
  // 30 ms covers the HC-SR04's practical range. pulseIn returns 0 on timeout.
  uint32_t duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return -1;
  int cm = (int)(duration * 0.0343f / 2.0f);
  return (cm >= 2 && cm <= 400) ? cm : -1;
}

// Median of valid pings. One or two missed pings retain the last distance;
// repeated no-echo cycles become out-of-range after about one second.
int readFilteredDistance() {
  int values[3], n = 0;
  for (int i = 0; i < 3; ++i) {
    int d = readDistanceOnce();
    lastPingCm[i] = d;
    if (d > 0) values[n++] = d;
    // HC-SR04 recommends at least 60 ms between trigger pulses.
    if (i < 2) delay(60);
  }
  if (!n) {
    if (consecutiveNoEchoSets < 255) ++consecutiveNoEchoSets;
    return (consecutiveNoEchoSets >= 4) ? 450 : filteredDistance;
  }
  consecutiveNoEchoSets = 0;
  for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j)
    if (values[j] < values[i]) { int t = values[i]; values[i] = values[j]; values[j] = t; }
  filteredDistance = values[n / 2];
  return filteredDistance;
}

bool connectWiFi(uint32_t timeoutMs = 9000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] connected ip=%s gateway=%s\n",
      WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
  } else {
    Serial.printf("[WiFi] connect failed status=%d\n", WiFi.status());
  }
  return WiFi.status() == WL_CONNECTED;
}

bool fetchRecords() {
  recordCount = 0;
  if (!connectWiFi()) { serverUnavailable = true; return false; }
  HTTPClient http;
  String url = String(SERVER_BASE_URL) + "/api/patient/" + PATIENT_ID + "/records";
  http.setTimeout(5000);
  if (!http.begin(url)) { serverUnavailable = true; return false; }
  int code = http.GET();
  Serial.printf("[API] GET records -> HTTP %d\n", code);
  if (code != HTTP_CODE_OK) { http.end(); serverUnavailable = true; return false; }
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err || !doc["records"].is<JsonArray>()) { serverUnavailable = true; return false; }
  JsonArray arr = doc["records"].as<JsonArray>();
  recordCount = 0;
  for (JsonObject item : arr) {
    if (recordCount >= MAX_RECORDS) break;
    Record &r = records[recordCount++];
    r.date = item["date"] | ""; r.doctor = item["doctor"] | "";
    r.diagnosis = item["diagnosis"] | ""; r.notes = item["notes"] | "";
    r.id = item["id"] | -1;
    String source = item["source"] | "";
    r.isDemo = source == "demo";
    r.medicineName = "No medicine listed"; r.dosage = ""; r.frequency = ""; r.duration = "";
    JsonArray meds = item["medicines"].as<JsonArray>();
    if (!meds.isNull() && meds.size()) {
      JsonObject med = meds[0];
      r.medicineName = med["name"] | "Medicine"; r.dosage = med["dosage"] | "";
      r.frequency = med["frequency"] | ""; r.duration = med["duration"] | "";
    }
  }
  serverUnavailable = false;
  return true;
}

String shortText(String s, size_t maxChars) {
  s.replace('\n', ' ');
  if (s.length() > maxChars) return s.substring(0, maxChars - 1) + "~";
  return s;
}

void drawBrowse() {
  if (confirmView) {
    showLines("Print this slip?", "YES: print", "NO: go back"); return;
  }
  if (!recordCount) {
    showLines("NO SAVED REPORTS", serverUnavailable ? "Server unavailable" : "History is empty", "YES: retry sync", "NO: log out"); return;
  }
  if (selectedItem == recordCount) {
    showLines("MENU", "[PRINT HISTORY]", "YES: print all", "NEXT: refresh"); return;
  }
  if (selectedItem == recordCount + 1) {
    showLines("MENU", "[REFRESH RECORDS]", "YES: sync latest", "NO: log out"); return;
  }
  Record &r = records[selectedItem];
  showLines(String(selectedItem + 1) + "/" + String(recordCount) + (r.isDemo ? " DEMO" : ""),
    shortText(r.date + " " + r.diagnosis, 21),
    shortText(r.medicineName + " " + r.dosage, 21),
    shortText(r.frequency + " " + r.duration, 21));
}

bool postSlip(bool allHistory, size_t index) {
  if (serverUnavailable || !connectWiFi()) return false;
  HTTPClient http;
  http.setTimeout(7000);
  if (!http.begin(String(SERVER_BASE_URL) + "/api/print-slip")) return false;
  http.addHeader("Content-Type", "application/json");
  // This workspace's server uses case-sensitive "Bedu" and record_ids.
  // Its print-slip endpoint does not accept record_index.
  String body = String("{\"patient_id\":\"") + PATIENT_ID + "\",\"record_ids\":[";
  bool first = true;
  if (allHistory) {
    for (size_t i = 0; i < recordCount; ++i) if (records[i].id >= 0) {
      if (!first) body += ","; body += String(records[i].id); first = false;
    }
  } else if (index < recordCount && records[index].id >= 0) {
    body += String(records[index].id); first = false;
  }
  body += "]}";
  if (first) { http.end(); return false; }
  int code = http.POST(body); http.end();
  return code >= 200 && code < 300;
}

void showPrintResult(bool success) {
  if (success) {
    setLeds(false, true, false); showLines("Slip sent to", "desk printer!");
  } else showLines("Print unavailable", "Check Wi-Fi/server");
  delay(2000);
  setLeds(false, true, false);
}

void beginAuthentication() {
  setState(STATE_AUTHENTICATING); setLeds(true, false, false);
  for (int i = 0; i < 3; ++i) {
    String dots = ""; for (int j = 0; j <= i; ++j) dots += ".";
    showLines("AUTHENTICATING", String("Please wait") + dots);
    delay(350);
  }
  fetchRecords();
  setLeds(true, false, false); showLines("AUTHENTICATED", "WELCOME, Bedu"); delay(1500);
  selectedItem = 0; confirmView = false; setState(STATE_BROWSE); drawBrowse();
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  pinMode(TOUCH_PIN, TOUCH_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT); pinMode(LED_BLUE, OUTPUT);
  pinMode(ENC_CLK, INPUT_PULLUP); pinMode(ENC_DT, INPUT_PULLUP); pinMode(ENC_SW, INPUT_PULLUP);
  yesButton.begin(); noButton.begin();
  Wire.begin(OLED_SDA, OLED_SCL);
  Serial.println("[OLED] I2C scan:");
  bool i2cDeviceFound = false;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[OLED] I2C device at 0x%02X\n", address);
      i2cDeviceFound = true;
    }
  }
  if (!i2cDeviceFound) Serial.println("[OLED] no I2C devices found on GPIO21/22");
  display.setI2CAddress(OLED_ADDR << 1); // U8g2 accepts the 8-bit form of 0x3C.
  display.begin();
  display.setFlipMode(1); // Explicit 180-degree controller flip in addition to R2 drawing rotation.
  Serial.println("[OLED] firmware orientation=180");
  display.setPowerSave(0);
  showLines("VISRODECK", "OLED ready", "Checking HC-SR04...");
  encoderPrevious = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT), encoderISR, CHANGE);
  setState(STATE_IDLE);
}

void loop() {
  uint32_t now = millis();
  if (now - lastDistanceRead >= 300) {
    lastDistanceRead = now;
    int distance = readFilteredDistance();
    Serial.printf("[HC-SR04] pings=%d,%d,%d cm filtered=%d cm\n",
      lastPingCm[0], lastPingCm[1], lastPingCm[2], distance);
    if (distance >= 0) {
      if (distance <= 400) sensorHasValidReading = true;
      if (distance <= APPROACH_DISTANCE_CM) farSince = 0;
      else if (!farSince) farSince = now;
    }
    if (state == STATE_IDLE && distance > 400) {
      display.setPowerSave(0);
      if (!sensorWarningVisible) {
        showLines("HC-SR04 no echo", "TRIG: GPIO5", "ECHO: GPIO18", "Check 5V/GND/divider");
        sensorWarningVisible = true;
      }
    } else if (state == STATE_IDLE && distance >= 0 && distance <= APPROACH_DISTANCE_CM) {
      setState(STATE_APPROACH);
    } else if (state == STATE_IDLE && distance > APPROACH_DISTANCE_CM && distance < 400) {
      if (sensorWarningVisible) showIdleScreen();
      sensorWarningVisible = false;
    }
    if (state == STATE_APPROACH && farSince && now - farSince >= AWAY_APPROACH_MS) setState(STATE_IDLE);
  }

  // Let a touch signal start demo login even if the ultrasonic sensor has no echo.
  // INPUT_PULLDOWN keeps an unconnected GPIO4 from floating HIGH.
  bool touchLevelHigh = digitalRead(TOUCH_PIN) == HIGH;
  if (!touchRawInitialized || touchLevelHigh != touchLastRawHigh) {
    Serial.printf("[TOUCH] GPIO4 raw=%s\n", touchLevelHigh ? "HIGH" : "LOW");
    touchLastRawHigh = touchLevelHigh;
    touchRawInitialized = true;
  }
  bool touchActive = TOUCH_ACTIVE_HIGH ? touchLevelHigh : !touchLevelHigh;
  if (state == STATE_IDLE || state == STATE_APPROACH) {
    bool buttonLogin = yesButton.pressed();
    bool touchLogin = TOUCH_LOGIN_ENABLED && touchActive && !touchWasHigh;
    if (buttonLogin || touchLogin) {
      if (state == STATE_IDLE) setState(STATE_APPROACH);
      Serial.printf("[LOGIN] start via %s\n", buttonLogin ? "YES button" : "touch sensor");
      beginAuthentication();
    }
  }
  touchWasHigh = touchActive;

  if (state == STATE_BROWSE) {
    int32_t steps;
    portENTER_CRITICAL(&encoderMux); steps = encoderSteps; encoderSteps = 0; portEXIT_CRITICAL(&encoderMux);
    if (steps) {
      int direction = (steps > 0) ? 1 : -1;
      int32_t count = abs(steps);
      size_t itemCount = recordCount ? recordCount + 2 : 1;
      while (count--) {
        if (direction > 0) {
          if (selectedItem + 1 < itemCount) ++selectedItem;
        } else if (selectedItem > 0) {
          --selectedItem;
        }
      }
      Serial.printf("[ENCODER] steps=%ld selection=%u/%u\n",
        (long)steps, (unsigned)(selectedItem + 1), (unsigned)itemCount);
      drawBrowse();
    }
    bool yes = yesButton.pressed(), no = noButton.pressed();
    if (no) {
      if (confirmView) { confirmView = false; drawBrowse(); }
      else { setState(STATE_APPROACH); farSince = 0; }
    } else if (yes) {
      if (confirmView) {
        bool ok = postSlip(false, selectedItem); confirmView = false; showPrintResult(ok); drawBrowse();
      } else if (!recordCount) {
        bool ok = fetchRecords(); selectedItem = 0;
        showLines(ok && recordCount ? "REPORTS SYNCED" : "NO SAVED REPORTS",
          serverUnavailable ? "Check Wi-Fi/server" : "History is empty");
        delay(1100); drawBrowse();
      } else if (selectedItem == recordCount) {
        bool ok = postSlip(true, 0); showPrintResult(ok); drawBrowse();
      } else if (selectedItem == recordCount + 1) {
        bool ok = fetchRecords(); selectedItem = 0;
        showLines(ok ? "REPORTS SYNCED" : "SYNC FAILED",
          serverUnavailable ? "Check Wi-Fi/server" : "Newest report loaded");
        delay(1100); drawBrowse();
      } else { confirmView = true; drawBrowse(); }
    }
  }
  delay(5);
}
