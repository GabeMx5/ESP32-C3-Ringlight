#define FIRMWARE_VERSION "1.2.22"

#include "teeSerial.h"
TeeSerial teeSerial;
#include <vector>
#include <set>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include "wifiConfigManager.h"
#include "ringController.h"
#include "networkManager.h"
#include "mqttController.h"
#include "timerController.h"
#include "configController.h"
#include "geoController.h"
#include "otaController.h"
#include "serialConsole.h"
#include "alexaController.h"
#include "bambulabController.h"
#ifdef IMPROV_ENABLED
#include "improvController.h"
#endif

AsyncWebServer webServer(80);
AsyncWebSocket ws("/ws");
static std::set<uint32_t> _consoleViewerIds;  // clients currently on the console tab
static std::set<uint32_t> _infoViewerIds;     // clients currently on the info or settings tab
static std::set<uint32_t> _bambuViewerIds;    // clients currently on the bambu tab

WiFiConfigManager  wifiManager;
RingController     ringController;
NetworkManager     networkManager;
MQTTController     mqttController;
TimerController    timerController;
ConfigController   configController;
GeoController      geoController;
OTAController      otaController;
SerialConsole      serialConsole(wifiManager, mqttController, FIRMWARE_VERSION);
AlexaController    alexaController;
BambuLabController bambuController;

// The BambuLab overlay has a finite lifetime and is re-armed from loop(). A
// short hold plus a periodic refresh means the ring recovers on its own if the
// refresh ever stops, and avoids any millis() overflow arithmetic.
static const unsigned long BAMBU_OVERLAY_HOLD_MS    = 120000;  // 2 minutes
static const unsigned long BAMBU_OVERLAY_REFRESH_MS = 30000;   // 30 seconds

void weatherTempToRgb(float temp, uint8_t& r, uint8_t& g, uint8_t& b);
void conditionToRgb(WeatherCondition cond, bool isDay, uint8_t& r, uint8_t& g, uint8_t& b);
void humidityToRgb(float humidity, uint8_t& r, uint8_t& g, uint8_t& b);
static void aqValueToRgb(float v, float tGood, float tModerate, float tPoor,
                         uint8_t& r, uint8_t& g, uint8_t& b);
static void broadcastRingStatus();
static void broadcastEffectStatus();

static const char* conditionToString(WeatherCondition c)
{
    switch (c) {
        case WeatherCondition::CLEAR:         return "Clear";
        case WeatherCondition::PARTLY_CLOUDY: return "Partly Cloudy";
        case WeatherCondition::FOGGY:         return "Foggy";
        case WeatherCondition::DRIZZLE:       return "Drizzle";
        case WeatherCondition::RAINY:         return "Rainy";
        case WeatherCondition::SNOWY:         return "Snowy";
        case WeatherCondition::STORMY:        return "Stormy";
        default:                              return "Unknown";
    }
}

// Longest loop() iteration since the last INFO push. A direct read on how much
// the network stack is blocking the main task: the ring no longer renders here,
// but MQTT, OTA and TLS work still queue up behind it.
static uint32_t _loopMaxMs = 0;

// True when at least one client is on a tab that consumes BambuLab updates.
static bool _hasBambuOrInfoViewers()
{
    return !_bambuViewerIds.empty() || !_infoViewerIds.empty();
}

static void _sendToBambuAndInfoViewers(const String& msg)
{
    for (uint32_t id : _bambuViewerIds) {
        AsyncWebSocketClient* c = ws.client(id);
        if (c) c->text(msg);
    }
    for (uint32_t id : _infoViewerIds) {
        AsyncWebSocketClient* c = ws.client(id);
        if (c) c->text(msg);
    }
}

// ─── Effect helpers ───────────────────────────────────────────────────────────

// Turns exactly one effect on (or none), keeping every stored parameter. Going
// through a single place keeps the "effects are mutually exclusive" rule in one
// spot instead of scattered across every command handler.
static void applyEffectExclusive(RingEffect e)
{
    ringController.setSpinner(e == RingEffect::SPINNER,
                              ringController.getSpinnerR(), ringController.getSpinnerG(),
                              ringController.getSpinnerB(), ringController.getSpinnerTail(),
                              ringController.getSpinnerSpeed(), ringController.getSpinnerCW());
    if (e == RingEffect::SPINNER) return;
    ringController.setRainbow(e == RingEffect::RAINBOW, ringController.getRainbowCycleTime());
    if (e == RingEffect::RAINBOW) return;
    ringController.setParty(e == RingEffect::PARTY, ringController.getPartyMadness());
    if (e == RingEffect::PARTY) return;
    ringController.setProgress(e == RingEffect::PROGRESS, ringController.getProgressPercent(),
                               ringController.getProgressFR(), ringController.getProgressFG(),
                               ringController.getProgressFB(), ringController.getProgressBR(),
                               ringController.getProgressBG(), ringController.getProgressBB());
    if (e == RingEffect::PROGRESS) return;
    ringController.setClock(e == RingEffect::CLOCK);
    if (e == RingEffect::CLOCK) return;
    ringController.setChase(e == RingEffect::CHASE,
                            ringController.getChaseR(), ringController.getChaseG(),
                            ringController.getChaseB(), ringController.getChaseSpeed());
}

// Fills a logical pixel buffer with a progress arc in the given colour.
static void buildProgressPixels(uint8_t percent, uint8_t r, uint8_t g, uint8_t b,
                                uint8_t out[LED_COUNT][3])
{
    if (percent > 100) percent = 100;
    float exact = (float)percent * LED_COUNT / 100.0f;
    int   full  = (int)exact;
    float frac  = exact - full;
    for (int i = 0; i < LED_COUNT; i++)
    {
        if (i < full)       { out[i][0] = r; out[i][1] = g; out[i][2] = b; }
        else if (i == full) { out[i][0] = (uint8_t)(r * frac);
                              out[i][1] = (uint8_t)(g * frac);
                              out[i][2] = (uint8_t)(b * frac); }
        else                { out[i][0] = 0; out[i][1] = 0; out[i][2] = 0; }
    }
}

// ─── BambuLab ring mapping ────────────────────────────────────────────────────

// While printing the ring doubles as a progress bar drawn in the state colour;
// every other state paints the whole ring in its colour.
static void applyBambuRingState(BambuState s)
{
    uint8_t r, g, b;
    bambuController.getStateColor(s, r, g, b);
    if (!r && !g && !b) { ringController.cancelOverlay(); return; }
    if (s == BambuState::RUNNING)
    {
        uint8_t px[LED_COUNT][3];
        buildProgressPixels(bambuController.getPercent(), r, g, b, px);
        ringController.showPixels(px, BAMBU_OVERLAY_HOLD_MS);
    }
    else
    {
        ringController.showColor(r, g, b, BAMBU_OVERLAY_HOLD_MS);
    }
}

// States that map to "all off" release the overlay instead of holding it, so
// there is nothing to re-arm for them.
static bool bambuStateHasColor(BambuState s)
{
    uint8_t r, g, b;
    bambuController.getStateColor(s, r, g, b);
    return r || g || b;
}

