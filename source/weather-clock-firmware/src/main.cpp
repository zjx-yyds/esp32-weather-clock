#define SCREEN_DIAGNOSTIC 0

#if SCREEN_DIAGNOSTIC

#include <Arduino.h>
#include <TFT_eSPI.h>

TFT_eSPI tft;

void drawDiagnosticScreen(uint16_t color, const char *label) {
  tft.fillScreen(color);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color == TFT_WHITE ? TFT_BLACK : TFT_WHITE, color);
  tft.drawString(label, 120, 94, 4);
  tft.drawString("240x240 ST7789", 120, 132, 2);
  tft.drawRect(0, 0, 240, 240, color == TFT_WHITE ? TFT_BLACK : TFT_WHITE);
  Serial.print("Screen test: ");
  Serial.println(label);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("ESP32 Weather Clock screen diagnostic booting...");

  tft.init();
  tft.setRotation(0);
  drawDiagnosticScreen(TFT_RED, "RED");
}

void loop() {
  static uint8_t index = 0;
  static unsigned long lastChangeMs = 0;

  if (millis() - lastChangeMs < 1200) {
    return;
  }
  lastChangeMs = millis();

  switch (index++ % 5) {
    case 0:
      drawDiagnosticScreen(TFT_RED, "RED");
      break;
    case 1:
      drawDiagnosticScreen(TFT_GREEN, "GREEN");
      break;
    case 2:
      drawDiagnosticScreen(TFT_BLUE, "BLUE");
      break;
    case 3:
      drawDiagnosticScreen(TFT_WHITE, "WHITE");
      break;
    default:
      drawDiagnosticScreen(TFT_BLACK, "BLACK");
      break;
  }
}

#else

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <time.h>

#include "secrets.h"

namespace {
constexpr unsigned long kWeatherRefreshMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kWifiPortalTimeoutSec = 180;
constexpr const char *kConfigPortalSsid = "ESP32-WeatherClock";

TFT_eSPI tft;

struct WeatherState {
  bool valid = false;
  float temperature = NAN;
  float windspeed = NAN;
  int weatherCode = -1;
  String updatedAt = "--";
  String error = "not updated";
};

WeatherState weather;
unsigned long lastWeatherFetchMs = 0;

const char *weatherText(int code) {
  if (code == 0) return "Clear";
  if (code == 1 || code == 2 || code == 3) return "Cloudy";
  if ((code >= 45 && code <= 48)) return "Fog";
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 95) return "Storm";
  return "Weather";
}

void drawStatus(const String &line) {
  tft.fillRect(0, 216, 240, 24, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line, 120, 228, 2);
}

void drawBootScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("ESP32", 120, 62, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Weather Clock", 120, 104, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("ST7789 240x240", 120, 148, 2);
}

void connectWifi() {
  drawStatus("Connecting WiFi...");
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(kWifiPortalTimeoutSec);
  wifiManager.setConnectTimeout(15);
  wifiManager.setAPCallback([](WiFiManager *manager) {
    Serial.print("Config portal started: ");
    Serial.println(manager->getConfigPortalSSID());
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("WiFi Setup", 120, 26, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Connect to AP:", 120, 78, 2);
    tft.drawString(kConfigPortalSsid, 120, 104, 2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Open 192.168.4.1", 120, 144, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Portal timeout 3 min", 120, 184, 2);
  });

  const bool connected = wifiManager.autoConnect(kConfigPortalSsid);

  if (connected && WiFi.status() == WL_CONNECTED) {
    drawStatus("WiFi " + WiFi.localIP().toString());
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
  } else {
    drawStatus("WiFi failed");
    Serial.println("WiFi connect failed.");
  }
}

void setupClock() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov");
}

String twoDigits(int value) {
  return value < 10 ? "0" + String(value) : String(value);
}

bool getLocalTimeSafe(tm &timeinfo) {
  return getLocalTime(&timeinfo, 20);
}

String currentTimeText() {
  tm timeinfo{};
  if (!getLocalTimeSafe(timeinfo)) return "--:--";
  return twoDigits(timeinfo.tm_hour) + ":" + twoDigits(timeinfo.tm_min);
}

String currentDateText() {
  tm timeinfo{};
  if (!getLocalTimeSafe(timeinfo)) return "Waiting for NTP";
  return String(timeinfo.tm_year + 1900) + "-" + twoDigits(timeinfo.tm_mon + 1) + "-" +
         twoDigits(timeinfo.tm_mday);
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    weather.valid = false;
    weather.error = "wifi offline";
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += WEATHER_LATITUDE;
  url += "&longitude=";
  url += WEATHER_LONGITUDE;
  url += "&current_weather=true&timezone=Asia%2FShanghai";

  if (!http.begin(client, url)) {
    weather.valid = false;
    weather.error = "http begin failed";
    return;
  }

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    weather.valid = false;
    weather.error = "http " + String(status);
    http.end();
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getString());
  http.end();

  if (error) {
    weather.valid = false;
    weather.error = "json failed";
    return;
  }

  JsonObject current = doc["current_weather"];
  weather.temperature = current["temperature"] | NAN;
  weather.windspeed = current["windspeed"] | NAN;
  weather.weatherCode = current["weathercode"] | -1;
  weather.updatedAt = current["time"] | "--";
  weather.valid = true;
  weather.error = "";
  lastWeatherFetchMs = millis();
}

void drawMainScreen() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(WEATHER_LOCATION_NAME, 120, 8, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(currentTimeText(), 120, 38, 7);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(currentDateText(), 120, 108, 2);

  if (weather.valid) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString(String(weather.temperature, 1) + " C", 120, 136, 4);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(weatherText(weather.weatherCode), 120, 172, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Wind " + String(weather.windspeed, 0) + " km/h", 120, 202, 2);
  } else {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("Weather offline", 120, 148, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(weather.error, 120, 186, 2);
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("ESP32 Weather Clock booting...");

  tft.init();
  tft.setRotation(0);
  drawBootScreen();
  drawStatus("Booting...");

  connectWifi();
  setupClock();
  fetchWeather();
  drawMainScreen();
}

void loop() {
  static unsigned long lastDrawMs = 0;
  const unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED &&
      (lastWeatherFetchMs == 0 || now - lastWeatherFetchMs > kWeatherRefreshMs)) {
    fetchWeather();
  }

  if (now - lastDrawMs > 1000) {
    drawMainScreen();
    lastDrawMs = now;
  }
}

#endif
