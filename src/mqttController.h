#pragma once
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

typedef void (*MQTTCommandHandler)(uint8_t *, unsigned int);
typedef void (*MQTTConnectedHandler)();

class MQTTController
{
private:
    WiFiClient   wifiClient;
    PubSubClient mqttClient;

    String broker;
    int    port        = 1883;
    String username;
    String password;
    String clientId    = "ringlight";
    String topicPrefix = "ringlight";
    String cmdTopic;
    String statusTopic;
    bool   enabled     = false;

    unsigned long lastReconnectAttempt = 0;
    unsigned long _lastRssiPublish     = 0;
    bool          pendingConnected     = false;

    static MQTTController *_instance;

    static void onMessage(char *topic, uint8_t *payload, unsigned int length)
    {
        if (!_instance || !_instance->commandHandler) return;

        // Home Assistant light command for the whole ring: translate the HA
        // JSON light schema into our own setRing message.
        String haCmdTopic = _instance->topicPrefix + "/cmd/ring";
        if (String(topic) == haCmdTopic)
        {
            JsonDocument src;
            if (deserializeJson(src, payload, length)) return;
            JsonDocument out;
            out["type"] = "setRing";
            out["on"]   = (strcmp(src["state"] | "", "ON") == 0);
            int brightness = src["brightness"] | 255;
            if (!src["color"].isNull())
            {
                out["r"] = (int)(src["color"]["r"] | 0) * brightness / 255;
                out["g"] = (int)(src["color"]["g"] | 0) * brightness / 255;
                out["b"] = (int)(src["color"]["b"] | 0) * brightness / 255;
            }
            else if (!src["brightness"].isNull())
            {
                out["brightness"] = brightness;
            }
            out["blink"] = false;
            String translated;
            serializeJson(out, translated);
            _instance->commandHandler((uint8_t *)translated.c_str(), translated.length());
            return;
        }

        _instance->commandHandler(payload, length);
    }

    bool reconnect()
    {
        Serial.printf("MQTT connecting to %s:%d...\n", broker.c_str(), port);
        bool ok = username.isEmpty()
            ? mqttClient.connect(clientId.c_str())
            : mqttClient.connect(clientId.c_str(), username.c_str(), password.c_str());
        if (ok)
        {
            Serial.println("MQTT connected, subscribing to " + cmdTopic);
            mqttClient.subscribe(cmdTopic.c_str());
            String ringCmdTopic = topicPrefix + "/cmd/ring";
            mqttClient.subscribe(ringCmdTopic.c_str());
            publishDiscovery();
            pendingConnected = true;
        }
        else
        {
            Serial.printf("MQTT failed, rc=%d\n", mqttClient.state());
        }
        return ok;
    }

public:
    MQTTCommandHandler  commandHandler  = nullptr;
    MQTTConnectedHandler connectedHandler = nullptr;

    bool loadConfig()
    {
        if (!LittleFS.exists("/mqtt.json")) return false;
        File file = LittleFS.open("/mqtt.json", "r");
        if (!file) return false;
        JsonDocument doc;
        if (deserializeJson(doc, file)) { file.close(); return false; }
        file.close();
        broker      = doc["broker"]   | "";
        port        = doc["port"]     | 1883;
        username    = doc["username"] | "";
        password    = doc["password"] | "";
        clientId    = doc["clientId"] | "ringlight";
        topicPrefix = doc["topic"]    | "ringlight";
        enabled     = doc["enabled"]  | false;
        cmdTopic    = topicPrefix + "/cmd";
        statusTopic = topicPrefix + "/status";
        return !broker.isEmpty();
    }

    void begin(MQTTCommandHandler handler)
    {
        commandHandler = handler;
        _instance      = this;
        if (!loadConfig() || !enabled)
        {
            Serial.println("MQTT: disabled or broker not configured, skipping");
            return;
        }
        mqttClient.setClient(wifiClient);
        mqttClient.setServer(broker.c_str(), port);
        mqttClient.setBufferSize(2048);
        mqttClient.setCallback(onMessage);
        reconnect();
    }