// Leaves BambuLab printer mode and tells every client. Called whenever another
// effect takes the ring.
static void cancelBambuMode()
{
    if (!bambuController.getBambuMode()) return;
    bambuController.setBambuMode(false);
    configController.setBambuMode(false);
    bambuController.resetIdle();
    ringController.cancelOverlay();
    JsonDocument doc;
    doc["type"]      = "bambuConfig";
    doc["bambuMode"] = false;
    String msg; serializeJson(doc, msg);
    ws.textAll(msg);
    mqttController.publishSwitchState("bambu_mode", false);
}

// ─── Status builders ──────────────────────────────────────────────────────────

static String _buildRingStatus()
{
    JsonDocument doc;
    doc["type"] = "ringStatus";
    uint32_t c  = ringController.getColor();
    doc["r"]     = (c >> 16) & 0xFF;
    doc["g"]     = (c >> 8)  & 0xFF;
    doc["b"]     = c & 0xFF;
    doc["on"]    = ringController.getOn();
    doc["blink"] = ringController.getBlink();
    String msg; serializeJson(doc, msg); return msg;
}

static void sendRingStatus(AsyncWebSocketClient* client) { client->text(_buildRingStatus()); }

static void broadcastRingStatus()
{
    String msg = _buildRingStatus();
    ws.textAll(msg);
    mqttController.publish(msg);
    uint32_t c = ringController.getColor();
    mqttController.publishRingState((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF,
                                    ringController.getOn());
}

static String _buildEffectStatus()
{
    JsonDocument doc;
    doc["type"]   = "effectStatus";
    doc["effect"] = RingController::effectToString(ringController.getEffect());

    JsonObject sp = doc["spinner"].to<JsonObject>();
    sp["r"]     = ringController.getSpinnerR();
    sp["g"]     = ringController.getSpinnerG();
    sp["b"]     = ringController.getSpinnerB();
    sp["tail"]  = ringController.getSpinnerTail();
    sp["speed"] = ringController.getSpinnerSpeed();
    sp["cw"]    = ringController.getSpinnerCW();

    doc["rainbowCycleTime"] = ringController.getRainbowCycleTime();
    doc["partyMadness"]     = ringController.getPartyMadness();

    JsonObject pr = doc["progress"].to<JsonObject>();
    pr["pct"] = ringController.getProgressPercent();
    pr["fr"]  = ringController.getProgressFR();
    pr["fg"]  = ringController.getProgressFG();
    pr["fb"]  = ringController.getProgressFB();
    pr["br"]  = ringController.getProgressBR();
    pr["bg"]  = ringController.getProgressBG();
    pr["bb"]  = ringController.getProgressBB();

    JsonObject cl = doc["clock"].to<JsonObject>();
    cl["hr"] = ringController.getClockColor(CLOCK_HOURS,   0);
    cl["hg"] = ringController.getClockColor(CLOCK_HOURS,   1);
    cl["hb"] = ringController.getClockColor(CLOCK_HOURS,   2);
    cl["mr"] = ringController.getClockColor(CLOCK_MINUTES, 0);
    cl["mg"] = ringController.getClockColor(CLOCK_MINUTES, 1);
    cl["mb"] = ringController.getClockColor(CLOCK_MINUTES, 2);
    cl["sr"] = ringController.getClockColor(CLOCK_SECONDS, 0);
    cl["sg"] = ringController.getClockColor(CLOCK_SECONDS, 1);
    cl["sb"] = ringController.getClockColor(CLOCK_SECONDS, 2);

    JsonObject ch = doc["chase"].to<JsonObject>();
    ch["r"]     = ringController.getChaseR();
    ch["g"]     = ringController.getChaseG();
    ch["b"]     = ringController.getChaseB();
    ch["speed"] = ringController.getChaseSpeed();

    JsonObject ge = doc["geometry"].to<JsonObject>();
    ge["origin"]  = ringController.getOrigin();
    ge["reverse"] = ringController.getReverse();

    String msg; serializeJson(doc, msg); return msg;
}

static void sendEffectStatus(AsyncWebSocketClient* client) { client->text(_buildEffectStatus()); }

static void broadcastEffectStatus()
{
    String msg = _buildEffectStatus();
    ws.textAll(msg);
    mqttController.publish(msg);
    RingEffect e = ringController.getEffect();
    mqttController.publishSwitchState("spinner",    e == RingEffect::SPINNER);
    mqttController.publishSwitchState("rainbow",    e == RingEffect::RAINBOW);
    mqttController.publishSwitchState("party",      e == RingEffect::PARTY);
    mqttController.publishSwitchState("chase",      e == RingEffect::CHASE);
    mqttController.publishSwitchState("clock",      e == RingEffect::CLOCK);
    mqttController.publishSwitchState("progress",   e == RingEffect::PROGRESS);
    mqttController.publishSwitchState("bambu_mode", bambuController.getBambuMode());
    mqttController.publishProgress(ringController.getProgressPercent());
}

static String _buildConfigStatus()
{
    JsonDocument doc;
    doc["type"]                  = "configStatus";
    doc["makeChangesPersistent"] = configController.getMakeChangesPersistent();
    doc["latitude"]              = configController.getLatitude();
    doc["longitude"]             = configController.getLongitude();
    String msg; serializeJson(doc, msg); return msg;
}

static void sendConfigStatus(AsyncWebSocketClient* client) { client->text(_buildConfigStatus()); }
static void broadcastConfigStatus() { ws.textAll(_buildConfigStatus()); }

static void sendMqttConfig(AsyncWebSocketClient* client)
{
    JsonDocument doc;
    doc["type"]      = "mqttConfig";
    doc["broker"]    = mqttController.getBroker();
    doc["port"]      = mqttController.getPort();
    doc["username"]  = mqttController.getUsername();
    doc["password"]  = mqttController.getPassword();
    doc["clientId"]  = mqttController.getClientId();
    doc["topic"]     = mqttController.getTopicPrefix();
    doc["enabled"]   = mqttController.getEnabled();
    doc["connected"] = mqttController.isConnected();
    String response; serializeJson(doc, response);
    client->text(response);
}

static void sendBambuConfig(AsyncWebSocketClient* client)
{
    JsonDocument doc;
    doc["type"]           = "bambuConfig";
    doc["ip"]             = bambuController.getIp();
    doc["serial"]         = bambuController.getSerial();
    doc["accessCode"]     = bambuController.getAccessCode();
    doc["enabled"]        = bambuController.getEnabled();
    doc["connected"]      = bambuController.isConnected();
    doc["state"]          = BambuLabController::stateToString(bambuController.getState());
    doc["bambuMode"]      = bambuController.getBambuMode();
    doc["idleTimeoutMin"] = bambuController.getIdleTimeoutMin();
    doc["percent"]        = bambuController.getPercent();
    bambuController.addStateColorsToJson(doc);
    String response; serializeJson(doc, response);
    client->text(response);
}

static void sendWifiConfig(AsyncWebSocketClient* client)
{
    JsonDocument doc;
    doc["type"]       = "wifiConfig";
    doc["deviceName"] = wifiManager.deviceName;
    doc["ntpServer"]  = wifiManager.ntpServer;
    doc["timezone"]   = wifiManager.timezone;
    doc["ssid"]       = wifiManager.wifiSSID;
    doc["password"]   = wifiManager.wifiPassword;
    doc["dhcp"]       = wifiManager.dhcp;
    if (!wifiManager.dhcp)
    {
        doc["ip"]      = wifiManager.localIP.toString();
        doc["subnet"]  = wifiManager.subnet.toString();
        doc["gateway"] = wifiManager.gateway.toString();
        doc["dns"]     = wifiManager.dns.toString();
    }
    String response; serializeJson(doc, response);
    client->text(response);
}

// Dynamic fields — pushed every second to clients on the info tab
static void sendSysInfo(AsyncWebSocketClient* client)
{
    JsonDocument doc;
    doc["type"]     = "sysInfo";
    doc["rssi"]     = WiFi.RSSI();
    doc["freeHeap"] = ESP.getFreeHeap();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        doc["datetime"] = buf;
    }
    doc["uptime"]         = millis() / 1000;
    doc["loopMax"]        = _loopMaxMs;
    _loopMaxMs            = 0;   // report the peak of the last second, then restart
    doc["bambuConnected"] = bambuController.isConnected();
    doc["mqttConnected"]  = mqttController.isConnected();
    // Surfaced so the page served in AP mode can say why the join failed
    // instead of leaving the user to guess.
    doc["wifiFailReason"] = networkManager.getLastFailReason();
    doc["wifiFailText"]   = networkManager.getLastFailText();
    doc["apMode"]         = networkManager.isAPMode();
    String response; serializeJson(doc, response);
    client->text(response);
}

