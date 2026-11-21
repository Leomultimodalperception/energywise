#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <array>
#include <cmath>
#include <ctime>
#include <cctype>
#if __has_include("wifi.local.h")
#include "wifi.local.h"
#endif
#include "ui.h"

constexpr int hanRx = 16;
constexpr int pins[] = {25, 26, 27, 32};
constexpr size_t maxPrices = 200;
constexpr size_t maxTelegram = 4096;
constexpr uint32_t meterTimeoutMs = 30000;
constexpr size_t historySize = 60;
constexpr uint32_t sampleIntervalMs = 10000;
HardwareSerial han(2);
WebServer web(80);
Preferences preferences;
struct Price { time_t start; unsigned duration; double cents; };
struct Rule { unsigned minutes; double limit; };
std::array<Price, maxPrices> prices;
size_t priceCount = 0;
Rule rules[4];
bool outputs[4]{};
double importKw = NAN, exportKw = NAN, importKwh = NAN;
uint32_t meterAt = 0;
bool meterSeen = false;
String telegram;
bool collecting = false;
bool atLineStart = true;
struct PowerSample { double importKw; double exportKw; };
std::array<PowerSample, historySize> history;
size_t historyCount = 0, nextSample = 0;
uint32_t sampledAt = 0;

void samplePower(uint32_t now) {
  if (now - sampledAt < sampleIntervalMs) return;
  sampledAt = now;
  bool fresh = meterSeen && now - meterAt < meterTimeoutMs;
  history[nextSample] = {fresh ? importKw : NAN, fresh ? exportKw : NAN};
  nextSample = (nextSample + 1) % historySize;
  if (historyCount < historySize) ++historyCount;
}

uint16_t crc16(const String& data, size_t length) {
  uint16_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xA001 : 0);
  }
  return crc;
}

double meterValue(const char* field, const char* unit) {
  int offset = telegram.indexOf(field);
  if (offset < 0) return NAN;
  const char* start = telegram.c_str() + offset + strlen(field);
  char* end;
  double result = strtod(start, &end);
  return end != start && strncmp(end, unit, strlen(unit)) == 0 &&
         std::isfinite(result) && result >= 0 ? result : NAN;
}

void readHan() {
  while (han.available()) {
    char byte = han.read();
    if (byte == '/' && atLineStart) {
      telegram = "";
      collecting = true;
    }
    atLineStart = byte == '\n';
    if (!collecting) continue;
    telegram += byte;
    if (telegram.length() >= maxTelegram) {
      collecting = false;
      continue;
    }
    if (byte != '\n') continue;
    int mark = telegram.lastIndexOf('!');
    if (mark < 0) continue;
    collecting = false;
    String checksum = telegram.substring(mark + 1);
    checksum.trim();
    if (checksum.length() != 4) continue;
    bool hex = true;
    for (size_t i = 0; i < checksum.length(); ++i)
      hex &= isxdigit(static_cast<unsigned char>(checksum[i])) != 0;
    if (!hex ||
        crc16(telegram, mark + 1) != strtoul(checksum.c_str(), nullptr, 16)) continue;
    importKw = meterValue("1-0:1.7.0(", "*kW)");
    exportKw = meterValue("1-0:2.7.0(", "*kW)");
    importKwh = meterValue("1-0:1.8.0(", "*kWh)");
    meterSeen = true;
    meterAt = millis();
  }
}

const Price* currentPrice(time_t now, unsigned minutes) {
  if (now < 1700000000) return nullptr;
  for (size_t i = 0; i < priceCount; ++i) {
    const auto& p = prices[i];
    if (p.duration == minutes && now >= p.start && now - p.start < p.duration * 60)
      return &p;
  }
  return nullptr;
}

void control() {
  time_t now = time(nullptr);
  for (size_t i = 0; i < 4; ++i) {
    auto p = currentPrice(now, rules[i].minutes);
    outputs[i] = p && p->cents <= rules[i].limit;
    digitalWrite(pins[i], outputs[i] ? HIGH : LOW);
  }
}

void powerHistory() {
  JsonDocument doc;
  auto samples = doc.to<JsonArray>();
  size_t oldest = (nextSample + historySize - historyCount) % historySize;
  for (size_t i = 0; i < historyCount; ++i) {
    const auto& sample = history[(oldest + i) % historySize];
    auto values = samples.add<JsonArray>();
    if (std::isfinite(sample.importKw)) values.add(sample.importKw);
    else values.add(nullptr);
    if (std::isfinite(sample.exportKw)) values.add(sample.exportKw);
    else values.add(nullptr);
  }
  String response;
  serializeJson(doc, response);
  web.send(200, "application/json", response);
}

void status() {
  JsonDocument doc;
  bool fresh = meterSeen && millis() - meterAt < meterTimeoutMs;
  doc["meterFresh"] = fresh;
  if (fresh && std::isfinite(importKw)) doc["importKw"] = importKw;
  if (fresh && std::isfinite(exportKw)) doc["exportKw"] = exportKw;
  if (fresh && std::isfinite(importKwh)) doc["importKwh"] = importKwh;
  time_t now = time(nullptr);
  doc["clockReady"] = now >= 1700000000;
  for (unsigned minutes : {15U, 60U}) {
    auto p = currentPrice(now, minutes);
    if (p) doc[minutes == 15 ? "quarterPrice" : "hourPrice"] = p->cents;
  }
  auto channels = doc["outputs"].to<JsonArray>();
  for (size_t i = 0; i < 4; ++i) {
    auto item = channels.add<JsonObject>();
    item["on"] = outputs[i];
    item["minutes"] = rules[i].minutes;
    item["limit"] = rules[i].limit;
  }
  String response;
  serializeJson(doc, response);
  web.send(200, "application/json", response);
}

