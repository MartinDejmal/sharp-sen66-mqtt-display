/*
 * ============================================================
 *  Sharp Memory LCD + SEN66 Sensor + MQTT (Home Assistant)
 *  ESP32-C3-MINI1 | PlatformIO + Arduino Framework
 * ============================================================
 *  Verze: 2.2.0
 *  Datum: 2026-08-11
 *
 *  Piny:
 *    SPI (displej): CLK=6, MOSI=7, MISO=2, CS=3
 *    I2C (SEN66):   SDA=10, SCL=8
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include <SensirionI2cSen66.h>
#include <ArduinoJson.h>
#include "config.h"
#include "WifiProvisioning.h"

#define FW_VERSION "2.2.0"

// =============================================
//  HARDWARE
// =============================================

// SPI piny (Sharp LCD)
#define PIN_SPI_CLK   6
#define PIN_SPI_MOSI  7
#define PIN_SPI_MISO  2   // nepouzity - displej je write-only
#define PIN_SPI_CS    3

// I2C piny (SEN66)
#define PIN_SDA  10
#define PIN_SCL  8

// Displej
#define DISPLAY_WIDTH  400
#define DISPLAY_HEIGHT 240
#define BLACK 0
#define WHITE 1

// Intervaly (ms)
#define SENSOR_READ_INTERVAL     2000
#define MQTT_RECONNECT_INTERVAL  5000

// =============================================
//  GLOBALNI OBJEKTY
// =============================================

Adafruit_SharpMem display(PIN_SPI_CLK, PIN_SPI_MOSI, PIN_SPI_CS,
                          DISPLAY_WIDTH, DISPLAY_HEIGHT);
SensirionI2cSen66 sen66;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer webServer(80);
WifiProvisioning wifiProvisioning;

AppConfig appConfig;

// =============================================
//  MQTT TOPICS (skladaji se za behu z mqttBaseTopic)
// =============================================

String deviceId;  // posledni 3 byty MAC, napr. "a1b2c3"

String topicText, topicClear, topicCommand;
String topicStatus, topicSensor;
String topicTemp, topicHumidity, topicPm1, topicPm25, topicPm4, topicPm10;
String topicVoc, topicNox, topicCo2;
String topicHaStatus;

void buildTopics() {
  const String& b = appConfig.mqttBaseTopic;

  topicText     = b + "/display/text";
  topicClear    = b + "/display/clear";
  topicCommand  = b + "/display/command";

  topicStatus   = b + "/status";
  topicSensor   = b + "/sensor";
  topicTemp     = b + "/sensor/temperature";
  topicHumidity = b + "/sensor/humidity";
  topicPm1      = b + "/sensor/pm1";
  topicPm25     = b + "/sensor/pm25";
  topicPm4      = b + "/sensor/pm4";
  topicPm10     = b + "/sensor/pm10";
  topicVoc      = b + "/sensor/voc";
  topicNox      = b + "/sensor/nox";
  topicCo2      = b + "/sensor/co2";

  topicHaStatus = appConfig.haPrefix + "/status";
}

// =============================================
//  DATA SENZORU
// =============================================

struct SensorData {
  float pm1   = 0.0f;
  float pm25  = 0.0f;
  float pm4   = 0.0f;
  float pm10  = 0.0f;
  float temperature = 0.0f;
  float humidity    = 0.0f;
  float voc   = 0.0f;
  float nox   = 0.0f;
  uint16_t co2 = 0;
  bool valid    = false;   // teplota / vlhkost / PM / VOC / NOx
  bool co2Valid = false;   // CO2 se validuje zvlast (nabiha pozdeji)
} sensorData;

// =============================================
//  STAV APLIKACE
// =============================================

unsigned long lastSensorRead = 0;
unsigned long lastMqttPublish = 0;
unsigned long lastDisplayRefresh = 0;
unsigned long lastMqttReconnect = 0;
unsigned long lastTmepRequest = 0;
unsigned long firstValidSensorAt = 0;

String lastTmepStatus = "TMEP:---";

bool sen66Ready = false;
bool displayOverride = false;
unsigned long displayOverrideUntil = 0;

String overrideText = "";
int overrideTextSize = 2;
int overrideX = 10;
int overrideY = 10;

// =============================================
//  DISPLAY - BARVY A POMOCNE FUNKCE
// =============================================

// Sharp Memory LCD nema HW inverzi, delame ji softwarove prohozenim barev.
static inline uint16_t inkColor() {
  return appConfig.displayInvertRequested ? WHITE : BLACK;
}

void clearCanvas() {
  display.clearDisplay();
  if (appConfig.displayInvertRequested) {
    display.fillRect(0, 0, display.width(), display.height(), BLACK);
  }
  display.setTextColor(inkColor());
}

void applyDisplaySettings() {
  display.setRotation(appConfig.displayRotation % 4);
  display.setTextColor(inkColor());
  Serial.printf("Display: rotace=%u, inverze=%s\n",
                appConfig.displayRotation % 4,
                appConfig.displayInvertRequested ? "ano (SW)" : "ne");
}

void drawCenteredText(const char* text, int y, int textSize) {
  display.setTextSize(textSize);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((DISPLAY_WIDTH - (int)w) / 2, y);
  display.print(text);
}

void drawRightAlignedText(const char* text, int y, int textSize) {
  display.setTextSize(textSize);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(DISPLAY_WIDTH - (int)w - 5, y);
  display.print(text);
}

void drawDividerLine(int y) {
  display.drawLine(5, y, DISPLAY_WIDTH - 5, y, inkColor());
}

// Ikona teplomeru
void drawThermIcon(int x, int y) {
  display.drawCircle(x + 3, y + 12, 4, inkColor());
  display.drawRect(x + 1, y, 5, 12, inkColor());
  display.fillCircle(x + 3, y + 12, 3, inkColor());
}

// Ikona kapky (vlhkost)
void drawDropIcon(int x, int y) {
  const uint16_t c = inkColor();
  display.drawPixel(x + 3, y, c);
  display.drawLine(x + 2, y + 1, x + 4, y + 1, c);
  display.drawLine(x + 1, y + 2, x + 5, y + 2, c);
  display.drawLine(x, y + 3, x + 6, y + 3, c);
  display.drawLine(x, y + 4, x + 6, y + 4, c);
  display.drawLine(x, y + 5, x + 6, y + 5, c);
  display.drawLine(x + 1, y + 6, x + 5, y + 6, c);
  display.drawLine(x + 2, y + 7, x + 4, y + 7, c);
}

// =============================================
//  VALIDACE DAT SENZORU
// =============================================

static inline bool inRange(float v, float lo, float hi) {
  return !isnan(v) && v >= lo && v <= hi;
}

// Teplota/vlhkost/PM/VOC/NOx. CO2 se resi zvlast, protoze nabiha az po ~1 minute
// a driv vraci 0 nebo 0xFFFF - drive to shodilo cely vzorek vcetne teploty.
bool coreValuesLookValid(float pm1, float pm25, float pm4, float pm10,
                         float hum, float temp, float voc, float nox) {
  if (!inRange(temp, -40.0f, 85.0f)) return false;
  if (!inRange(hum, 0.0f, 100.0f)) return false;
  if (!inRange(pm1, 0.0f, 1000.0f)) return false;
  if (!inRange(pm25, 0.0f, 1000.0f)) return false;
  if (!inRange(pm4, 0.0f, 1000.0f)) return false;
  if (!inRange(pm10, 0.0f, 1000.0f)) return false;
  if (!inRange(voc, 0.0f, 500.0f)) return false;
  if (!inRange(nox, 0.0f, 500.0f)) return false;
  return true;
}

// SEN66 vraci 0xFFFF, dokud CO2 neni k dispozici. Merici rozsah je 400-5000 ppm,
// dolni mez volime 300 kvuli ASC kalibraci, horni 10000 kvuli spickam.
bool co2LooksValid(uint16_t co2) {
  return co2 >= 300 && co2 <= 10000;
}

// =============================================
//  DISPLAY - HLAVNI OBRAZOVKY
// =============================================

// Hodnoceni kvality vzduchu podle PM2.5
const char* getAirQuality(float pm25) {
  if (pm25 < 12.0)  return "VYNIKAJICI";
  if (pm25 < 35.4)  return "DOBRE";
  if (pm25 < 55.4)  return "PRIJATELNE";
  if (pm25 < 150.4) return "SPATNE";
  if (pm25 < 250.4) return "VELMI SPATNE";
  return "NEBEZPECNE";
}

void drawSensorScreen() {
  clearCanvas();

  char buf[64];

  // === STATUS BAR ===
  display.setTextSize(1);

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "WiFi:%s", WiFi.localIP().toString().c_str());
  } else if (wifiProvisioning.isCaptiveMode()) {
    snprintf(buf, sizeof(buf), "AP:%s", wifiProvisioning.getApIp().c_str());
  } else {
    snprintf(buf, sizeof(buf), "WiFi:---");
  }
  display.setCursor(5, 5);
  display.print(buf);

  display.setCursor(165, 5);
  display.print(lastTmepStatus);

  display.setCursor(240, 5);
  display.print(mqtt.connected() ? "MQTT:OK" : "MQTT:---");

  display.setCursor(315, 5);
  display.print(sen66Ready ? "SEN66:OK" : "SEN66:---");

  unsigned long uptimeSec = millis() / 1000;
  unsigned long hrs = uptimeSec / 3600;
  unsigned long mins = (uptimeSec % 3600) / 60;
  snprintf(buf, sizeof(buf), "%luh%02lum", hrs, mins);
  drawRightAlignedText(buf, 5, 1);

  drawDividerLine(18);

  if (!sensorData.valid) {
    drawCenteredText("Cekam na data", 80, 2);
    drawCenteredText("ze senzoru SEN66...", 110, 2);
    display.refresh();
    return;
  }

  // === TEPLOTA & VLHKOST ===
  drawThermIcon(15, 28);
  snprintf(buf, sizeof(buf), "%.1f", sensorData.temperature);
  display.setTextSize(4);
  display.setCursor(35, 25);
  display.print(buf);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(buf, 35, 25, &x1, &y1, &w, &h);
  display.setTextSize(2);
  display.setCursor(35 + w + 5, 25);
  display.print("o");
  display.setCursor(35 + w + 5, 40);
  display.print("C");

  drawDropIcon(220, 28);
  snprintf(buf, sizeof(buf), "%.1f", sensorData.humidity);
  display.setTextSize(4);
  display.setCursor(240, 25);
  display.print(buf);
  display.getTextBounds(buf, 240, 25, &x1, &y1, &w, &h);
  display.setTextSize(2);
  display.setCursor(240 + w + 5, 30);
  display.print("%");

  drawDividerLine(68);

  // === PM HODNOTY ===
  display.setTextSize(1);
  display.setCursor(15, 74);
  display.print("PM1.0");
  display.setCursor(115, 74);
  display.print("PM2.5");
  display.setCursor(215, 74);
  display.print("PM4.0");
  display.setCursor(315, 74);
  display.print("PM10");

  display.setTextSize(3);
  snprintf(buf, sizeof(buf), "%.0f", sensorData.pm1);
  display.setCursor(10, 90);
  display.print(buf);

  snprintf(buf, sizeof(buf), "%.0f", sensorData.pm25);
  display.setCursor(110, 90);
  display.print(buf);

  snprintf(buf, sizeof(buf), "%.0f", sensorData.pm4);
  display.setCursor(210, 90);
  display.print(buf);

  snprintf(buf, sizeof(buf), "%.0f", sensorData.pm10);
  display.setCursor(310, 90);
  display.print(buf);

  display.setTextSize(1);
  static const int pmUnitX[4] = {15, 115, 215, 315};
  for (uint8_t i = 0; i < 4; i++) {
    display.setCursor(pmUnitX[i], 118);
    display.print("ug/m3");
  }

  drawDividerLine(132);

  // === VOC, NOx, CO2 ===
  display.setTextSize(1);
  display.setCursor(15, 138);
  display.print("VOC Index");
  display.setCursor(155, 138);
  display.print("NOx Index");
  display.setCursor(295, 138);
  display.print("CO2");

  display.setTextSize(3);
  snprintf(buf, sizeof(buf), "%.0f", sensorData.voc);
  display.setCursor(15, 152);
  display.print(buf);

  snprintf(buf, sizeof(buf), "%.0f", sensorData.nox);
  display.setCursor(155, 152);
  display.print(buf);

  if (sensorData.co2Valid) {
    snprintf(buf, sizeof(buf), "%u", sensorData.co2);
  } else {
    snprintf(buf, sizeof(buf), "---");
  }
  display.setCursor(280, 152);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(350, 170);
  display.print("ppm");

  drawDividerLine(185);

  // === AIR QUALITY BAR ===
  const char* quality = getAirQuality(sensorData.pm25);
  display.setTextSize(1);
  display.setCursor(15, 192);
  display.print("Kvalita vzduchu:");

  display.setTextSize(3);
  display.setCursor(15, 208);
  display.print(quality);

  float barValue = min(sensorData.pm25 / 150.0f, 1.0f);
  int barWidth = (int)(barValue * 120);
  display.drawRect(270, 200, 122, 24, inkColor());
  if (barWidth > 0) display.fillRect(271, 201, barWidth, 22, inkColor());

  display.refresh();
}

void drawCustomTextScreen() {
  clearCanvas();
  display.setTextSize(overrideTextSize);
  display.setCursor(overrideX, overrideY);
  display.println(overrideText);
  display.refresh();
}

void drawSplashScreen() {
  clearCanvas();

  drawCenteredText("Sharp LCD + SEN66", 40, 3);
  drawCenteredText("MQTT Dashboard", 80, 2);

  drawDividerLine(110);

  display.setTextSize(1);
  display.setCursor(30, 125);
  display.print("FW:   ");
  display.print(FW_VERSION);

  display.setCursor(30, 140);
  display.print("WiFi: ");
  display.print(appConfig.wifiSsid.length() ? appConfig.wifiSsid : String("(nenastaveno)"));

  display.setCursor(30, 155);
  display.print("MQTT: ");
  display.print(appConfig.mqttServer.length() ? appConfig.mqttServer : String("(nenastaveno)"));

  display.setCursor(30, 175);
  display.print("Inicializace...");

  display.refresh();
}

// =============================================
//  SEN66 SENZOR
// =============================================

#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

void initSEN66() {
  Serial.println("SEN66: Inicializace I2C...");
  Wire.begin(PIN_SDA, PIN_SCL);

  sen66.begin(Wire, SEN66_I2C_ADDR_6B);

  int16_t error = sen66.deviceReset();
  if (error != NO_ERROR) {
    char msg[64];
    errorToString(error, msg, sizeof(msg));
    Serial.printf("SEN66: deviceReset() CHYBA: %s\n", msg);
    sen66Ready = false;
    return;
  }

  delay(1200);  // SEN66 potrebuje cas po resetu

  int8_t serialNumber[32] = {0};
  error = sen66.getSerialNumber(serialNumber, 32);
  if (error != NO_ERROR) {
    char msg[64];
    errorToString(error, msg, sizeof(msg));
    Serial.printf("SEN66: getSerialNumber() CHYBA: %s\n", msg);
  } else {
    Serial.printf("SEN66: S/N: %s\n", (const char*)serialNumber);
  }

  error = sen66.startContinuousMeasurement();
  if (error != NO_ERROR) {
    char msg[64];
    errorToString(error, msg, sizeof(msg));
    Serial.printf("SEN66: startContinuousMeasurement() CHYBA: %s\n", msg);
    sen66Ready = false;
    return;
  }

  sen66Ready = true;
  Serial.println("SEN66: OK, mereni spusteno!");
}

void readSEN66() {
  if (!sen66Ready) return;

  float pm1, pm25, pm4, pm10, hum, temp, voc, nox;
  uint16_t co2;

  int16_t error = sen66.readMeasuredValues(
      pm1, pm25, pm4, pm10, hum, temp, voc, nox, co2);

  if (error != NO_ERROR) {
    char msg[64];
    errorToString(error, msg, sizeof(msg));
    Serial.printf("SEN66: readMeasuredValues() CHYBA: %s\n", msg);
    return;
  }

  if (!coreValuesLookValid(pm1, pm25, pm4, pm10, hum, temp, voc, nox)) {
    Serial.println("SEN66: neplatna data (T/H/PM/VOC/NOx), vzorek preskocen");
    return;
  }

  sensorData.pm1  = pm1;
  sensorData.pm25 = pm25;
  sensorData.pm4  = pm4;
  sensorData.pm10 = pm10;
  sensorData.temperature = temp + appConfig.temperatureOffset;
  sensorData.humidity    = hum;
  sensorData.voc  = voc;
  sensorData.nox  = nox;
  sensorData.valid = true;

  if (co2LooksValid(co2)) {
    sensorData.co2 = co2;
    sensorData.co2Valid = true;
  } else {
    sensorData.co2Valid = false;
  }

  if (firstValidSensorAt == 0) firstValidSensorAt = millis();

  Serial.printf("SEN66: T(raw)=%.1f T(adj)=%.1f H=%.1f PM2.5=%.1f VOC=%.0f NOx=%.0f CO2=%s\n",
                temp, sensorData.temperature, hum, pm25, voc, nox,
                sensorData.co2Valid ? String(sensorData.co2).c_str() : "n/a");
}

// =============================================
//  TMEP.CZ
// =============================================

String formatFloat1(const float value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", value);
  return String(buf);
}

String formatFloat0(const float value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.0f", value);
  return String(buf);
}

void replaceAllTokens(String& target, const String& token, const String& value) {
  target.replace("*" + token + "*", value);
  target.replace("{" + token + "}", value);
}

String buildTmepQueryParams() {
  String params = appConfig.tmepParams;

  // Hodnoty ze senzoru
  replaceAllTokens(params, "TEMP", formatFloat1(sensorData.temperature));
  replaceAllTokens(params, "HUM", formatFloat1(sensorData.humidity));
  replaceAllTokens(params, "PM1", formatFloat1(sensorData.pm1));
  replaceAllTokens(params, "PM2", formatFloat1(sensorData.pm25));
  replaceAllTokens(params, "PM4", formatFloat1(sensorData.pm4));
  replaceAllTokens(params, "PM10", formatFloat1(sensorData.pm10));
  replaceAllTokens(params, "VOC", formatFloat0(sensorData.voc));
  replaceAllTokens(params, "NOX", formatFloat0(sensorData.nox));
  replaceAllTokens(params, "CO2", sensorData.co2Valid ? String(sensorData.co2) : String(""));

  // Systemove hodnoty (README je dokumentoval, implementace chybela)
  replaceAllTokens(params, "RSSI", WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String(""));
  replaceAllTokens(params, "UPTIME", String(millis() / 1000));
  replaceAllTokens(params, "FREEHEAP", String(ESP.getFreeHeap()));
  replaceAllTokens(params, "IP", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(""));

  return params;
}

String buildTmepRequestUrl() {
  if (appConfig.tmepDomain.length() == 0 || appConfig.tmepParams.length() == 0 || !sensorData.valid) {
    return "";
  }
  return "http://" + appConfig.tmepDomain + ".tmep.cz/?" + buildTmepQueryParams();
}

bool sendTmepRequest(const bool manualTrigger) {
  if (appConfig.tmepDomain.length() == 0 || appConfig.tmepParams.length() == 0) {
    lastTmepStatus = "TMEP:OFF";
    return false;
  }
  if (!sensorData.valid) {
    Serial.println("TMEP: nejsou validni data senzoru, request preskocen");
    lastTmepStatus = "TMEP:SKIP";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("TMEP: WiFi neni pripojena, request preskocen");
    lastTmepStatus = "TMEP:SKIP";
    return false;
  }

  String url = buildTmepRequestUrl();
  if (url.length() == 0) {
    lastTmepStatus = "TMEP:SKIP";
    return false;
  }

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(url)) {
    Serial.println("TMEP: Nelze inicializovat HTTP request");
    lastTmepStatus = "TMEP:ERR";
    return false;
  }

  int httpCode = http.GET();
  String response = http.getString();
  http.end();

  if (httpCode > 0 && httpCode < 400) {
    Serial.printf("TMEP: %srequest OK, HTTP %d\n", manualTrigger ? "manual " : "", httpCode);
    lastTmepStatus = "TMEP:OK";
    return true;
  }

  Serial.printf("TMEP: %srequest CHYBA, HTTP %d, body: %s\n",
                manualTrigger ? "manual " : "", httpCode, response.c_str());
  lastTmepStatus = "TMEP:ERR";
  return false;
}

// =============================================
//  WEB SERVER
// =============================================

// V captive rezimu autentizaci nevyzadujeme, jinak by se uzivatel nedostal
// k prvotnimu nastaveni. V STA rezimu chrani heslo, pokud je nastavene.
bool requireAuth() {
  if (wifiProvisioning.isCaptiveMode()) return false;
  if (appConfig.webPassword.length() == 0) return false;
  if (!webServer.authenticate(appConfig.webUser.c_str(), appConfig.webPassword.c_str())) {
    webServer.requestAuthentication();
    return true;
  }
  return false;
}

void sendJson(int code, JsonDocument& doc) {
  String payload;
  serializeJson(doc, payload);  // String roste dle potreby, nic se neorizne
  webServer.send(code, "application/json", payload);
}

void handleWebRoot() {
  if (requireAuth()) return;

  const char* html = R"HTML(
<!doctype html><html lang="cs"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SEN66 panel</title>
<style>
body{font-family:Arial,sans-serif;margin:0;background:#f3f5f7;color:#222}header{background:#0f172a;color:#fff;padding:12px 16px}main{padding:16px;max-width:980px;margin:0 auto}
.tabs{display:flex;gap:8px;margin-bottom:12px}.tab{padding:10px 14px;border:0;border-radius:8px;background:#dbe2ea;cursor:pointer}.tab.active{background:#2563eb;color:#fff}
.panel{display:none;background:#fff;padding:16px;border-radius:10px;box-shadow:0 1px 3px rgba(0,0,0,.15)}.panel.active{display:block}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px}.card{border:1px solid #e5e7eb;border-radius:8px;padding:10px}
label{display:block;font-size:14px;margin-top:8px}input{width:100%;padding:8px;border:1px solid #cbd5e1;border-radius:6px;box-sizing:border-box}
button.save,button.secondary,button.warn{margin-top:12px;padding:10px 14px;color:#fff;border:0;border-radius:8px;cursor:pointer}
button.save{background:#16a34a}button.secondary{background:#2563eb}button.warn{background:#b91c1c}
.muted{color:#666;font-size:13px}.ok{color:#166534}.err{color:#b91c1c}code.url{display:block;padding:8px;background:#f1f5f9;border-radius:6px;word-break:break-all;font-size:13px}
</style></head><body><header><h2>SEN66 MQTT displej</h2></header><main>
<div class="tabs"><button class="tab active" data-tab="data">Aktuální data</button><button class="tab" data-tab="cfg">Konfigurace</button></div>
<section id="data" class="panel active"><div class="grid" id="cards"></div><p class="muted" id="status"></p></section>
<section id="cfg" class="panel"><form id="cfgForm"><h3>Wi-Fi</h3>
<p class="muted" id="wifiMode"></p><p class="muted" id="wifiConn"></p>
<label>SSID<input name="wifiSsid" required></label>
<label>Heslo<input type="password" name="wifiPassword" id="wifiPass" placeholder="(beze změny)"></label>
<label><input id="showPass" type="checkbox" style="width:auto"> Zobrazit heslo</label>
<button id="wifiOnlySaveBtn" class="secondary" type="button">Uložit jen Wi-Fi a restartovat</button>
<button id="wifiForgetBtn" class="warn" type="button">Zapomenout Wi-Fi</button><p class="muted" id="wifiMsg"></p>
<h3>MQTT</h3><label>Server<input name="mqttServer" placeholder="192.168.1.10"></label><label>Port<input type="number" min="1" max="65535" name="mqttPort" required></label><label>Uživatel<input name="mqttUser"></label><label>Heslo<input type="password" name="mqttPassword" placeholder="(beze změny)"></label>
<label>Client ID<input name="mqttClientId"></label><label>Base topic<input name="mqttBaseTopic" placeholder="sharp"></label><label>Discovery prefix Home Assistantu<input name="haPrefix" placeholder="homeassistant"></label>
<h3>Zabezpečení web UI</h3><label>Uživatel<input name="webUser" placeholder="admin"></label><label>Heslo (prázdné pole = beze změny)<input type="password" name="webPassword" placeholder="(beze změny)"></label>
<h3>TMEP.cz</h3><label>Doména pro zasílání hodnot<input name="tmepDomain" placeholder="xxk4sk-g6rxfh"></label><label>Parametry požadavku<input name="tmepParams" placeholder="tempV=*TEMP*&humV=*HUM*&co2=*CO2*"></label>
<p class="muted">Proměnné: *TEMP*, *HUM*, *PM1*, *PM2*, *PM4*, *PM10*, *VOC*, *NOX*, *CO2*, *RSSI*, *UPTIME*, *FREEHEAP*, *IP*.</p><p class="muted">Reálné URL volané na TMEP.cz:</p><code id="tmepUrl" class="url muted">Není dostupné</code>
<button id="tmepSendBtn" class="secondary" type="button">Odeslat TMEP request ručně</button><p id="tmepMsg" class="muted"></p>
<h3>Displej</h3><label>Rotace (0-3)<input type="number" min="0" max="3" name="displayRotation" required></label><label>Inverze (0/1)<input type="number" min="0" max="1" name="displayInvertRequested" required></label>
<h3>Intervaly (ms)</h3><label>Překreslení displeje<input type="number" min="500" name="displayRefreshInterval" required></label><label>MQTT publish<input type="number" min="1000" name="mqttPublishInterval" required></label><label>TMEP request interval<input type="number" min="1000" name="tmepRequestInterval" required></label><label>MQTT warmup delay<input type="number" min="1000" name="mqttWarmupDelay" required></label>
<h3>Kalibrace</h3><label>Teplotní offset (°C)<input type="number" step="0.1" min="-20" max="20" name="temperatureOffset" required></label><p class="muted">Hodnota se přičítá k naměřené teplotě.</p>
<button class="save" type="submit">Uložit plnou konfiguraci</button><p id="cfgMsg" class="muted"></p></form></section></main>
<script>
const NUM=['mqttPort','displayRotation','displayInvertRequested','displayRefreshInterval','mqttPublishInterval','tmepRequestInterval','mqttWarmupDelay','temperatureOffset'];
const tabs=document.querySelectorAll('.tab');tabs.forEach(t=>t.onclick=()=>{tabs.forEach(x=>x.classList.remove('active'));document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));t.classList.add('active');document.getElementById(t.dataset.tab).classList.add('active')});
function setMsg(id,text,ok){const m=document.getElementById(id);m.textContent=text;m.className=ok?'ok':'err'}
async function loadData(){try{const r=await fetch('/api/data');const d=await r.json();const cards=document.getElementById('cards');cards.innerHTML='';for(const [k,v] of Object.entries(d.values)){const c=document.createElement('div');c.className='card';const t=document.createElement('strong');t.textContent=k;const val=document.createElement('div');val.textContent=v;c.appendChild(t);c.appendChild(val);cards.appendChild(c)}
document.getElementById('status').textContent='WiFi: '+d.wifi+' | režim: '+d.wifiMode+' | TMEP: '+d.tmepStatus+' | MQTT: '+d.mqtt+' | validní data: '+d.valid+' | uptime: '+d.uptime+' s | FW '+d.fw;
document.getElementById('wifiMode').textContent='Režim: '+d.wifiMode+(d.apSsid?(' | AP: '+d.apSsid+' @ '+d.apIp):'');
document.getElementById('wifiConn').textContent='Aktuální SSID: '+(d.currentSsid||'-')+' | IP: '+(d.currentIp||'-')+' | RSSI: '+(d.rssi||'-')+' dBm';
const u=document.getElementById('tmepUrl');u.textContent=d.tmepUrl||'Není dostupné';u.className=d.tmepUrl?'url':'url muted'}catch(e){}}
async function loadCfg(){const r=await fetch('/api/config');const c=await r.json();const f=document.getElementById('cfgForm');Object.keys(c).forEach(k=>{const el=f[k];if(el&&'value' in el)el.value=c[k]})}
function buildPayload(f){const p=Object.fromEntries(new FormData(f).entries());
NUM.forEach(k=>{if(p[k]!==undefined&&p[k]!=='')p[k]=Number(p[k]);else delete p[k]});
['wifiPassword','mqttPassword','webPassword'].forEach(k=>{if(p[k]==='')delete p[k]});
return p}
document.getElementById('showPass').onchange=(e)=>{document.getElementById('wifiPass').type=e.target.checked?'text':'password'};
document.getElementById('cfgForm').onsubmit=async(e)=>{e.preventDefault();const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(buildPayload(e.target))});setMsg('cfgMsg',await r.text(),r.ok)};
document.getElementById('wifiOnlySaveBtn').onclick=async()=>{const f=document.getElementById('cfgForm');const p={wifiSsid:f.wifiSsid.value};if(f.wifiPassword.value!=='')p.wifiPassword=f.wifiPassword.value;const r=await fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});const d=await r.json();setMsg('wifiMsg',d.message||'?',r.ok)};
document.getElementById('wifiForgetBtn').onclick=async()=>{if(!confirm('Opravdu smazat uložené Wi-Fi údaje a restartovat do AP režimu?'))return;const r=await fetch('/api/wifi/forget',{method:'POST'});const d=await r.json();setMsg('wifiMsg',d.message||'?',r.ok)};
document.getElementById('tmepSendBtn').onclick=async()=>{const r=await fetch('/api/tmep/send',{method:'POST'});setMsg('tmepMsg',await r.text(),r.ok);await loadData()};
loadData();loadCfg();setInterval(loadData,2000);
</script></body></html>)HTML";

  webServer.send(200, "text/html; charset=utf-8", html);
}

void handleApiData() {
  if (requireAuth()) return;

  JsonDocument doc;
  doc["fw"] = FW_VERSION;
  doc["wifi"] = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  doc["mqtt"] = mqtt.connected() ? "connected" : "disconnected";
  doc["valid"] = sensorData.valid;
  doc["uptime"] = millis() / 1000;
  doc["tmepUrl"] = buildTmepRequestUrl();
  doc["tmepStatus"] = lastTmepStatus;
  doc["wifiMode"] = wifiProvisioning.getStateText();
  doc["apSsid"] = wifiProvisioning.isCaptiveMode() ? wifiProvisioning.getApSsid() : "";
  doc["apIp"] = wifiProvisioning.isCaptiveMode() ? wifiProvisioning.getApIp() : "";
  doc["currentSsid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  doc["currentIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;

  JsonObject values = doc["values"].to<JsonObject>();
  values["temperature"] = round(sensorData.temperature * 10) / 10.0;
  values["humidity"] = round(sensorData.humidity * 10) / 10.0;
  values["pm1"] = round(sensorData.pm1 * 10) / 10.0;
  values["pm25"] = round(sensorData.pm25 * 10) / 10.0;
  values["pm4"] = round(sensorData.pm4 * 10) / 10.0;
  values["pm10"] = round(sensorData.pm10 * 10) / 10.0;
  values["voc"] = round(sensorData.voc);
  values["nox"] = round(sensorData.nox);
  if (sensorData.co2Valid) {
    values["co2"] = sensorData.co2;
  } else {
    values["co2"] = "---";
  }

  sendJson(200, doc);
}

void handleApiConfigGet() {
  if (requireAuth()) return;

  JsonDocument doc;
  doc["wifiSsid"] = appConfig.wifiSsid;
  doc["mqttServer"] = appConfig.mqttServer;
  doc["mqttPort"] = appConfig.mqttPort;
  doc["mqttUser"] = appConfig.mqttUser;
  doc["mqttClientId"] = appConfig.mqttClientId;
  doc["mqttBaseTopic"] = appConfig.mqttBaseTopic;
  doc["haPrefix"] = appConfig.haPrefix;
  doc["webUser"] = appConfig.webUser;
  doc["tmepDomain"] = appConfig.tmepDomain;
  doc["tmepParams"] = appConfig.tmepParams;
  doc["displayRotation"] = appConfig.displayRotation;
  doc["displayInvertRequested"] = appConfig.displayInvertRequested ? 1 : 0;
  doc["displayRefreshInterval"] = appConfig.displayRefreshInterval;
  doc["mqttPublishInterval"] = appConfig.mqttPublishInterval;
  doc["tmepRequestInterval"] = appConfig.tmepRequestInterval;
  doc["mqttWarmupDelay"] = appConfig.mqttWarmupDelay;
  doc["temperatureOffset"] = appConfig.temperatureOffset;

  // Hesla se zasadne neposilaji zpet, jen priznak, zda jsou nastavena.
  doc["wifiPasswordSet"] = appConfig.wifiPassword.length() > 0;
  doc["mqttPasswordSet"] = appConfig.mqttPassword.length() > 0;
  doc["webPasswordSet"] = appConfig.webPassword.length() > 0;

  sendJson(200, doc);
}

// HTML formular posila vsechny hodnoty jako retezce. ArduinoJson 7 vraci
// u `variant | default` default, pokud typ nesedi - proto explicitni prevod.
template <typename T>
T readNumber(JsonVariantConst v, T fallback) {
  if (v.isNull()) return fallback;
  if (v.is<T>()) return v.as<T>();
  if (v.is<const char*>()) {
    String s = v.as<String>();
    s.trim();
    if (s.length() == 0) return fallback;
    return (T)s.toDouble();
  }
  if (v.is<float>()) return (T)v.as<float>();
  return fallback;
}

String readString(JsonVariantConst v, const String& fallback) {
  if (v.is<const char*>()) return v.as<String>();
  return fallback;
}

void handleApiConfigPost() {
  if (requireAuth()) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, webServer.arg("plain"));
  if (err) {
    webServer.send(400, "text/plain", "Neplatny JSON");
    return;
  }

  AppConfig updated = appConfig;

  updated.wifiSsid      = readString(doc["wifiSsid"], updated.wifiSsid);
  updated.mqttServer    = readString(doc["mqttServer"], updated.mqttServer);
  updated.mqttUser      = readString(doc["mqttUser"], updated.mqttUser);
  updated.mqttClientId  = readString(doc["mqttClientId"], updated.mqttClientId);
  updated.mqttBaseTopic = readString(doc["mqttBaseTopic"], updated.mqttBaseTopic);
  updated.haPrefix      = readString(doc["haPrefix"], updated.haPrefix);
  updated.webUser       = readString(doc["webUser"], updated.webUser);
  updated.tmepDomain    = readString(doc["tmepDomain"], updated.tmepDomain);
  updated.tmepParams    = readString(doc["tmepParams"], updated.tmepParams);

  // Hesla: klic nedorazi => beze zmeny, dorazi prazdny retezec => smazat.
  if (!doc["wifiPassword"].isNull()) updated.wifiPassword = doc["wifiPassword"].as<String>();
  if (!doc["mqttPassword"].isNull()) updated.mqttPassword = doc["mqttPassword"].as<String>();
  if (!doc["webPassword"].isNull()) updated.webPassword = doc["webPassword"].as<String>();

  updated.mqttPort        = readNumber<int>(doc["mqttPort"], updated.mqttPort);
  updated.displayRotation = (uint8_t)readNumber<int>(doc["displayRotation"], updated.displayRotation);
  updated.displayInvertRequested =
      readNumber<int>(doc["displayInvertRequested"], updated.displayInvertRequested ? 1 : 0) != 0;
  updated.displayRefreshInterval =
      readNumber<unsigned long>(doc["displayRefreshInterval"], updated.displayRefreshInterval);
  updated.mqttPublishInterval =
      readNumber<unsigned long>(doc["mqttPublishInterval"], updated.mqttPublishInterval);
  updated.tmepRequestInterval =
      readNumber<unsigned long>(doc["tmepRequestInterval"], updated.tmepRequestInterval);
  updated.mqttWarmupDelay =
      readNumber<unsigned long>(doc["mqttWarmupDelay"], updated.mqttWarmupDelay);
  updated.temperatureOffset =
      readNumber<float>(doc["temperatureOffset"], updated.temperatureOffset);

  if (!validateConfig(updated)) {
    webServer.send(400, "text/plain", "Neplatne hodnoty konfigurace");
    return;
  }

  if (!saveConfig(updated)) {
    webServer.send(500, "text/plain", "Nepodarilo se ulozit konfiguraci");
    return;
  }

  appConfig = updated;
  webServer.send(200, "text/plain", "Konfigurace ulozena, zarizeni se restartuje...");
  delay(300);
  ESP.restart();
}

void handleApiWifiSave() {
  if (requireAuth()) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, webServer.arg("plain"));
  JsonDocument out;

  if (err) {
    out["ok"] = false;
    out["message"] = "Neplatny JSON";
    sendJson(400, out);
    return;
  }

  AppConfig updated = appConfig;
  updated.wifiSsid = doc["wifiSsid"].as<String>();
  if (!doc["wifiPassword"].isNull()) updated.wifiPassword = doc["wifiPassword"].as<String>();

  if (updated.wifiSsid.length() == 0) {
    out["ok"] = false;
    out["message"] = "SSID nesmi byt prazdne";
    sendJson(400, out);
    return;
  }

  if (!saveConfig(updated)) {
    out["ok"] = false;
    out["message"] = "Nepodarilo se ulozit WiFi konfiguraci";
    sendJson(500, out);
    return;
  }

  appConfig = updated;
  out["ok"] = true;
  out["message"] = "Wi-Fi ulozena, restartuji a pripojuji se";
  sendJson(200, out);

  // Odpovime driv, nez zacneme cokoli blokujiciho - drive se 20s pokus
  // o pripojeni delal primo v handleru a prohlizeci vyprsel fetch.
  delay(300);
  ESP.restart();
}

void handleApiWifiForget() {
  if (requireAuth()) return;

  AppConfig updated = appConfig;
  updated.wifiSsid = "";
  updated.wifiPassword = "";
  const bool ok = saveConfig(updated);
  if (ok) appConfig = updated;

  JsonDocument out;
  out["ok"] = ok;
  out["message"] = ok ? "Wi-Fi zapomenuta, restartuji do captive AP"
                      : "Nepodarilo se zapomenout Wi-Fi";
  sendJson(ok ? 200 : 500, out);

  if (ok) {
    delay(300);
    ESP.restart();
  }
}

void handleApiTmepSend() {
  if (requireAuth()) return;

  if (sendTmepRequest(true)) {
    webServer.send(200, "text/plain", "TMEP request byl uspesne odeslan");
    return;
  }
  webServer.send(500, "text/plain",
                 "TMEP request se nepodarilo odeslat (zkontrolujte domenu, WiFi a data senzoru)");
}

void handleCaptiveRedirect() {
  if (!wifiProvisioning.isCaptiveMode()) {
    webServer.send(404, "text/plain", "Not found");
    return;
  }
  webServer.sendHeader("Location", String("http://") + wifiProvisioning.getApIp() + "/", true);
  webServer.send(302, "text/plain", "Redirecting to captive portal");
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleWebRoot);
  webServer.on("/api/data", HTTP_GET, handleApiData);
  webServer.on("/api/config", HTTP_GET, handleApiConfigGet);
  webServer.on("/api/config", HTTP_POST, handleApiConfigPost);
  webServer.on("/api/wifi/save", HTTP_POST, handleApiWifiSave);
  webServer.on("/api/wifi/forget", HTTP_POST, handleApiWifiForget);
  webServer.on("/api/tmep/send", HTTP_POST, handleApiTmepSend);

  webServer.on("/generate_204", HTTP_ANY, handleCaptiveRedirect);
  webServer.on("/gen_204", HTTP_ANY, handleCaptiveRedirect);
  webServer.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveRedirect);
  webServer.on("/library/test/success.html", HTTP_ANY, handleCaptiveRedirect);
  webServer.on("/connecttest.txt", HTTP_ANY, handleCaptiveRedirect);
  webServer.on("/ncsi.txt", HTTP_ANY, handleCaptiveRedirect);
  webServer.on("/redirect", HTTP_ANY, handleCaptiveRedirect);

  webServer.onNotFound([]() {
    if (wifiProvisioning.isCaptiveMode()) {
      handleCaptiveRedirect();
      return;
    }
    webServer.send(404, "text/plain", "Not found");
  });

  webServer.begin();
  Serial.println("WEB: Server bezi na portu 80");
}

// =============================================
//  MQTT - HOME ASSISTANT AUTO-DISCOVERY
// =============================================

void publishHADiscovery() {
  if (!mqtt.connected()) return;

  struct HASensor {
    const char* name;
    const char* uidSuffix;
    const String* topic;
    const char* unit;
    const char* devClass;
    const char* icon;
    uint8_t precision;
  };

  const HASensor sensors[] = {
    {"Teplota",   "temp",     &topicTemp,     "°C",    "temperature",    "mdi:thermometer",   1},
    {"Vlhkost",   "humidity", &topicHumidity, "%",     "humidity",       "mdi:water-percent", 1},
    {"PM1.0",     "pm1",      &topicPm1,      "µg/m³", "pm1",            "mdi:blur",          1},
    {"PM2.5",     "pm25",     &topicPm25,     "µg/m³", "pm25",           "mdi:blur",          1},
    {"PM4.0",     "pm4",      &topicPm4,      "µg/m³", nullptr,          "mdi:blur-radial",   1},
    {"PM10",      "pm10",     &topicPm10,     "µg/m³", "pm10",           "mdi:blur-radial",   1},
    {"VOC Index", "voc",      &topicVoc,      nullptr, "aqi",            "mdi:air-filter",    0},
    {"NOx Index", "nox",      &topicNox,      nullptr, "aqi",            "mdi:molecule",      0},
    {"CO2",       "co2",      &topicCo2,      "ppm",   "carbon_dioxide", "mdi:molecule-co2",  0},
  };

  // unique_id vcetne MAC - jinak by se dve zarizeni v jedne HA prepsala.
  const String devUid = "sharp_sen66_" + deviceId;

  for (const auto& s : sensors) {
    JsonDocument doc;
    const String uid = devUid + "_" + s.uidSuffix;

    doc["name"] = s.name;
    doc["unique_id"] = uid;
    doc["state_topic"] = *s.topic;
    if (s.unit) doc["unit_of_measurement"] = s.unit;
    if (s.devClass) doc["device_class"] = s.devClass;
    if (s.icon) doc["icon"] = s.icon;
    doc["state_class"] = "measurement";  // nutne pro dlouhodobe statistiky v HA
    doc["suggested_display_precision"] = s.precision;
    doc["availability_topic"] = topicStatus;
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = devUid;
    dev["name"] = "Sharp SEN66 Displej";
    dev["model"] = "Sharp LS027B7DH01 + SEN66";
    dev["manufacturer"] = "DIY";
    dev["sw_version"] = FW_VERSION;
    if (WiFi.status() == WL_CONNECTED) {
      dev["configuration_url"] = String("http://") + WiFi.localIP().toString();
    }

    JsonObject origin = doc["origin"].to<JsonObject>();
    origin["name"] = "sharp-sen66-mqtt-display";
    origin["sw"] = FW_VERSION;

    const String topic = appConfig.haPrefix + "/sensor/" + uid + "/config";

    String payload;
    serializeJson(doc, payload);
    if (!mqtt.publish(topic.c_str(), payload.c_str(), true)) {
      Serial.printf("HA Discovery: publish selhal (%s, %u B)\n", s.name, (unsigned)payload.length());
    }
    delay(50);
  }

  Serial.println("HA Discovery: Hotovo!");
}

// =============================================
//  MQTT - CALLBACK
// =============================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  const String t(topic);
  Serial.printf("MQTT RX [%s]: %s\n", topic, message.c_str());

  // --- HA nastartoval (birth message): poslat discovery znovu ---
  if (t == topicHaStatus) {
    if (message == "online") {
      Serial.println("MQTT: HA birth message, posilam discovery znovu");
      delay(random(200, 1500));  // rozprostreni zateze dle doporuceni HA
      publishHADiscovery();
    }
    return;
  }

  // --- TEXT ---
  if (t == topicText) {
    overrideText = message;
    overrideTextSize = 2;
    overrideX = 10;
    overrideY = 10;
    displayOverride = true;
    displayOverrideUntil = millis() + 30000UL;
    drawCustomTextScreen();
    return;
  }

  // --- CLEAR ---
  if (t == topicClear) {
    displayOverride = false;
    clearCanvas();
    display.refresh();
    Serial.println("Display cleared");
    return;
  }

  // --- COMMAND (JSON) ---
  if (t != topicCommand) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, message);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return;
  }

  // {"text":"Hello","x":10,"y":50,"size":3,"duration":60}
  if (doc["text"].is<const char*>()) {
    overrideText = doc["text"].as<String>();
    overrideX = doc["x"] | 10;
    overrideY = doc["y"] | 10;
    overrideTextSize = constrain((int)(doc["size"] | 2), 1, 8);
    const long duration = constrain((long)(doc["duration"] | 30), 1L, 86400L);
    displayOverride = true;
    displayOverrideUntil = millis() + (unsigned long)duration * 1000UL;
    drawCustomTextScreen();
  }

  // {"line":{"x1":0,"y1":120,"x2":399,"y2":120}}
  if (doc["line"].is<JsonObject>()) {
    JsonObject line = doc["line"];
    display.drawLine(line["x1"] | 0, line["y1"] | 0,
                     line["x2"] | (DISPLAY_WIDTH - 1), line["y2"] | 0, inkColor());
    display.refresh();
  }

  // {"rect":{"x":10,"y":10,"w":100,"h":50,"fill":false}}
  if (doc["rect"].is<JsonObject>()) {
    JsonObject rect = doc["rect"];
    const int x = rect["x"] | 0;
    const int y = rect["y"] | 0;
    const int w = rect["w"] | 50;
    const int h = rect["h"] | 30;
    const bool fill = rect["fill"] | false;
    if (fill) {
      display.fillRect(x, y, w, h, inkColor());
    } else {
      display.drawRect(x, y, w, h, inkColor());
    }
    display.refresh();
  }

  // {"invert":true} - softwarova inverze (Sharp LCD nema HW inverzi)
  if (doc["invert"].is<bool>()) {
    appConfig.displayInvertRequested = doc["invert"].as<bool>();
    Serial.printf("Display: inverze %s (docasne, neuklada se do NVS)\n",
                  appConfig.displayInvertRequested ? "zapnuta" : "vypnuta");
    if (displayOverride) {
      drawCustomTextScreen();
    } else {
      drawSensorScreen();
    }
  }

  // {"dashboard":true}
  if (doc["dashboard"].is<bool>()) {
    displayOverride = false;
    drawSensorScreen();
  }
}

// =============================================
//  MQTT - PUBLISH SENSOR DATA
// =============================================

void publishSensorData() {
  if (!mqtt.connected() || !sensorData.valid) return;
  if (firstValidSensorAt == 0 || (millis() - firstValidSensorAt) < appConfig.mqttWarmupDelay) {
    Serial.println("MQTT: warmup delay aktivni, publikace preskocena");
    return;
  }

  char buf[16];

  snprintf(buf, sizeof(buf), "%.1f", sensorData.temperature);
  mqtt.publish(topicTemp.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%.1f", sensorData.humidity);
  mqtt.publish(topicHumidity.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%.1f", sensorData.pm1);
  mqtt.publish(topicPm1.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%.1f", sensorData.pm25);
  mqtt.publish(topicPm25.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%.1f", sensorData.pm4);
  mqtt.publish(topicPm4.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%.1f", sensorData.pm10);
  mqtt.publish(topicPm10.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%.0f", sensorData.voc);
  mqtt.publish(topicVoc.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%.0f", sensorData.nox);
  mqtt.publish(topicNox.c_str(), buf, true);

  if (sensorData.co2Valid) {
    snprintf(buf, sizeof(buf), "%u", sensorData.co2);
    mqtt.publish(topicCo2.c_str(), buf, true);
  }

  JsonDocument doc;
  doc["temperature"] = round(sensorData.temperature * 10) / 10.0;
  doc["humidity"]    = round(sensorData.humidity * 10) / 10.0;
  doc["pm1"]  = round(sensorData.pm1 * 10) / 10.0;
  doc["pm25"] = round(sensorData.pm25 * 10) / 10.0;
  doc["pm4"]  = round(sensorData.pm4 * 10) / 10.0;
  doc["pm10"] = round(sensorData.pm10 * 10) / 10.0;
  doc["voc"]  = round(sensorData.voc);
  doc["nox"]  = round(sensorData.nox);
  if (sensorData.co2Valid) doc["co2"] = sensorData.co2;
  doc["quality"] = getAirQuality(sensorData.pm25);
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;

  String jsonBuf;
  serializeJson(doc, jsonBuf);
  mqtt.publish(topicSensor.c_str(), jsonBuf.c_str(), true);

  Serial.printf("MQTT: publikovano, JSON: %s\n", jsonBuf.c_str());
}

// =============================================
//  MQTT - CONNECT
// =============================================

bool reconnectMQTT() {
  if (appConfig.mqttServer.length() == 0) return false;

  Serial.print("MQTT: Pripojuji...");

  // Client ID musi byt na brokeru unikatni - pridavame MAC suffix.
  const String clientId = appConfig.mqttClientId + "-" + deviceId;

  const char* user = appConfig.mqttUser.length() ? appConfig.mqttUser.c_str() : nullptr;
  const char* pass = appConfig.mqttPassword.length() ? appConfig.mqttPassword.c_str() : nullptr;

  if (!mqtt.connect(clientId.c_str(), user, pass,
                    topicStatus.c_str(), 0, true, "offline")) {
    Serial.printf("CHYBA rc=%d\n", mqtt.state());
    return false;
  }

  Serial.println("OK!");

  mqtt.publish(topicStatus.c_str(), "online", true);

  mqtt.subscribe(topicText.c_str());
  mqtt.subscribe(topicClear.c_str());
  mqtt.subscribe(topicCommand.c_str());
  mqtt.subscribe(topicHaStatus.c_str());

  publishHADiscovery();
  return true;
}

// =============================================
//  SETUP
// =============================================

void setup() {
  Serial.begin(115200);
  delay(2000);  // ESP32-C3 potrebuje cas na USB Serial

  Serial.println("\n========================================");
  Serial.println("  Sharp LCD + SEN66 + MQTT v" FW_VERSION);
  Serial.println("========================================\n");

  const bool configLoaded = loadConfig(appConfig);
  Serial.printf("CFG: load %s\n", configLoaded ? "OK" : "FAILED - defaults");

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char idBuf[7];
  snprintf(idBuf, sizeof(idBuf), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  deviceId = String(idBuf);

  buildTopics();

  Serial.printf("CFG: device id=%s, base topic=%s, HA prefix=%s\n",
                deviceId.c_str(), appConfig.mqttBaseTopic.c_str(), appConfig.haPrefix.c_str());
  Serial.printf("CFG: MQTT %s:%d, publish=%lu ms, TMEP=%lu ms, warmup=%lu ms\n",
                appConfig.mqttServer.length() ? appConfig.mqttServer.c_str() : "(nenastaveno)",
                appConfig.mqttPort, appConfig.mqttPublishInterval,
                appConfig.tmepRequestInterval, appConfig.mqttWarmupDelay);
  Serial.printf("CFG: TMEP domena: %s\n",
                appConfig.tmepDomain.length() ? appConfig.tmepDomain.c_str() : "(nenastaveno)");
  Serial.printf("CFG: temperature offset=%.2f, web auth=%s\n",
                appConfig.temperatureOffset,
                appConfig.webPassword.length() ? "zapnuta" : "vypnuta");

  // 1. Displej
  Serial.println("Display: Inicializace...");
  display.begin();
  applyDisplaySettings();
  clearCanvas();
  drawSplashScreen();
  Serial.println("Display: OK!");

  // 2. WiFi provisioning (STA / AP captive)
  wifiProvisioning.begin(&appConfig, 20000UL);

  // 3. MQTT
  mqtt.setServer(appConfig.mqttServer.c_str(), appConfig.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(2048);  // HA discovery payload s device+origin ma pres 500 B
  mqtt.setKeepAlive(30);

  if (wifiProvisioning.getState() == WIFI_STA_CONNECTED) {
    reconnectMQTT();
  }

  setupWebServer();

  // 4. SEN66
  initSEN66();

  // 5. Splash na 2 sekundy
  delay(2000);

  Serial.println("\n=== SETUP HOTOV ===\n");
}

// =============================================
//  LOOP
// =============================================

void loop() {
  const unsigned long now = millis();

  wifiProvisioning.process();
  webServer.handleClient();

  if (wifiProvisioning.getState() == WIFI_STA_CONNECTED && !mqtt.connected()) {
    if (now - lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
      lastMqttReconnect = now;
      reconnectMQTT();
    }
  }

  if (mqtt.connected()) {
    mqtt.loop();
  }

  if (now - lastSensorRead > SENSOR_READ_INTERVAL) {
    lastSensorRead = now;
    readSEN66();
  }

  if (now - lastMqttPublish > appConfig.mqttPublishInterval) {
    lastMqttPublish = now;
    publishSensorData();
  }

  if (now - lastTmepRequest > appConfig.tmepRequestInterval) {
    lastTmepRequest = now;
    sendTmepRequest(false);
  }

  // Pozor: znamenkovy rozdil, aby to prezilo preteceni millis() po ~49,7 dne.
  if (displayOverride && (long)(now - displayOverrideUntil) >= 0) {
    displayOverride = false;
    Serial.println("Display: Override expired, zpet na dashboard");
  }

  if (now - lastDisplayRefresh > appConfig.displayRefreshInterval) {
    lastDisplayRefresh = now;
    if (!displayOverride) {
      drawSensorScreen();
    }
  }

  delay(10);
}