// Static fields — sent once at connect and whenever weather or air quality refresh
static String _buildSysInfoStatic()
{
    JsonDocument doc;
    doc["type"]         = "sysInfoStatic";
    doc["version"]      = FIRMWARE_VERSION;
    doc["ip"]           = WiFi.localIP().toString();
    doc["ssid"]         = WiFi.SSID();
    doc["mac"]          = WiFi.macAddress();
    doc["cpuFreq"]      = ESP.getCpuFreqMHz();
    doc["chipModel"]    = ESP.getChipModel();
    doc["chipRevision"] = ESP.getChipRevision();
    doc["wifiChannel"]  = WiFi.channel();
    doc["mqttBroker"]   = mqttController.getBroker();
    doc["ledCount"]     = LED_COUNT;
    doc["ledPin"]       = LED_DATA_PIN;
    doc["latitude"]     = configController.getLatitude();
    doc["longitude"]    = configController.getLongitude();
    if (geoController.weather.valid) {
        doc["weatherCode"]      = geoController.weather.weatherCode;
        doc["weatherTemp"]      = geoController.weather.temperature;
        doc["weatherHumidity"]  = geoController.weather.humidity;
        doc["weatherCondition"] = (int)geoController.weather.condition;
        uint8_t wr, wg, wb;
        weatherTempToRgb(geoController.weather.temperature, wr, wg, wb);
        doc["temperatureR"] = wr; doc["temperatureG"] = wg; doc["temperatureB"] = wb;
        uint8_t cr, cg, cb;
        conditionToRgb(geoController.weather.condition, geoController.weather.isDay, cr, cg, cb);
        doc["conditionR"] = cr; doc["conditionG"] = cg; doc["conditionB"] = cb;
        uint8_t hr, hg, hb;
        humidityToRgb(geoController.weather.humidity, hr, hg, hb);
        doc["humidityR"] = hr; doc["humidityG"] = hg; doc["humidityB"] = hb;
    }
    if (geoController.airQuality.valid) {
        doc["aqPm25"] = geoController.airQuality.pm2_5;
        doc["aqPm10"] = geoController.airQuality.pm10;
        doc["aqNo2"]  = geoController.airQuality.no2;
        uint8_t r, g, b;
        aqValueToRgb(geoController.airQuality.pm2_5, 12, 35,  55,  r, g, b);
        doc["aqPm25R"] = r; doc["aqPm25G"] = g; doc["aqPm25B"] = b;
        aqValueToRgb(geoController.airQuality.pm10,  20, 40,  140, r, g, b);
        doc["aqPm10R"] = r; doc["aqPm10G"] = g; doc["aqPm10B"] = b;
        aqValueToRgb(geoController.airQuality.no2,   40, 100, 200, r, g, b);
        doc["aqNo2R"]  = r; doc["aqNo2G"]  = g; doc["aqNo2B"]  = b;
    }
    String msg; serializeJson(doc, msg); return msg;
}

static void sendSysInfoStatic(AsyncWebSocketClient* client) { client->text(_buildSysInfoStatic()); }
static void broadcastSysInfoStatic() { ws.textAll(_buildSysInfoStatic()); }

// ─── Colour mapping ───────────────────────────────────────────────────────────

void weatherTempToRgb(float temp, uint8_t& outR, uint8_t& outG, uint8_t& outB)
{
    float t     = max(5.0f, min(30.0f, temp));
    float ratio = (t - 5.0f) / 25.0f;
    float hue   = 210.0f * (1.0f - ratio);
    float h = hue / 60.0f;
    float c = 1.0f;
    float x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if      (h < 1) { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else            { r = c; b = x; }
    outR = (uint8_t)(r * 255);
    outG = (uint8_t)(g * 255);
    outB = (uint8_t)(b * 255);
}

void conditionToRgb(WeatherCondition cond, bool isDay, uint8_t& r, uint8_t& g, uint8_t& b)
{
    switch (cond) {
        case WeatherCondition::CLEAR:
            if (isDay) { r=255; g=200; b=0;   }  // golden yellow
            else       { r=0;   g=20;  b=150; }  // dark blue
            break;
        case WeatherCondition::PARTLY_CLOUDY: r=150; g=170; b=200; break;  // steel blue
        case WeatherCondition::FOGGY:         r=160; g=160; b=160; break;  // gray
        case WeatherCondition::DRIZZLE:       r=80;  g=140; b=255; break;  // light blue
        case WeatherCondition::RAINY:         r=0;   g=60;  b=220; break;  // blue
        case WeatherCondition::SNOWY:         r=200; g=240; b=255; break;  // ice white
        case WeatherCondition::STORMY:        r=140; g=0;   b=200; break;  // purple
        default:                              r=80;  g=80;  b=80;  break;  // dim gray
    }
}

void humidityToRgb(float humidity, uint8_t& r, uint8_t& g, uint8_t& b)
{
    float h = max(0.0f, min(100.0f, humidity)) / 100.0f;
    // 0% = yellow (255,200,0), 50% = green (0,200,80), 100% = blue (0,60,220)
    if (h < 0.5f) {
        float t = h * 2.0f;
        r = (uint8_t)(255 * (1.0f - t));
        g = (uint8_t)(200 * (1.0f - t) + 200 * t);
        b = (uint8_t)(80  * t);
    } else {
        float t = (h - 0.5f) * 2.0f;
        r = 0;
        g = (uint8_t)(200 * (1.0f - t));
        b = (uint8_t)(80  * (1.0f - t) + 220 * t);
    }
}

// Maps a pollutant to an AQI-style colour: green, yellow, orange, red, purple.
static void aqValueToRgb(float v, float tGood, float tModerate, float tPoor,
                         uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (v < tGood)     { r=0;   g=200; b=0;   return; }
    if (v < tModerate) { r=220; g=220; b=0;   return; }
    if (v < tPoor)     { r=255; g=100; b=0;   return; }
    if (v < tPoor*2)   { r=255; g=0;   b=0;   return; }
                         r=160; g=0;   b=160;
}

// The ring is split into three arcs of four pixels each, so one glance gives
// three readings instead of one.
static void fillArc(uint8_t px[LED_COUNT][3], int from, int count,
                    uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = from; i < from + count && i < LED_COUNT; i++)
    {
        px[i][0] = r; px[i][1] = g; px[i][2] = b;
    }
}