bool requireJson() {
  if (web.header("Content-Type") == "application/json") return true;
  web.send(415, "text/plain", "Tarvitaan application/json");
  return false;
}

void saveRules() {
  if (!requireJson()) return;
  JsonDocument doc;
  if (deserializeJson(doc, web.arg("plain")) || !doc.is<JsonArray>() || doc.size() != 4) {
    web.send(400, "text/plain", "Tarvitaan nelja asetusrivia");
    return;
  }
  Rule next[4];
  size_t i = 0;
  for (JsonVariant item : doc.as<JsonArray>()) {
    auto minutes = item["minutes"];
    auto limit = item["limit"];
    unsigned period = minutes.as<unsigned>();
    if (!minutes.is<unsigned>() || (period != 0 && period != 15 && period != 60) ||
        !limit.is<double>() || !std::isfinite(limit.as<double>())) {
      web.send(400, "text/plain", "Virheellinen lahtoasetus");
      return;
    }
    next[i++] = {period, limit.as<double>()};
  }
  if (preferences.putBytes("rules", next, sizeof(next)) != sizeof(next)) {
    web.send(500, "text/plain", "Asetusten tallennus epaonnistui");
    return;
  }
  memcpy(rules, next, sizeof(rules));
  control();
  web.send(204);
}

void loadPrices() {
  if (!requireJson()) return;
  JsonDocument doc;
  String body = web.arg("plain");
  if (body.length() > 20000 || deserializeJson(doc, body) ||
      !doc.is<JsonArray>() || doc.size() > maxPrices) {
    web.send(400, "text/plain", "Virheellinen hinnasto tai yli 200 jaksoa");
    return;
  }
  // Keep the last valid schedule when an upload fails validation.
  static std::array<Price, maxPrices> next;
  size_t count = 0;
  for (JsonVariant item : doc.as<JsonArray>()) {
    auto start = item["start"];
    auto minutes = item["minutes"];
    auto cents = item["cents"];
    unsigned duration = minutes.as<unsigned>();
    if (!start.is<time_t>() || start.as<time_t>() < 1700000000 ||
        !minutes.is<unsigned>() || (duration != 15 && duration != 60) ||
        !cents.is<double>() || !std::isfinite(cents.as<double>()) ||
        start.as<time_t>() % (duration * 60) != 0) {
      web.send(400, "text/plain", "Virheellinen hintajakso");
      return;
    }
    auto timestamp = start.as<time_t>();
    for (size_t i = 0; i < count; ++i) {
      if (next[i].start == timestamp && next[i].duration == duration) {
        web.send(400, "text/plain", "Paallekkainen hintajakso");
        return;
      }
    }
    next[count++] = {timestamp, duration, cents.as<double>()};
  }
  // Store the complete schedule in one NVS entry before replacing the live data.
  const size_t bytes = count * sizeof(Price);
  bool saved = bytes ? preferences.putBytes("prices-v1", next.data(), bytes) == bytes
                     : !preferences.isKey("prices-v1") || preferences.remove("prices-v1");
  if (!saved) {
    web.send(500, "text/plain", "Hinnaston tallennus epaonnistui");
    return;
  }
  prices = next;
  priceCount = count;
  control();
  web.send(204);
}

void setup() {
  for (int pin : pins) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
  }
  Serial.begin(115200);
  telegram.reserve(maxTelegram);
  han.setRxBufferSize(8192);
  han.begin(115200, SERIAL_8N1, hanRx, -1);
  preferences.begin("energywise", false);
  if (preferences.getBytesLength("rules") == sizeof(rules))
    preferences.getBytes("rules", rules, sizeof(rules));

  const size_t bytes = preferences.getBytesLength("prices-v1");
  if (bytes && bytes <= sizeof(prices) && bytes % sizeof(Price) == 0 &&
      preferences.getBytes("prices-v1", prices.data(), bytes) == bytes)
    priceCount = bytes / sizeof(Price);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
#ifdef ENERGYWISE_WIFI_SSID
  WiFi.begin(ENERGYWISE_WIFI_SSID, ENERGYWISE_WIFI_PASSWORD);
#else
  WiFi.begin();
#endif
  configTime(0, 0, "pool.ntp.org");
  const char* headers[] = {"Content-Type"};
  web.collectHeaders(headers, 1);
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", page); });
  web.on("/api/status", HTTP_GET, status);
  web.on("/api/history", HTTP_GET, powerHistory);
  web.on("/api/rules", HTTP_POST, saveRules);
  web.on("/api/prices", HTTP_POST, loadPrices);
  web.begin();
}

void loop() {
  readHan();
  samplePower(millis());
  control();
  web.handleClient();
  static bool connected = false;
  bool online = WiFi.status() == WL_CONNECTED;
  if (online && !connected) Serial.println(WiFi.localIP());
  connected = online;
  delay(2);
}
