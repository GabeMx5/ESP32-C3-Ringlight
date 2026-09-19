#pragma once
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "ringController.h"

#define CONFIG_FILE            "/config.json"
#define CONFIG_SAVE_DELAY_MS   10000

// Persists the ring state to /config.json. Writes are debounced: markDirty()
// only records the time, and loop() commits once the ring has been quiet for
// CONFIG_SAVE_DELAY_MS, so dragging a colour picker does not hammer the flash.
class ConfigController
{
public:
    void begin(RingController &ring)
    {
        _ring = &ring;
        load();
    }

    void loop()
    {
        if (_dirtyTime > 0 && millis() - _dirtyTime >= CONFIG_SAVE_DELAY_MS)
        {
            save();
            _dirtyTime = 0;
        }
    }

    void markDirty()
    {
        if (_makeChangesPersistent) _dirtyTime = millis();
    }

    bool    getMakeChangesPersistent() { return _makeChangesPersistent; }
    float   getLatitude()              { return _latitude;       }
    float   getLongitude()             { return _longitude;      }
    bool    getBambuMode()             { return _bambuMode;      }
    uint8_t getIdleTimeoutMin()        { return _idleTimeoutMin; }

    void setBambuMode(bool v)          { _bambuMode = v;      markDirty(); }
    void setIdleTimeoutMin(uint8_t v)  { _idleTimeoutMin = v; markDirty(); }

    void setMakeChangesPersistent(bool value)
    {
        _makeChangesPersistent = value;
        save();
    }

    void setLocation(float lat, float lon)
    {
        _latitude  = lat;
        _longitude = lon;
        save();
    }

    void save()
    {
        JsonDocument doc;

        uint32_t c = _ring->getColor();
        JsonObject base = doc["base"].to<JsonObject>();
        base["r"]     = (c >> 16) & 0xFF;
        base["g"]     = (c >> 8)  & 0xFF;
        base["b"]     = c & 0xFF;
        base["on"]    = _ring->getOn();
        base["blink"] = _ring->getBlink();

        doc["effect"] = RingController::effectToString(_ring->getEffect());

        JsonObject sp = doc["spinner"].to<JsonObject>();
        sp["r"]     = _ring->getSpinnerR();
        sp["g"]     = _ring->getSpinnerG();
        sp["b"]     = _ring->getSpinnerB();
        sp["tail"]  = _ring->getSpinnerTail();
        sp["speed"] = _ring->getSpinnerSpeed();
        sp["cw"]    = _ring->getSpinnerCW();

        doc["rainbowCycleTime"] = _ring->getRainbowCycleTime();
        doc["partyMadness"]     = _ring->getPartyMadness();

        JsonObject pr = doc["progress"].to<JsonObject>();
        pr["pct"] = _ring->getProgressPercent();
        pr["fr"]  = _ring->getProgressFR();
        pr["fg"]  = _ring->getProgressFG();
        pr["fb"]  = _ring->getProgressFB();
        pr["br"]  = _ring->getProgressBR();
        pr["bg"]  = _ring->getProgressBG();
        pr["bb"]  = _ring->getProgressBB();

        JsonObject cl = doc["clock"].to<JsonObject>();
        cl["hr"] = _ring->getClockColor(CLOCK_HOURS,   0);
        cl["hg"] = _ring->getClockColor(CLOCK_HOURS,   1);
        cl["hb"] = _ring->getClockColor(CLOCK_HOURS,   2);
        cl["mr"] = _ring->getClockColor(CLOCK_MINUTES, 0);
        cl["mg"] = _ring->getClockColor(CLOCK_MINUTES, 1);
        cl["mb"] = _ring->getClockColor(CLOCK_MINUTES, 2);
        cl["sr"] = _ring->getClockColor(CLOCK_SECONDS, 0);
        cl["sg"] = _ring->getClockColor(CLOCK_SECONDS, 1);
        cl["sb"] = _ring->getClockColor(CLOCK_SECONDS, 2);

        JsonObject ch = doc["chase"].to<JsonObject>();
        ch["r"]     = _ring->getChaseR();
        ch["g"]     = _ring->getChaseG();
        ch["b"]     = _ring->getChaseB();
        ch["speed"] = _ring->getChaseSpeed();

        JsonObject geo = doc["geometry"].to<JsonObject>();
        geo["origin"]  = _ring->getOrigin();
        geo["reverse"] = _ring->getReverse();

        doc["makeChangesPersistent"] = _makeChangesPersistent;
        doc["latitude"]              = _latitude;
        doc["longitude"]             = _longitude;
        doc["bambuMode"]             = _bambuMode;
        doc["idleTimeoutMin"]        = _idleTimeoutMin;

        File file = LittleFS.open(CONFIG_FILE, "w");
        if (!file) return;
        serializeJsonPretty(doc, file);
        file.close();
        Serial.println("[Config] config.json saved");
    }

private:
    RingController  *_ring                  = nullptr;
    unsigned long    _dirtyTime             = 0;
    bool             _makeChangesPersistent = true;
    float            _latitude              = 0.0f;
    float            _longitude             = 0.0f;
    bool             _bambuMode             = false;
    uint8_t          _idleTimeoutMin        = 5;