    void loop()
    {
        if (broker.isEmpty() || !enabled) return;
        if (!mqttClient.connected())
        {
            unsigned long now = millis();
            if (now - lastReconnectAttempt >= 5000)
            {
                lastReconnectAttempt = now;
                reconnect();
            }
        }
        else
        {
            mqttClient.loop();
            if (pendingConnected)
            {
                pendingConnected = false;
                if (connectedHandler) connectedHandler();
            }
            if (millis() - _lastRssiPublish >= 60000)
            {
                _lastRssiPublish = millis();
                publishRssi(WiFi.RSSI());
            }
        }
    }

    void publish(const String &payload)
    {
        if (!mqttClient.connected() || statusTopic.isEmpty()) return;
        mqttClient.publish(statusTopic.c_str(), payload.c_str());
    }

    bool isConnected() { return mqttClient.connected(); }

    String getBroker()      { return broker;      }
    int    getPort()        { return port;        }
    String getUsername()    { return username;    }
    String getPassword()    { return password;    }
    String getClientId()    { return clientId;    }
    String getTopicPrefix() { return topicPrefix; }

    bool saveConfig()
    {
        Serial.println("Saving MQTT config...");
        JsonDocument doc;
        doc["broker"]   = broker;
        doc["port"]     = port;
        doc["username"] = username;
        doc["password"] = password;
        doc["clientId"] = clientId;
        doc["topic"]    = topicPrefix;
        doc["enabled"]  = enabled;
        File file = LittleFS.open("/mqtt.json", "w");
        if (!file) return false;
        serializeJsonPretty(doc, file);
        file.close();
        return true;
    }

    void publishSwitchState(const char *name, bool on)
    {
        if (!mqttClient.connected()) return;
        String topic = topicPrefix + "/status/switch/" + name;
        mqttClient.publish(topic.c_str(), on ? "ON" : "OFF", true);
    }

    void publishRingState(int r, int g, int b, bool on)
    {
        if (!mqttClient.connected()) return;
        String topic = topicPrefix + "/status/ring";
        JsonDocument doc;
        int maxC = max({r, g, b});
        doc["state"]      = on ? "ON" : "OFF";
        doc["color_mode"] = "rgb";
        doc["brightness"] = maxC;
        doc["color"]["r"] = maxC > 0 ? r * 255 / maxC : 0;
        doc["color"]["g"] = maxC > 0 ? g * 255 / maxC : 0;
        doc["color"]["b"] = maxC > 0 ? b * 255 / maxC : 0;
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(topic.c_str(), payload.c_str(), true);
    }

    void publishProgress(uint8_t pct)
    {
        if (!mqttClient.connected()) return;
        mqttClient.publish((topicPrefix + "/status/progress").c_str(),
                           String((int)pct).c_str(), true);
    }