void applyWeatherColor()
{
    if (!geoController.weather.valid) return;
    uint8_t px[LED_COUNT][3] = {{0}};
    uint8_t r, g, b;
    conditionToRgb(geoController.weather.condition, geoController.weather.isDay, r, g, b);
    fillArc(px, 0, 4, r, g, b);                                  // pixels 0-3:  condition
    weatherTempToRgb(geoController.weather.temperature, r, g, b);
    fillArc(px, 4, 4, r, g, b);                                  // pixels 4-7:  temperature
    humidityToRgb(geoController.weather.humidity, r, g, b);
    fillArc(px, 8, 4, r, g, b);                                  // pixels 8-11: humidity
    ringController.showPixels(px, 15000);
}

void applyAirQualityColor()
{
    if (!geoController.airQuality.valid) return;
    uint8_t px[LED_COUNT][3] = {{0}};
    uint8_t r, g, b;
    aqValueToRgb(geoController.airQuality.pm2_5, 12, 35,  55,  r, g, b);
    fillArc(px, 0, 4, r, g, b);                                  // pixels 0-3:  PM2.5
    aqValueToRgb(geoController.airQuality.pm10,  20, 40,  140, r, g, b);
    fillArc(px, 4, 4, r, g, b);                                  // pixels 4-7:  PM10
    aqValueToRgb(geoController.airQuality.no2,   40, 100, 200, r, g, b);
    fillArc(px, 8, 4, r, g, b);                                  // pixels 8-11: NO2
    ringController.showPixels(px, 15000);
}

// ─── Command processing (shared between WebSocket, MQTT and HTTP POST) ────────