    void load()
    {
        if (!LittleFS.exists(CONFIG_FILE)) return;
        File file = LittleFS.open(CONFIG_FILE, "r");
        if (!file) return;
        JsonDocument doc;
        if (deserializeJson(doc, file)) { file.close(); return; }
        file.close();

        // Geometry first: every effect draws through it.
        _ring->setGeometry(doc["geometry"]["origin"] | (uint8_t)0,
                           doc["geometry"]["reverse"] | false);

        _ring->setSolid(doc["base"]["r"]     | (uint8_t)0,
                        doc["base"]["g"]     | (uint8_t)0,
                        doc["base"]["b"]     | (uint8_t)0,
                        doc["base"]["on"]    | false,
                        doc["base"]["blink"] | false);

        // Parameters are restored with the effect switched off, then the saved
        // effect is enabled once — so loading never starts two effects at a time.
        _ring->setSpinner(false,
                          doc["spinner"]["r"]     | (uint8_t)0,
                          doc["spinner"]["g"]     | (uint8_t)120,
                          doc["spinner"]["b"]     | (uint8_t)255,
                          doc["spinner"]["tail"]  | (uint8_t)4,
                          doc["spinner"]["speed"] | (uint16_t)60,
                          doc["spinner"]["cw"]    | true);
        _ring->setRainbow(false, doc["rainbowCycleTime"] | 5.0f);
        _ring->setParty(false,   doc["partyMadness"]     | 5);
        _ring->setProgress(false,
                           doc["progress"]["pct"] | (uint8_t)0,
                           doc["progress"]["fr"]  | (uint8_t)0,
                           doc["progress"]["fg"]  | (uint8_t)170,
                           doc["progress"]["fb"]  | (uint8_t)255,
                           doc["progress"]["br"]  | (uint8_t)0,
                           doc["progress"]["bg"]  | (uint8_t)0,
                           doc["progress"]["bb"]  | (uint8_t)0);
        _ring->setClockColor(CLOCK_HOURS,
                             doc["clock"]["hr"] | (uint8_t)200,
                             doc["clock"]["hg"] | (uint8_t)40,
                             doc["clock"]["hb"] | (uint8_t)0);
        _ring->setClockColor(CLOCK_MINUTES,
                             doc["clock"]["mr"] | (uint8_t)0,
                             doc["clock"]["mg"] | (uint8_t)180,
                             doc["clock"]["mb"] | (uint8_t)0);
        _ring->setClockColor(CLOCK_SECONDS,
                             doc["clock"]["sr"] | (uint8_t)0,
                             doc["clock"]["sg"] | (uint8_t)0,
                             doc["clock"]["sb"] | (uint8_t)60);
        _ring->setChase(false,
                        doc["chase"]["r"]     | (uint8_t)255,
                        doc["chase"]["g"]     | (uint8_t)255,
                        doc["chase"]["b"]     | (uint8_t)255,
                        doc["chase"]["speed"] | (uint16_t)120);

        RingEffect saved = RingController::effectFromString(doc["effect"] | "none");
        if (saved != RingEffect::NONE) _applyEffect(saved);

        _makeChangesPersistent = doc["makeChangesPersistent"] | true;
        _latitude              = doc["latitude"]              | 0.0f;
        _longitude             = doc["longitude"]             | 0.0f;
        _bambuMode             = doc["bambuMode"]             | false;
        _idleTimeoutMin        = doc["idleTimeoutMin"]        | (uint8_t)5;
        Serial.println("[Config] config.json loaded");
    }

    // Re-enables one effect using the parameters already restored above.
    void _applyEffect(RingEffect e)
    {
        switch (e)
        {
            case RingEffect::SPINNER:
                _ring->setSpinner(true, _ring->getSpinnerR(), _ring->getSpinnerG(),
                                  _ring->getSpinnerB(), _ring->getSpinnerTail(),
                                  _ring->getSpinnerSpeed(), _ring->getSpinnerCW());
                break;
            case RingEffect::RAINBOW:
                _ring->setRainbow(true, _ring->getRainbowCycleTime());
                break;
            case RingEffect::PARTY:
                _ring->setParty(true, _ring->getPartyMadness());
                break;
            case RingEffect::PROGRESS:
                _ring->setProgress(true, _ring->getProgressPercent(),
                                   _ring->getProgressFR(), _ring->getProgressFG(), _ring->getProgressFB(),
                                   _ring->getProgressBR(), _ring->getProgressBG(), _ring->getProgressBB());
                break;
            case RingEffect::CLOCK:
                _ring->setClock(true);
                break;
            case RingEffect::CHASE:
                _ring->setChase(true, _ring->getChaseR(), _ring->getChaseG(),
                                _ring->getChaseB(), _ring->getChaseSpeed());
                break;
            default:
                break;
        }
    }
};