    void publishDiscovery()
    {
        if (!mqttClient.connected()) return;

        // Single light entity for the whole ring.
        {
            String uid         = clientId + "_ring";
            String discTopic   = "homeassistant/light/" + uid + "/config";
            String stateTopic  = topicPrefix + "/status/ring";
            String ringCmdTopic = topicPrefix + "/cmd/ring";

            JsonDocument doc;
            doc["name"]          = "Ring";
            doc["unique_id"]     = uid;
            doc["schema"]        = "json";
            doc["state_topic"]   = stateTopic;
            doc["command_topic"] = ringCmdTopic;
            doc["supported_color_modes"][0] = "rgb";

            JsonObject dev        = doc["device"].to<JsonObject>();
            dev["identifiers"][0] = clientId;
            dev["name"]           = "Ringlight";
            dev["model"]          = "ESP32-C3";

            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
        }

        struct SwitchDef { const char *name; const char *id; const char *cmdOn; const char *cmdOff; };
        SwitchDef switches[] = {
            {"Spinner",  "spinner",  "{\"type\":\"setSpinner\",\"on\":true}",  "{\"type\":\"setSpinner\",\"on\":false}"},
            {"Rainbow",  "rainbow",  "{\"type\":\"setRainbow\",\"on\":true}",  "{\"type\":\"setRainbow\",\"on\":false}"},
            {"Party",    "party",    "{\"type\":\"setParty\",\"on\":true}",    "{\"type\":\"setParty\",\"on\":false}"},
            {"Chase",    "chase",    "{\"type\":\"setChase\",\"on\":true}",    "{\"type\":\"setChase\",\"on\":false}"},
            {"Clock",    "clock",    "{\"type\":\"setClock\",\"on\":true}",    "{\"type\":\"setClock\",\"on\":false}"},
            {"Progress", "progress", "{\"type\":\"setProgress\",\"on\":true}", "{\"type\":\"setProgress\",\"on\":false}"},
        };

        for (auto &sw : switches)
        {
            String uid        = clientId + "_" + sw.id;
            String discTopic  = "homeassistant/switch/" + uid + "/config";
            String stateTopic = topicPrefix + "/status/switch/" + sw.id;

            JsonDocument doc;
            doc["name"]          = sw.name;
            doc["unique_id"]     = uid;
            doc["state_topic"]   = stateTopic;
            doc["command_topic"] = cmdTopic;
            doc["payload_on"]    = sw.cmdOn;
            doc["payload_off"]   = sw.cmdOff;
            doc["state_on"]      = "ON";
            doc["state_off"]     = "OFF";

            JsonObject dev        = doc["device"].to<JsonObject>();
            dev["identifiers"][0] = clientId;
            dev["name"]           = "Ringlight";
            dev["model"]          = "ESP32-C3";

            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
        }

        struct { const char *name; const char *id; const char *cmd; } buttons[] = {
            {"Random Yes/No", "random_yn",     "{\"type\":\"randomYesNo\"}"},
            {"Weather Color", "weather_color", "{\"type\":\"weatherColor\"}"},
        };
        for (auto &b : buttons)
        {
            String uid   = clientId + "_" + b.id;
            String topic = "homeassistant/button/" + uid + "/config";

            JsonDocument doc;
            doc["name"]          = b.name;
            doc["unique_id"]     = uid;
            doc["command_topic"] = cmdTopic;
            doc["payload_press"] = b.cmd;

            JsonObject dev        = doc["device"].to<JsonObject>();
            dev["identifiers"][0] = clientId;
            dev["name"]           = "Ringlight";
            dev["model"]          = "ESP32-C3";

            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(topic.c_str(), payload.c_str(), true);
        }

        // Weather sensors
        struct { const char *name; const char *id; const char *devClass; const char *unit; } wSensors[] = {
            {"Temperature", "temperature", "temperature", "°C"},
            {"Humidity",    "humidity",    "humidity",    "%"},
            {"Condition",   "condition",   nullptr,       nullptr},
        };
        for (auto &s : wSensors)
        {
            String uid        = clientId + "_weather_" + s.id;
            String discTopic  = "homeassistant/sensor/" + uid + "/config";
            String stateTopic = topicPrefix + "/status/weather/" + s.id;

            JsonDocument doc;
            doc["name"]        = s.name;
            doc["unique_id"]   = uid;
            doc["state_topic"] = stateTopic;
            if (s.devClass) {
                doc["device_class"]   = s.devClass;
                doc["state_class"]    = "measurement";
            }
            if (s.unit) doc["unit_of_measurement"] = s.unit;

            JsonObject dev        = doc["device"].to<JsonObject>();
            dev["identifiers"][0] = clientId;
            dev["name"]           = "Ringlight";
            dev["model"]          = "ESP32-C3";

            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
        }

        // Air quality sensors
        struct { const char *name; const char *id; const char *unit; } aqSensors[] = {
            {"PM2.5",  "pm2_5", "µg/m³"},
            {"PM10",   "pm10",  "µg/m³"},
            {"NO₂",    "no2",   "µg/m³"},
        };
        for (auto &s : aqSensors)
        {
            String uid        = clientId + "_aq_" + s.id;
            String discTopic  = "homeassistant/sensor/" + uid + "/config";
            String stateTopic = topicPrefix + "/status/airquality/" + s.id;

            JsonDocument doc;
            doc["name"]                = s.name;
            doc["unique_id"]           = uid;
            doc["state_topic"]         = stateTopic;
            doc["state_class"]         = "measurement";
            doc["unit_of_measurement"] = s.unit;

            JsonObject dev        = doc["device"].to<JsonObject>();
            dev["identifiers"][0] = clientId;
            dev["name"]           = "Ringlight";
            dev["model"]          = "ESP32-C3";

            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
        }

        // Air quality color button
        {
            String uid   = clientId + "_air_quality_color";
            String topic = "homeassistant/button/" + uid + "/config";
            JsonDocument doc;
            doc["name"]          = "Air Quality Color";
            doc["unique_id"]     = uid;
            doc["command_topic"] = cmdTopic;
            doc["payload_press"] = "{\"type\":\"airQualityColor\"}";
            JsonObject dev        = doc["device"].to<JsonObject>();
            dev["identifiers"][0] = clientId;
            dev["name"]           = "Ringlight";
            dev["model"]          = "ESP32-C3";
            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(topic.c_str(), payload.c_str(), true);
        }

        // Progress sensor (percentage shown on the ring)
        {
            String uid        = clientId + "_progress";
            String discTopic  = "homeassistant/sensor/" + uid + "/config";
            String stateTopic = topicPrefix + "/status/progress";

            JsonDocument doc;
            doc["name"]                = "Progress";
            doc["unique_id"]           = uid;
            doc["state_topic"]         = stateTopic;
            doc["state_class"]         = "measurement";
            doc["unit_of_measurement"] = "%";

            JsonObject dev        = doc["device"].to<JsonObject>();
            dev["identifiers"][0] = clientId;
            dev["name"]           = "Ringlight";
            dev["model"]          = "ESP32-C3";

            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
        }

        // RSSI sensor
        {
            String uid        = clientId + "_rssi";
            String discTopic  = "homeassistant/sensor/" + uid + "/config";
            String stateTopic = topicPrefix + "/status/rssi";

            JsonDocument doc;
            doc["name"]                = "RSSI";
            doc["unique_id"]           = uid;
            doc["state_topic"]         = stateTopic;
            doc["device_class"]        = "signal_strength";
            doc["state_class"]         = "measurement";
            doc["unit_of_measurement"] = "dBm";
            doc["entity_category"]     = "diagnostic";

            JsonObject dev        = doc["device"].to<JsonObject>();
            dev["identifiers"][0] = clientId;
            dev["name"]           = "Ringlight";
            dev["model"]          = "ESP32-C3";

            String payload;
            serializeJson(doc, payload);
            mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
        }

        Serial.println("MQTT: HA discovery published");
    }