void processCommand(JsonDocument &doc)
{
    const char *type = doc["type"];
    if (!type) return;

    if (strcmp(type, "setRing") == 0)
    {
        if (bambuController.getBambuMode() && bambuController.isConnected()) return;
        uint32_t existing = ringController.getColor();
        int er = (existing >> 16) & 0xFF;
        int eg = (existing >> 8)  & 0xFF;
        int eb = existing & 0xFF;
        int red, green, blue;
        if (!doc["brightness"].isNull() && doc["r"].isNull())
        {
            // Brightness-only command: scale the current colour, keeping its hue.
            int br   = doc["brightness"];
            int maxC = max({er, eg, eb});
            red   = maxC > 0 ? er * br / maxC : 0;
            green = maxC > 0 ? eg * br / maxC : 0;
            blue  = maxC > 0 ? eb * br / maxC : 0;
        }
        else
        {
            red   = doc["r"].isNull() ? er : (int)doc["r"];
            green = doc["g"].isNull() ? eg : (int)doc["g"];
            blue  = doc["b"].isNull() ? eb : (int)doc["b"];
        }
        bool on    = doc["on"]    | false;
        bool blink = doc["blink"] | false;
        applyEffectExclusive(RingEffect::NONE);
        ringController.setSolid(red, green, blue, on, blink);
        configController.markDirty();
        broadcastRingStatus();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "getRing") == 0)
    {
        broadcastRingStatus();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "allOff") == 0)
    {
        cancelBambuMode();
        applyEffectExclusive(RingEffect::NONE);
        uint32_t c = ringController.getColor();
        ringController.setSolid((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, false, false);
        configController.markDirty();
        broadcastRingStatus();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "setSpinner") == 0)
    {
        bool on = doc["on"] | false;
        if (on) cancelBambuMode();
        ringController.setSpinner(on,
            doc["r"]     | ringController.getSpinnerR(),
            doc["g"]     | ringController.getSpinnerG(),
            doc["b"]     | ringController.getSpinnerB(),
            doc["tail"]  | ringController.getSpinnerTail(),
            doc["speed"] | ringController.getSpinnerSpeed(),
            doc["cw"]    | ringController.getSpinnerCW());
        configController.markDirty();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "setRainbow") == 0)
    {
        bool on = doc["on"] | false;
        if (on) cancelBambuMode();
        ringController.setRainbow(on, doc["rainbowCycleTime"] | ringController.getRainbowCycleTime());
        configController.markDirty();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "setParty") == 0)
    {
        bool on = doc["on"] | false;
        if (on) cancelBambuMode();
        ringController.setParty(on, doc["partyMadness"] | -1);
        configController.markDirty();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "setChase") == 0)
    {
        bool on = doc["on"] | false;
        if (on) cancelBambuMode();
        ringController.setChase(on,
            doc["r"]     | ringController.getChaseR(),
            doc["g"]     | ringController.getChaseG(),
            doc["b"]     | ringController.getChaseB(),
            doc["speed"] | ringController.getChaseSpeed());
        configController.markDirty();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "setClock") == 0)
    {
        bool on = doc["on"] | false;
        if (on) cancelBambuMode();
        // Hand colours are optional: anything left out keeps its stored value.
        ringController.setClockColor(CLOCK_HOURS,
            doc["hr"] | ringController.getClockColor(CLOCK_HOURS, 0),
            doc["hg"] | ringController.getClockColor(CLOCK_HOURS, 1),
            doc["hb"] | ringController.getClockColor(CLOCK_HOURS, 2));
        ringController.setClockColor(CLOCK_MINUTES,
            doc["mr"] | ringController.getClockColor(CLOCK_MINUTES, 0),
            doc["mg"] | ringController.getClockColor(CLOCK_MINUTES, 1),
            doc["mb"] | ringController.getClockColor(CLOCK_MINUTES, 2));
        ringController.setClockColor(CLOCK_SECONDS,
            doc["sr"] | ringController.getClockColor(CLOCK_SECONDS, 0),
            doc["sg"] | ringController.getClockColor(CLOCK_SECONDS, 1),
            doc["sb"] | ringController.getClockColor(CLOCK_SECONDS, 2));
        ringController.setClock(on);
        configController.markDirty();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "setProgress") == 0)
    {
        bool on = doc["on"] | false;
        if (on) cancelBambuMode();
        ringController.setProgress(on,
            doc["pct"] | ringController.getProgressPercent(),
            doc["fr"]  | ringController.getProgressFR(),
            doc["fg"]  | ringController.getProgressFG(),
            doc["fb"]  | ringController.getProgressFB(),
            doc["br"]  | ringController.getProgressBR(),
            doc["bg"]  | ringController.getProgressBG(),
            doc["bb"]  | ringController.getProgressBB());
        configController.markDirty();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "setBambuMode") == 0)
    {
        bool    mode    = doc["bambuMode"]      | false;
        uint8_t timeout = doc["idleTimeoutMin"] | (uint8_t)5;
        bambuController.setBambuMode(mode);
        bambuController.setIdleTimeoutMin(timeout);
        configController.setBambuMode(mode);
        configController.setIdleTimeoutMin(timeout);
        bambuController.resetIdle();
        if (mode) {
            applyEffectExclusive(RingEffect::NONE);
            broadcastEffectStatus();
            if (bambuController.isConnected()) {
                BambuState cur = bambuController.getState();
                applyBambuRingState(cur);
                if (cur == BambuState::IDLE || cur == BambuState::FINISH)
                    bambuController.markIdle();
            }
        } else {
            ringController.cancelOverlay();
        }
        {
            JsonDocument bdoc;
            bdoc["type"]      = "bambuConfig";
            bdoc["bambuMode"] = mode;
            String bmsg; serializeJson(bdoc, bmsg);
            ws.textAll(bmsg);
        }
        sendBambuConfig(nullptr);
        return;
    }
    else if (strcmp(type, "setGeometry") == 0)
    {
        ringController.setGeometry(doc["origin"]  | ringController.getOrigin(),
                                   doc["reverse"] | ringController.getReverse());
        configController.markDirty();
        broadcastEffectStatus();
    }
    else if (strcmp(type, "wipe") == 0)
    {
        cancelBambuMode();
        ringController.startWipe(doc["r"] | 255, doc["g"] | 255, doc["b"] | 255,
                                 doc["speed"] | 60);
        broadcastEffectStatus();
    }
    else if (strcmp(type, "morse") == 0)
    {
        cancelBambuMode();
        ringController.startMorse(doc["text"] | "SOS");
        broadcastEffectStatus();
    }
    else if (strcmp(type, "randomYesNo") == 0)
    {
        cancelBambuMode();
        // "pick": 1 bets on green, 0 bets on red, absent means no bet.
        ringController.startRandomYesNo(doc["pick"] | -1);
        broadcastEffectStatus();
    }
    else if (strcmp(type, "weatherColor") == 0)
    {
        cancelBambuMode();
        Serial.printf("[weatherColor] valid=%d temp=%.1f\n",
                      geoController.weather.valid, geoController.weather.temperature);
        applyWeatherColor();
    }
    else if (strcmp(type, "airQualityColor") == 0)
    {
        cancelBambuMode();
        Serial.printf("[airQualityColor] valid=%d pm2_5=%.1f\n",
                      geoController.airQuality.valid, geoController.airQuality.pm2_5);
        applyAirQualityColor();
    }
    else if (strcmp(type, "setConfig") == 0)
    {
        configController.setMakeChangesPersistent(doc["makeChangesPersistent"] | true);
        broadcastConfigStatus();
    }
    else if (strcmp(type, "setLocation") == 0)
    {
        float lat = doc["latitude"]  | 0.0f;
        float lon = doc["longitude"] | 0.0f;
        configController.setLocation(lat, lon);
        geoController.setLocation(lat, lon);
        Serial.printf("[Geo] Location updated: %.6f, %.6f\n", lat, lon);
    }
    else if (strcmp(type, "startOTA") == 0)
    {
        xTaskCreate([](void*) {
            otaController.start();
            vTaskDelete(NULL);
        }, "ota", 8192, NULL, 1, NULL);
    }
    else if (strcmp(type, "setWifi") == 0)
    {
        String newName     = doc["deviceName"] | wifiManager.deviceName.c_str();
        String newNtp      = doc["ntpServer"]  | wifiManager.ntpServer.c_str();
        String newTz       = doc["timezone"]   | wifiManager.timezone.c_str();
        String newSSID     = doc["ssid"]       | "";
        String newPassword = doc["password"]   | "";
        bool   dhcpMode    = doc["dhcp"]       | true;
        String ipStr      = doc["ip"]      | "";
        String subnetStr  = doc["subnet"]  | "";
        String gatewayStr = doc["gateway"] | "";
        String dnsStr     = doc["dns"]     | "";
        IPAddress localIP, subnet, gateway, dns;
        if (!dhcpMode &&
            (!localIP.fromString(ipStr) || !subnet.fromString(subnetStr) ||
             !gateway.fromString(gatewayStr) || !dns.fromString(dnsStr)))
        {
            mqttController.publish("{\"type\":\"status\",\"status\":\"error\",\"message\":\"Invalid IP\"}");
            return;
        }
        wifiManager.saveConfig(newName, newNtp, newTz, newSSID, newPassword,
                               dhcpMode, localIP, subnet, gateway, dns);
        mqttController.publish("{\"type\":\"status\",\"status\":\"saved\",\"reboot\":true}");
        delay(1000);
        ESP.restart();
    }
    else if (strcmp(type, "setMqtt") == 0)
    {
        mqttController.applyConfig(
            doc["broker"]   | "",
            doc["port"]     | 1883,
            doc["username"] | "",
            doc["password"] | "",
            doc["clientId"] | "ringlight",
            doc["topic"]    | "ringlight",
            doc["enabled"]  | false);
        mqttController.saveConfig();
        mqttController.publish("{\"type\":\"status\",\"status\":\"saved\",\"message\":\"MQTT config saved\"}");
    }
}

// ─── WebSocket ────────────────────────────────────────────────────────────────

