/* ESP32 Climate Monitor
   - DHT22 (temperature, humidity)
   - SGP30 (TVOC, eCO2)
   - LDR (analog)
   - LittleFS web files (index.html, script.js, style.css)
   - WiFiManager, mDNS
   - JSON API: /data.json
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "DHT.h"
#include <Adafruit_SGP30.h>

#define DHTPIN 33
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

Adafruit_SGP30 sgp;
WebServer server(80);

// LDR
const int analogPin = 32;
const float Vcc = 3.3;
const int ADC_MAX = 4095;
const float R_fixed = 10000.0;

// калибровки
const float A_cal = 35754.18994413404;
const float B_cal = 0.6817543949741522;

// период опроса всех датчиков (мс)
const unsigned long MEASURE_PERIOD_MS = 5000;
unsigned long lastMeasure = 0;

// значения измерений
float temperature = 0.0;
float humidity = 0.0;
float lux = 0.0;
uint16_t eCO2 = 0;
uint16_t TVOC = 0;

// ============================================================
// Вспомогательные функции
// ============================================================

float adcToLux(int adc) {
  if (adc <= 0 || adc >= ADC_MAX) return 0.0;
  float Vout = (float)adc * Vcc / ADC_MAX;
  if (Vcc - Vout <= 1e-6) return 0.0;
  float Rldr = R_fixed * Vout / (Vcc - Vout);
  float ratio = Rldr / A_cal;
  if (ratio <= 0) return 0.0;
  return pow(ratio, -1.0 / B_cal);
}

void measureAll() {
  // --- DHT22 ---
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity = h;

  // --- LDR ---
  int adcVal = analogRead(analogPin);
  lux = adcToLux(adcVal);

  // --- SGP30 ---
  if (sgp.IAQmeasure()) {
    eCO2 = sgp.eCO2;
    TVOC = sgp.TVOC;
  }

  Serial.printf("[Measure] T=%.1f°C, H=%.1f%%, Lux=%.1f, eCO2=%u ppm, TVOC=%u ppb\n",
                temperature, humidity, lux, eCO2, TVOC);
}

// ============================================================
// HTTP Handlers
// ============================================================

void handleRoot() {
  if (!LittleFS.exists("/index.html")) {
    server.send(200, "text/plain", "Index not found. Upload files to LittleFS /data.");
    return;
  }
  File f = LittleFS.open("/index.html", "r");
  server.streamFile(f, "text/html");
  f.close();
}

void handleDataJson() {
  StaticJsonDocument<256> doc;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["light"] = lux;
  doc["eCO2"] = eCO2;
  doc["TVOC"] = TVOC;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleNotFound() {
  String uri = server.uri();
  if (LittleFS.exists(uri)) {
    File f = LittleFS.open(uri, "r");
    String contentType = "text/plain";
    if (uri.endsWith(".js")) contentType = "application/javascript";
    else if (uri.endsWith(".css")) contentType = "text/css";
    else if (uri.endsWith(".png")) contentType = "image/png";
    else if (uri.endsWith(".svg")) contentType = "image/svg+xml";
    else if (uri.endsWith(".json")) contentType = "application/json";
    server.streamFile(f, contentType.c_str());
    f.close();
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void handleResetWiFi() {
  WiFiManager wm;
  wm.resetSettings();
  server.send(200, "text/plain", "WiFi settings cleared. Rebooting...");
  delay(500);
  ESP.restart();
}

// ============================================================
// Setup / Loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Booting...");

  // FS
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed!");
  else Serial.println("LittleFS mounted.");

  // DHT
  dht.begin();

  // SGP30
  if (!sgp.begin()) Serial.println("SGP30 not found :(");
  else {
    Serial.println("SGP30 initialised");
    sgp.IAQinit();
    delay(100);
  }

  // WiFiManager
  WiFiManager wm;
  bool res = wm.autoConnect("ESP32-Setup");
  if (!res) Serial.println("Failed to connect");
  else Serial.print("Connected to WiFi: "), Serial.println(WiFi.SSID());

  // mDNS
  if (MDNS.begin("climate")) Serial.println("mDNS responder: http://climate.local");
  else Serial.println("mDNS failed");

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data.json", HTTP_GET, handleDataJson);
  server.on("/resetwifi", HTTP_GET, handleResetWiFi);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started.");

  analogReadResolution(12);

  // Первая инициализация данных
  measureAll();
  lastMeasure = millis();
}

void loop() {
  server.handleClient();

  if (millis() - lastMeasure >= MEASURE_PERIOD_MS) {
    measureAll();
    lastMeasure = millis();
  }

  delay(10);
}