    void publishAirQuality(float pm2_5, float pm10, float no2)
    {
        if (!mqttClient.connected()) return;
        mqttClient.publish((topicPrefix + "/status/airquality/pm2_5").c_str(),  String(pm2_5,  1).c_str(), true);
        mqttClient.publish((topicPrefix + "/status/airquality/pm10").c_str(),   String(pm10,   1).c_str(), true);
        mqttClient.publish((topicPrefix + "/status/airquality/no2").c_str(),    String(no2,    1).c_str(), true);
    }

    void publishRssi(int rssi)
    {
        if (!mqttClient.connected()) return;
        mqttClient.publish((topicPrefix + "/status/rssi").c_str(),
            String(rssi).c_str(), true);
    }

    void publishWeather(float temperature, float humidity, const String &condition)
    {
        if (!mqttClient.connected()) return;
        mqttClient.publish((topicPrefix + "/status/weather/temperature").c_str(),
            String(temperature, 1).c_str(), true);
        mqttClient.publish((topicPrefix + "/status/weather/humidity").c_str(),
            String((int)humidity).c_str(), true);
        mqttClient.publish((topicPrefix + "/status/weather/condition").c_str(),
            condition.c_str(), true);
    }

    void applyConfig(const String &newBroker, int newPort, const String &newUsername,
                     const String &newPassword, const String &newClientId, const String &newTopic,
                     bool newEnabled)
    {
        Serial.println("Applying MQTT config...");
        enabled     = newEnabled;
        broker      = newBroker;
        port        = newPort;
        username    = newUsername;
        password    = newPassword;
        clientId    = newClientId;
        topicPrefix = newTopic;
        cmdTopic    = topicPrefix + "/cmd";
        statusTopic = topicPrefix + "/status";

        // Ensure the underlying WiFiClient is always set before any PubSubClient call
        mqttClient.setClient(wifiClient);
        mqttClient.setCallback(onMessage);
        mqttClient.setBufferSize(2048);

        if (!enabled || broker.isEmpty())
        {
            if (mqttClient.connected()) mqttClient.disconnect();
        }
        else
        {
            mqttClient.setServer(broker.c_str(), port);
            if (mqttClient.connected()) mqttClient.disconnect();
            lastReconnectAttempt = 0;
        }
    }

    bool getEnabled() { return enabled; }
};

MQTTController *MQTTController::_instance = nullptr;