void handleWebSocketMessage(AsyncWebSocketClient *client, uint8_t *data, size_t len)
{
    JsonDocument doc;
    if (deserializeJson(doc, data, len))
    {
        client->text("{\"type\":\"status\",\"status\":\"error\",\"message\":\"Invalid JSON\"}");
        return;
    }

    const char *type = doc["type"];
    if (!type) return;

    // setWifi needs a client-specific reply before the device restarts
    if (strcmp(type, "setWifi") == 0)
    {
        String newName     = doc["deviceName"] | wifiManager.deviceName.c_str();
        String newNtp      = doc["ntpServer"]  | wifiManager.ntpServer.c_str();
        String newTz       = doc["timezone"]   | wifiManager.timezone.c_str();
        String newSSID     = doc["ssid"]       | "";
        String newPassword = doc["password"]   | "";
        bool   dhcpMode    = doc["dhcp"]       | true;
        String ipStr      = doc["ip"]      | "";
        String subnetStr  = doc["subnet"]  | "";
        String gatewayStr = doc["gateway"] | "";
        String dnsStr     = doc["dns"]     | "";
        IPAddress localIP, subnet, gateway, dns;
        if (!dhcpMode &&
            (!localIP.fromString(ipStr) || !subnet.fromString(subnetStr) ||
             !gateway.fromString(gatewayStr) || !dns.fromString(dnsStr)))
        {
            client->text("{\"type\":\"status\",\"status\":\"error\",\"message\":\"Invalid IP configuration\"}");
            return;
        }
        wifiManager.saveConfig(newName, newNtp, newTz, newSSID, newPassword,
                               dhcpMode, localIP, subnet, gateway, dns);
        client->text("{\"type\":\"status\",\"status\":\"saved\",\"reboot\":true}");
        delay(1000);
        ESP.restart();
        return;
    }

    if (strcmp(type, "getWifi") == 0) { sendWifiConfig(client); return; }
    if (strcmp(type, "getMqtt") == 0) { sendMqttConfig(client); return; }
    if (strcmp(type, "getBambu") == 0) { sendBambuConfig(client); return; }

    if (strcmp(type, "consoleOpen")  == 0) { _consoleViewerIds.insert(client->id()); return; }
    if (strcmp(type, "consoleClose") == 0) { _consoleViewerIds.erase(client->id());  return; }
    if (strcmp(type, "infoOpen")     == 0) { _infoViewerIds.insert(client->id());
                                             sendSysInfo(client); return; }
    if (strcmp(type, "infoClose")    == 0) { _infoViewerIds.erase(client->id());     return; }
    if (strcmp(type, "bambuOpen")    == 0) { _bambuViewerIds.insert(client->id());   return; }
    if (strcmp(type, "bambuClose")   == 0) { _bambuViewerIds.erase(client->id());    return; }

    if (strcmp(type, "setMqtt") == 0)
    {
        mqttController.applyConfig(
            doc["broker"]   | "",
            doc["port"]     | 1883,
            doc["username"] | "",
            doc["password"] | "",
            doc["clientId"] | "ringlight",
            doc["topic"]    | "ringlight",
            doc["enabled"]  | false);
        mqttController.saveConfig();
        client->text("{\"type\":\"status\",\"status\":\"saved\",\"message\":\"MQTT config saved\"}");
        return;
    }

    if (strcmp(type, "setBambu") == 0)
    {
        bambuController.applyConfig(
            doc["ip"]         | "",
            doc["serial"]     | "",
            doc["accessCode"] | "",
            doc["enabled"]    | false);
        bambuController.saveConfig();
        sendBambuConfig(client);
        return;
    }

    if (strcmp(type, "setBambuMode") == 0)
    {
        bool    mode    = doc["bambuMode"]      | false;
        uint8_t timeout = doc["idleTimeoutMin"] | (uint8_t)5;
        bambuController.setBambuMode(mode);
        bambuController.setIdleTimeoutMin(timeout);
        configController.setBambuMode(mode);
        configController.setIdleTimeoutMin(timeout);
        bambuController.resetIdle();
        if (mode) {
            applyEffectExclusive(RingEffect::NONE);
            broadcastEffectStatus();
            if (bambuController.isConnected()) {
                BambuState cur = bambuController.getState();
                applyBambuRingState(cur);
                if (cur == BambuState::IDLE || cur == BambuState::FINISH)
                    bambuController.markIdle();
            }
        } else {
            ringController.cancelOverlay();
        }
        {
            JsonDocument bdoc;
            bdoc["type"]      = "bambuConfig";
            bdoc["bambuMode"] = mode;
            String bmsg; serializeJson(bdoc, bmsg);
            ws.textAll(bmsg);
        }
        mqttController.publishSwitchState("bambu_mode", mode);
        sendBambuConfig(client);
        return;
    }

    if (strcmp(type, "setBambuStateColor") == 0)
    {
        const char* stateName = doc["state"] | "";
        uint8_t r = doc["r"] | 0;
        uint8_t g = doc["g"] | 0;
        uint8_t b = doc["b"] | 0;
        BambuState s = BambuLabController::stateFromString(stateName);
        bambuController.setStateColor(s, r, g, b);
        bambuController.saveConfig();
        if (bambuController.getBambuMode() && bambuController.getState() == s)
            applyBambuRingState(s);
        return;
    }

    if (strcmp(type, "getTimers") == 0) { timerController.sendTimers(client); return; }

    if (strcmp(type, "setTimers") == 0)
    {
        timerController.setTimers(doc["timers"].as<JsonArray>());
        client->text("{\"type\":\"status\",\"status\":\"saved\",\"message\":\"Timers saved\"}");
        return;
    }

    if (strcmp(type, "consoleCmd") == 0)
    {
        String cmd = doc["cmd"] | "";
        if (cmd.length() > 0) serialConsole.executeFromWeb(cmd);
        return;
    }

    processCommand(doc);
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    switch (type)
    {
        case WS_EVT_CONNECT:
            Serial.printf("WebSocket client #%u connected from %s\n",
                          client->id(), client->remoteIP().toString().c_str());
            client->keepAlivePeriod(10);  // native WS ping frame every 10 s
            sendRingStatus(client);
            sendEffectStatus(client);
            sendWifiConfig(client);
            sendMqttConfig(client);
            sendBambuConfig(client);
            timerController.sendTimers(client);
            sendConfigStatus(client);
            sendSysInfoStatic(client);
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("WebSocket client #%u disconnected\n", client->id());
            _consoleViewerIds.erase(client->id());
            _infoViewerIds.erase(client->id());
            _bambuViewerIds.erase(client->id());
            break;
        case WS_EVT_DATA:
            handleWebSocketMessage(client, data, len);
            break;
        case WS_EVT_PONG:
        case WS_EVT_ERROR:
            break;
    }
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────

void onMQTTMessage(uint8_t *payload, unsigned int length)
{
    JsonDocument doc;
    if (deserializeJson(doc, payload, length))
    {
        mqttController.publish("{\"type\":\"status\",\"status\":\"error\",\"message\":\"Invalid JSON\"}");
        return;
    }
    processCommand(doc);
}

// ─── Web server ───────────────────────────────────────────────────────────────

void setupWebServer()
{
    ws.onEvent(onWebSocketEvent);
    webServer.addHandler(&ws);

    webServer.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "pong");
    });

    static String cmdBody;
    webServer.on("/cmd", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            JsonDocument doc;
            if (deserializeJson(doc, cmdBody)) { request->send(400, "text/plain", "invalid json"); return; }
            processCommand(doc);
            request->send(200, "text/plain", "ok");
        },
        nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index == 0) cmdBody = "";
            cmdBody += String((char*)data).substring(0, len);
        }
    );

    webServer.on("/backup", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        auto readFile = [&](const char* path, const char* key) {
            if (!LittleFS.exists(path)) return;
            File f = LittleFS.open(path, "r");
            if (!f) return;
            JsonDocument tmp;
            if (!deserializeJson(tmp, f)) doc[key] = tmp;
            f.close();
        };
        readFile("/config.json", "config");
        readFile("/wifi.json",   "wifi");
        readFile("/mqtt.json",   "mqtt");
        readFile("/timers.json", "timers");
        readFile("/bambu.json",  "bambu");
        String out;
        serializeJsonPretty(doc, out);
        AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", out);
        resp->addHeader("Content-Disposition", "attachment; filename=\"ringlight-backup.json\"");
        request->send(resp);
    });

    static String restoreBody;
    webServer.on("/restore", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            JsonDocument doc;
            if (deserializeJson(doc, restoreBody)) { request->send(400, "text/plain", "invalid json"); return; }
            auto writeFile = [&](const char* path, const char* key) {
                if (doc[key].isNull()) return;
                File f = LittleFS.open(path, "w");
                if (!f) return;
                serializeJsonPretty(doc[key], f);
                f.close();
            };
            writeFile("/config.json", "config");
            writeFile("/wifi.json",   "wifi");
            writeFile("/mqtt.json",   "mqtt");
            writeFile("/timers.json", "timers");
            writeFile("/bambu.json",  "bambu");
            Serial.println("[Backup] Restore completed.");
            request->send(200, "text/plain", "restore completed");
        },
        nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index == 0) restoreBody = "";
            restoreBody += String((char*)data).substring(0, len);
        }
    );

    webServer.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "restarting");
        delay(200);
        ESP.restart();
    });

    webServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    webServer.begin();
}

// ─── Watchdog ─────────────────────────────────────────────────────────────────

#define WDT_TIMEOUT_S 60

// Arms the task watchdog on the Arduino loop task. If loop() ever stops feeding
// it — deadlock, corrupted state, a network call that never returns — the device
// panics and reboots instead of sitting there unresponsive. 60 s leaves room for
// the longest blocking call in loop() (PubSubClient's 15 s socket timeout).
static void setupWatchdog()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms     = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;
    cfg.trigger_panic  = true;
    // Arduino 3.x already initialises the TWDT at boot, so reconfigure it
    // instead of initialising a second time.
    if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE)
        esp_task_wdt_reconfigure(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL);
    Serial.printf("[WDT] Task watchdog armed (%d s)\n", WDT_TIMEOUT_S);
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    delay(200);
#ifdef IMPROV_ENABLED
    // Sent before LittleFS (~600 ms from USB reset) so ImprovSerial detects the
    // device inside its 1 s window.
    improvAnnounceAuthorized();
#endif
    if (!LittleFS.begin(true))
    {
        // Returning here would leave loop() running against controllers that
        // were never initialised, and the watchdog would never be armed.
        Serial.println("LittleFS mount failed - rebooting in 5 s");
        delay(5000);
        ESP.restart();
    }
#ifdef IMPROV_ENABLED
    if (!LittleFS.exists("/wifi.json"))
        runImprovSetup(FIRMWARE_VERSION);
#endif

    ringController.begin();
    configController.begin(ringController);
    // From here the ring renders on its own task, so nothing loop() does can
    // make the animations stutter.
    ringController.startRenderTask();

    ringController.onRandomResult = [](bool yes, int pick) {
        JsonDocument doc;
        doc["type"] = "randomResult";
        doc["yes"]  = yes;
        doc["pick"] = pick;
        // The firmware owns the outcome, so every client sees the same verdict.
        if (pick >= 0) doc["win"] = ((pick == 1) == yes);
        String msg; serializeJson(doc, msg);
        ws.textAll(msg);
    };
    ringController.onAnimationEnd = []() {
        broadcastRingStatus();
        broadcastEffectStatus();
    };

    timerController.begin();
    timerController.onAllOff = []() {
        cancelBambuMode();
        applyEffectExclusive(RingEffect::NONE);
        uint32_t c = ringController.getColor();
        ringController.setSolid((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, false, false);
        broadcastRingStatus();
        broadcastEffectStatus();
    };
    timerController.onRing = [](uint8_t r, uint8_t g, uint8_t b) {
        cancelBambuMode();
        applyEffectExclusive(RingEffect::NONE);
        ringController.setSolid(r, g, b, true, false);
        broadcastRingStatus();
        broadcastEffectStatus();
    };
    timerController.onWipe = [](uint8_t r, uint8_t g, uint8_t b) {
        cancelBambuMode();
        ringController.startWipe(r, g, b, 60);
    };
    timerController.onSpinner = [](bool on) {
        if (on) cancelBambuMode();
        ringController.setSpinner(on, ringController.getSpinnerR(), ringController.getSpinnerG(),
                                  ringController.getSpinnerB(), ringController.getSpinnerTail(),
                                  ringController.getSpinnerSpeed(), ringController.getSpinnerCW());
        broadcastEffectStatus();
    };
    timerController.onRainbow = [](bool on) {
        if (on) cancelBambuMode();
        ringController.setRainbow(on, ringController.getRainbowCycleTime());
        broadcastEffectStatus();
    };
    timerController.onParty = [](bool on) {
        if (on) cancelBambuMode();
        ringController.setParty(on, ringController.getPartyMadness());
        broadcastEffectStatus();
    };
    timerController.onChase = [](bool on) {
        if (on) cancelBambuMode();
        ringController.setChase(on, ringController.getChaseR(), ringController.getChaseG(),
                                ringController.getChaseB(), ringController.getChaseSpeed());
        broadcastEffectStatus();
    };
    timerController.onClock = [](bool on) {
        if (on) cancelBambuMode();
        ringController.setClock(on);
        broadcastEffectStatus();
    };
    timerController.onProgress = [](bool on, uint8_t pct) {
        if (on) cancelBambuMode();
        ringController.setProgress(on, pct,
                                   ringController.getProgressFR(), ringController.getProgressFG(),
                                   ringController.getProgressFB(), ringController.getProgressBR(),
                                   ringController.getProgressBG(), ringController.getProgressBB());
        broadcastEffectStatus();
    };
    timerController.onRandomYesNo = []() {
        cancelBambuMode();
        ringController.startRandomYesNo();
        broadcastEffectStatus();
    };
    timerController.onMorse = [](const String& text) {
        cancelBambuMode();
        ringController.startMorse(text.c_str());
        broadcastEffectStatus();
    };
    timerController.onWeatherColor    = []() { cancelBambuMode(); applyWeatherColor(); };
    timerController.onAirQualityColor = []() { cancelBambuMode(); applyAirQualityColor(); };
    timerController.onBambuMode = [](bool on) {
        bambuController.setBambuMode(on);
        configController.setBambuMode(on);
        bambuController.resetIdle();
        if (on) {
            applyEffectExclusive(RingEffect::NONE);
            broadcastEffectStatus();
            if (bambuController.isConnected()) {
                BambuState cur = bambuController.getState();
                applyBambuRingState(cur);
                if (cur == BambuState::IDLE || cur == BambuState::FINISH)
                    bambuController.markIdle();
            }
        } else {
            ringController.cancelOverlay();
        }
        JsonDocument doc;
        doc["type"]      = "bambuConfig";
        doc["bambuMode"] = on;
        String msg; serializeJson(doc, msg);
        ws.textAll(msg);
        mqttController.publishSwitchState("bambu_mode", on);
    };

    networkManager.begin(wifiManager);

    geoController.begin(configController.getLatitude(), configController.getLongitude());
    geoController.onWeatherUpdate = []() {
        mqttController.publishWeather(
            geoController.weather.temperature,
            geoController.weather.humidity,
            conditionToString(geoController.weather.condition));
        broadcastSysInfoStatic();
    };
    geoController.onAirQualityUpdate = []() {
        mqttController.publishAirQuality(
            geoController.airQuality.pm2_5,
            geoController.airQuality.pm10,
            geoController.airQuality.no2);
        broadcastSysInfoStatic();
    };

    configTzTime(wifiManager.timezone.c_str(), wifiManager.ntpServer.c_str());
    MDNS.begin(wifiManager.deviceName.c_str());

    alexaController.begin(webServer, ringController);
    alexaController.onChanged = []() {
        broadcastRingStatus();
        broadcastEffectStatus();
        configController.markDirty();
    };

    setupWebServer();

    mqttController.connectedHandler = []() {
        broadcastRingStatus();
        broadcastEffectStatus();
    };
    mqttController.begin(onMQTTMessage);

    ArduinoOTA
        .onStart([]()   { Serial.println("OTA Start"); })
        .onEnd([]()     { Serial.println("\nOTA End"); })
        .onProgress([](unsigned int progress, unsigned int total)
                    { Serial.printf("Progress: %u%%\r", (progress * 100) / total); })
        .onError([](ota_error_t error)
                 { Serial.printf("Error[%u]\n", error); });
    ArduinoOTA.begin();

    bambuController.loadConfig();
    bambuController.setBambuMode(configController.getBambuMode());
    bambuController.setIdleTimeoutMin(configController.getIdleTimeoutMin());
    bambuController.onStateChange = [](BambuState state) {
        JsonDocument doc;
        doc["type"]      = "bambuStatus";
        doc["state"]     = BambuLabController::stateToString(state);
        doc["connected"] = bambuController.isConnected();
        doc["percent"]   = bambuController.getPercent();
        String msg; serializeJson(doc, msg);
        ws.textAll(msg);
        if (state == BambuState::IDLE || state == BambuState::FINISH)
            bambuController.markIdle();
        else
            bambuController.resetIdle();
        if (bambuController.getBambuMode()) applyBambuRingState(state);
    };
    bambuController.onProgress = [](uint8_t pct) {
        mqttController.publishProgress(pct);
        if (_hasBambuOrInfoViewers()) {
            JsonDocument doc;
            doc["type"]    = "bambuConfig";
            doc["percent"] = pct;
            String msg; serializeJson(doc, msg);
            _sendToBambuAndInfoViewers(msg);
        }
        // Redraw the arc immediately so the ring tracks the print.
        if (bambuController.getBambuMode() && bambuController.getState() == BambuState::RUNNING)
            applyBambuRingState(BambuState::RUNNING);
    };
    bambuController.onIdleTimeout = []() {
        ringController.showColor(0, 0, 0, BAMBU_OVERLAY_HOLD_MS);
    };
    // Suppress low-level TLS error spam from the ESP32 core; reconnects are
    // handled by our own logic and logged as "[BambuLab] connect failed".
    esp_log_level_set("ssl_client", ESP_LOG_NONE);
    bambuController.begin();

    serialConsole.setBambu(bambuController);
    serialConsole.setGeo(geoController);
    serialConsole.setRing(ringController);
    serialConsole.setNetwork(networkManager);
    serialConsole.onRingChanged = []() {
        configController.markDirty();
        broadcastRingStatus();
        broadcastEffectStatus();
    };
    serialConsole.begin();

    // Mark the firmware valid — cancels the automatic rollback. If setup() never
    // reaches this point (crash, watchdog, panic) the bootloader reverts to the
    // previous firmware on the next boot.
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[OTA] Firmware validated — rollback cancelled");

    otaController.onBeforeStart = []() {
        // Drop the BambuLab and MQTT TLS/TCP buffers before OTA opens its own
        // HTTPS connections, otherwise the SSL allocation fails.
        bambuController.applyConfig("", "", "", false);
        mqttController.applyConfig("", 1883, "", "", "", "", false);
    };
    otaController.onStatus = [](const char* step) {
        JsonDocument doc;
        doc["type"] = "otaStatus";
        doc["step"] = step;
        String msg; serializeJson(doc, msg);
        ws.textAll(msg);
    };
    otaController.onProgress = [](const char* step, int pct) {
        JsonDocument doc;
        doc["type"] = "otaProgress";
        doc["step"] = step;
        doc["pct"]  = pct;
        String msg; serializeJson(doc, msg);
        ws.textAll(msg);
    };

    setupWatchdog();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────

void loop()
{
    esp_task_wdt_reset();
    {
        static uint32_t _loopStart = 0;
        uint32_t now = millis();
        if (_loopStart) {
            uint32_t elapsed = now - _loopStart;
            if (elapsed > _loopMaxMs) _loopMaxMs = elapsed;
        }
        _loopStart = now;
    }
    ArduinoOTA.handle();
    alexaController.loop();
    networkManager.handleFallbackLogic();
    // The frames are drawn by the render task; this only fires the callbacks it
    // raised, so WebSocket and JSON work stays on this task. If the task could
    // not be created at boot, drive the animation from here instead.
    if (!ringController.hasRenderTask()) ringController.update();
    ringController.dispatchEvents();
    timerController.loop();
    ws.cleanupClients();

    if (!_consoleViewerIds.empty()) {
        String msg = teeSerial.drainOne();
        if (msg.length()) {
            for (uint32_t id : _consoleViewerIds) {
                AsyncWebSocketClient* c = ws.client(id);
                if (c) c->text(msg);
            }
        }
    }
    {
        static unsigned long _lastInfoPush = 0;
        if (!_infoViewerIds.empty() && millis() - _lastInfoPush >= 1000) {
            _lastInfoPush = millis();
            for (uint32_t id : _infoViewerIds) {
                AsyncWebSocketClient* c = ws.client(id);
                if (c) sendSysInfo(c);
            }
        }
    }
    {
        static bool _lastBambuConnected = false;
        bool nowConnected = bambuController.isConnected();
        if (nowConnected != _lastBambuConnected) {
            _lastBambuConnected = nowConnected;
            // Track the transition either way, build the message only when
            // somebody is listening.
            if (_hasBambuOrInfoViewers()) {
                JsonDocument doc;
                doc["type"]      = "bambuConfig";
                doc["connected"] = nowConnected;
                if (!nowConnected) doc["state"] = "offline";
                String msg; serializeJson(doc, msg);
                _sendToBambuAndInfoViewers(msg);
            }
        }
    }
    {
        // Without the viewer check this allocated a JsonDocument and a String
        // every single second for nobody, fragmenting the heap over days.
        static unsigned long _lastIdlePush = 0;
        if (_hasBambuOrInfoViewers() && millis() - _lastIdlePush >= 1000) {
            _lastIdlePush = millis();
            int32_t idleSec = bambuController.getIdleSec();
            if (idleSec >= 0) {
                JsonDocument doc;
                doc["type"]    = "bambuConfig";
                doc["idleSec"] = idleSec;
                String msg; serializeJson(doc, msg);
                _sendToBambuAndInfoViewers(msg);
            }
        }
    }
    // Re-arm the BambuLab overlay: applyBambuRingState() only runs on a state
    // change, so a printer sitting in one state longer than the overlay hold
    // time would otherwise lose the ring.
    {
        static unsigned long _lastBambuOverlay = 0;
        if (bambuController.getBambuMode() && bambuController.isConnected() &&
            millis() - _lastBambuOverlay >= BAMBU_OVERLAY_REFRESH_MS) {
            _lastBambuOverlay = millis();
            if (bambuController.isIdleLedOff())
                ringController.showColor(0, 0, 0, BAMBU_OVERLAY_HOLD_MS);
            else if (bambuStateHasColor(bambuController.getState()))
                applyBambuRingState(bambuController.getState());
        }
    }

    mqttController.loop();
    configController.loop();
    geoController.loop();
    serialConsole.loop();
    bambuController.loop();
}
