#pragma once
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <time.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ─── RingController ──────────────────────────────────────────────────────────
// Drives a 12-pixel WS2812B ring on GPIO 4.
//
// Pixel 0 is whatever pixel the data line reaches first. Because a ring can be
// mounted at any rotation, effects that care about position (progress, clock,
// wipe) address a LOGICAL index that _px() maps onto the physical strip using
// the configurable origin and direction — so the ring can be rotated in
// software instead of being rewired.
//
// Everything is non-blocking: update() advances whichever animation is active.
// Priority, highest first:
//   restore -> overlay -> wipe -> morse -> random yes/no -> effect -> base
// Only one continuous effect runs at a time; selecting one cancels the others.
//
// update() runs on its own FreeRTOS task at a fixed frame rate, above the
// Arduino loop task, so frames keep their timing while loop() is busy - most
// visibly while PubSubClient drags a multi-kilobyte BambuLab report through
// mbedTLS one byte at a time, which can stall loop() for over 100 ms.
//
// That makes the ring state shared between tasks: the render task reads it
// while WebSocket handlers (AsyncTCP task) and loop() write it, so every public
// method takes a recursive mutex. Callbacks never fire while the lock is held -
// the ticks only raise a flag and dispatchEvents() invokes them from loop(),
// keeping all WebSocket and JSON work off the frame timer.

#define LED_DATA_PIN 4
#define LED_COUNT    12

#define RING_FRAME_MS           10   // render task period (~100 fps)
#define BLINK_INTERVAL_MS      500
#define RANDOM_YN_SPIN_MS     5000
#define RANDOM_YN_RESULT_MS   5000

// Clock hand indices, used by get/setClockColor().
#define CLOCK_HOURS   0
#define CLOCK_MINUTES 1
#define CLOCK_SECONDS 2

enum class RingEffect : uint8_t {
    NONE = 0,
    SPINNER,
    RAINBOW,
    PARTY,
    PROGRESS,
    CLOCK,
    CHASE
};

class RingController
{
public:
    Adafruit_NeoPixel strip = Adafruit_NeoPixel(LED_COUNT, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

    // Fired when the random yes/no animation reveals its answer. `pick` is the
    // colour the user bet on — 1 green (yes), 0 red (no), -1 when nobody bet —
    // so the caller can work out whether the bet was won.
    std::function<void(bool yes, int pick)> onRandomResult;
    // Fired when a one-shot animation (wipe, morse, random yes/no) ends.
    std::function<void()> onAnimationEnd;

    void begin()
    {
        _lock = xSemaphoreCreateRecursiveMutex();
        strip.begin();
        strip.clear();
        strip.show();
    }

    // Starts the frame timer. Priority 2 sits above the Arduino loop task, so a
    // busy loop() can no longer make the animations stutter.
    void startRenderTask(UBaseType_t priority = 2, uint32_t stackBytes = 3072)
    {
        if (_renderTask) return;
        xTaskCreate(_renderTaskFn, "ring_fx", stackBytes, this, priority, &_renderTask);
    }

    // Fires the callbacks raised by the render task. Called from loop().
    void dispatchEvents()
    {
        if (_evRandomResult) {
            _evRandomResult = false;
            if (onRandomResult) onRandomResult(_evYes, _evPick);
        }
        if (_evAnimationEnd) {
            _evAnimationEnd = false;
            if (onAnimationEnd) onAnimationEnd();
        }
    }

    // ── Geometry ──────────────────────────────────────────────────────────────

    void setGeometry(uint8_t origin, bool reverse)
    {
        Lock _guard(this);
        _origin  = origin % LED_COUNT;
        _reverse = reverse;
    }
    uint8_t getOrigin()  { return _origin;  }
    bool    getReverse() { return _reverse; }

    // ── Base colour ───────────────────────────────────────────────────────────

    void setSolid(uint8_t r, uint8_t g, uint8_t b, bool on, bool blink)
    {
        Lock _guard(this);
        _r = r; _g = g; _b = b;
        _on = on; _blink = blink;
        if (_effect == RingEffect::NONE && !_busy())
        {
            if (!on)            _fillAll(0, 0, 0);
            else if (!blink)    _fillAll(_r, _g, _b);
            // on && blink: update() drives the blinking
        }
    }

    uint32_t getColor() { return strip.Color(_r, _g, _b); }
    bool     getOn()    { return _on;    }
    bool     getBlink() { return _blink; }

    void restoreAll()
    {
        Lock _guard(this);
        if (_on) _fillAll(_r, _g, _b);
        else     _fillAll(0, 0, 0);
    }

    // ── Effect selection ──────────────────────────────────────────────────────

    RingEffect getEffect() { return _effect; }

    static const char* effectToString(RingEffect e)
    {
        switch (e) {
            case RingEffect::SPINNER:  return "spinner";
            case RingEffect::RAINBOW:  return "rainbow";
            case RingEffect::PARTY:    return "party";
            case RingEffect::PROGRESS: return "progress";
            case RingEffect::CLOCK:    return "clock";
            case RingEffect::CHASE:    return "chase";
            default:                   return "none";
        }
    }

    static RingEffect effectFromString(const char* s)
    {
        if (!s) return RingEffect::NONE;
        if (strcmp(s, "spinner")  == 0) return RingEffect::SPINNER;
        if (strcmp(s, "rainbow")  == 0) return RingEffect::RAINBOW;
        if (strcmp(s, "party")    == 0) return RingEffect::PARTY;
        if (strcmp(s, "progress") == 0) return RingEffect::PROGRESS;
        if (strcmp(s, "clock")    == 0) return RingEffect::CLOCK;
        if (strcmp(s, "chase")    == 0) return RingEffect::CHASE;
        return RingEffect::NONE;
    }

    // ── Spinner / comet ───────────────────────────────────────────────────────

    void setSpinner(bool on, uint8_t r, uint8_t g, uint8_t b,
                    uint8_t tail, uint16_t speedMs, bool clockwise)
    {
        Lock _guard(this);
        _spinR = r; _spinG = g; _spinB = b;
        if (tail >= LED_COUNT) tail = LED_COUNT - 1;
        _spinTail  = tail;
        _spinSpeed = speedMs < 10 ? 10 : speedMs;
        _spinCW    = clockwise;
        _select(on ? RingEffect::SPINNER : RingEffect::NONE, RingEffect::SPINNER);
        if (on) { _spinPos = 0; _spinLast = 0; }
    }

    uint8_t  getSpinnerR()     { return _spinR;    }
    uint8_t  getSpinnerG()     { return _spinG;    }
    uint8_t  getSpinnerB()     { return _spinB;    }
    uint8_t  getSpinnerTail()  { return _spinTail; }
    uint16_t getSpinnerSpeed() { return _spinSpeed;}
    bool     getSpinnerCW()    { return _spinCW;   }

    // ── Rainbow ───────────────────────────────────────────────────────────────

    void setRainbow(bool on, float cycleTime)
    {
        Lock _guard(this);
        _rainbowCycle = cycleTime <= 0.1f ? 0.1f : cycleTime;
        _select(on ? RingEffect::RAINBOW : RingEffect::NONE, RingEffect::RAINBOW);
    }
    float getRainbowCycleTime() { return _rainbowCycle; }

    // ── Party ─────────────────────────────────────────────────────────────────

    void setParty(bool on, int madness)
    {
        Lock _guard(this);
        if (madness >= 1 && madness <= 10) _partyMadness = madness;
        _select(on ? RingEffect::PARTY : RingEffect::NONE, RingEffect::PARTY);
        if (on) _partyStart();
    }
    int getPartyMadness() { return _partyMadness; }

    // ── Progress ring ─────────────────────────────────────────────────────────

    void setProgress(bool on, uint8_t percent,
                     uint8_t fr, uint8_t fg, uint8_t fb,
                     uint8_t br, uint8_t bg, uint8_t bb)
    {
        Lock _guard(this);
        _progPct = percent > 100 ? 100 : percent;
        _progFR = fr; _progFG = fg; _progFB = fb;
        _progBR = br; _progBG = bg; _progBB = bb;
        _select(on ? RingEffect::PROGRESS : RingEffect::NONE, RingEffect::PROGRESS);
        if (on) _progLast = 0;   // force an immediate redraw
    }

    // Updates the percentage without touching the on/off state — used by the
    // BambuLab print progress feed.
    void setProgressPercent(uint8_t percent)
    {
        Lock _guard(this);
        _progPct  = percent > 100 ? 100 : percent;
        _progLast = 0;
    }

    uint8_t getProgressPercent() { return _progPct; }
    uint8_t getProgressFR() { return _progFR; }
    uint8_t getProgressFG() { return _progFG; }
    uint8_t getProgressFB() { return _progFB; }
    uint8_t getProgressBR() { return _progBR; }
    uint8_t getProgressBG() { return _progBG; }
    uint8_t getProgressBB() { return _progBB; }

    // ── Clock ─────────────────────────────────────────────────────────────────

    // Colours are kept separately from the on/off switch so every existing
    // caller can keep flipping the effect without restating them.
    void setClock(bool on)
    {
        Lock _guard(this);
        _select(on ? RingEffect::CLOCK : RingEffect::NONE, RingEffect::CLOCK);
        if (on) _clockLast = 0;
    }

    // hand: CLOCK_HOURS, CLOCK_MINUTES or CLOCK_SECONDS.
    void setClockColor(int hand, uint8_t r, uint8_t g, uint8_t b)
    {
        Lock _guard(this);
        if (hand < 0 || hand > 2) return;
        _clockColors[hand][0] = r;
        _clockColors[hand][1] = g;
        _clockColors[hand][2] = b;
        _clockLast = 0;   // redraw on the next tick
    }

    uint8_t getClockColor(int hand, int channel)
    {
        if (hand < 0 || hand > 2 || channel < 0 || channel > 2) return 0;
        return _clockColors[hand][channel];
    }

    // ── Theater chase ─────────────────────────────────────────────────────────

    void setChase(bool on, uint8_t r, uint8_t g, uint8_t b, uint16_t speedMs)
    {
        Lock _guard(this);
        _chaseR = r; _chaseG = g; _chaseB = b;
        _chaseSpeed = speedMs < 20 ? 20 : speedMs;
        _select(on ? RingEffect::CHASE : RingEffect::NONE, RingEffect::CHASE);
        if (on) { _chaseStep = 0; _chaseLast = 0; }
    }

    uint8_t  getChaseR()     { return _chaseR;     }
    uint8_t  getChaseG()     { return _chaseG;     }
    uint8_t  getChaseB()     { return _chaseB;     }
    uint16_t getChaseSpeed() { return _chaseSpeed; }

    // ── Colour wipe (one-shot) ────────────────────────────────────────────────

    void startWipe(uint8_t r, uint8_t g, uint8_t b, uint16_t speedMs)
    {
        Lock _guard(this);
        _cancelAll();
        _wipeR = r; _wipeG = g; _wipeB = b;
        _wipeSpeed   = speedMs < 20 ? 20 : speedMs;
        _wipeIdx     = 0;
        _wipeNext    = millis();
        _wipeHoldEnd = 0;
        _wipeRunning = true;
        strip.clear();
        strip.show();
    }
    bool isWipeRunning() { return _wipeRunning; }

    // ── Morse (one-shot) ──────────────────────────────────────────────────────

    void startMorse(const char* text)
    {
        Lock _guard(this);
        _cancelAll();
        _morseTotal = 0;
        _morseIdx   = 0;

        static const uint16_t DOT_MS  = 200;
        static const uint16_t DASH_MS = 600;
        static const uint16_t ELEM_MS = 200;
        static const uint16_t CHAR_MS = 600;
        static const uint16_t WORD_MS = 1400;

        for (int i = 0; text[i] != '\0' && _morseTotal < MAX_MORSE_EVENTS - 10; i++)
        {
            char c = toupper((unsigned char)text[i]);
            if (c == ' ')
            {
                if (_morseTotal > 0 && !_morseSeq[_morseTotal - 1].on)
                    _morseSeq[_morseTotal - 1].ms = WORD_MS;
                else
                    _morseSeq[_morseTotal++] = {false, WORD_MS};
                continue;
            }
            if (c < 'A' || c > 'Z') continue;
            const char* sym = _morseChar(c - 'A');
            if (!sym) continue;
            int len = strlen(sym);
            for (int j = 0; j < len; j++)
            {
                _morseSeq[_morseTotal++] = {true, sym[j] == '-' ? DASH_MS : DOT_MS};
                if (j < len - 1) _morseSeq[_morseTotal++] = {false, ELEM_MS};
            }
            _morseSeq[_morseTotal++] = {false, CHAR_MS};
        }

        _morseRunning = (_morseTotal > 0);
        _morseNext    = millis();
        strip.clear();
        strip.show();
    }
    bool isMorseRunning() { return _morseRunning; }

    // ── Random yes/no (one-shot) ──────────────────────────────────────────────

    // pick: 1 = bet on green (yes), 0 = bet on red (no), anything else = no bet.
    void startRandomYesNo(int pick = -1)
    {
        Lock _guard(this);
        _cancelAll();
        _ynPick   = (pick == 0 || pick == 1) ? pick : -1;
        _ynState  = YNState::SPIN;
        _ynStart  = millis();
        _ynLast   = millis();
        _ynPos    = 0;
        strip.clear();
        strip.show();
    }
    bool isRandomYNRunning() { return _ynState != YNState::IDLE; }
    int  getRandomPick()     { return _ynPick; }

    // ── Overlay (weather, air quality, BambuLab state) ────────────────────────

    // rgb[i] is the LOGICAL pixel i, so callers think in ring positions and not
    // in wiring order.
    void showPixels(const uint8_t rgb[LED_COUNT][3], unsigned long durationMs = 5000)
    {
        Lock _guard(this);
        _cancelAll();
        _restoreNeeded = false;
        _overlayRunning = true;
        _overlayUntil   = millis() + durationMs;
        for (int i = 0; i < LED_COUNT; i++)
            strip.setPixelColor(_px(i), strip.Color(rgb[i][0], rgb[i][1], rgb[i][2]));
        strip.show();
    }

    void showColor(uint8_t r, uint8_t g, uint8_t b, unsigned long durationMs = 5000)
    {
        Lock _guard(this);
        _cancelAll();
        _restoreNeeded  = false;
        _overlayRunning = true;
        _overlayUntil   = millis() + durationMs;
        _fillAll(r, g, b);
    }

    void cancelOverlay()
    {
        Lock _guard(this);
        _overlayRunning = false;
        _restoreNeeded  = true;
    }

    bool isOverlayRunning() { return _overlayRunning; }

    // ── Update ────────────────────────────────────────────────────────────────

    void update()
    {
        Lock _guard(this);
        if (_restoreNeeded)
        {
            _restoreNeeded = false;
            restoreAll();
            return;
        }
        if (_overlayRunning)
        {
            if (millis() >= _overlayUntil)
            {
                _overlayRunning = false;
                _restoreNeeded  = true;
            }
            return;
        }
        if (_wipeRunning)  { _wipeTick();  return; }
        if (_morseRunning) { _morseTick(); return; }
        if (_ynState != YNState::IDLE) { _ynTick(); return; }

        switch (_effect)
        {
            case RingEffect::SPINNER:  _spinnerTick();  return;
            case RingEffect::RAINBOW:  _rainbowTick();  return;
            case RingEffect::PARTY:    _partyTick();    return;
            case RingEffect::PROGRESS: _progressTick(); return;
            case RingEffect::CLOCK:    _clockTick();    return;
            case RingEffect::CHASE:    _chaseTick();    return;
            default: break;
        }

        // Base state: steady colour needs no work, blinking is driven here.
        if (!_blink || !_on) return;
        if (millis() - _blinkLast < BLINK_INTERVAL_MS) return;
        _blinkLast  = millis();
        _blinkPhase = !_blinkPhase;
        if (_blinkPhase) _fillAll(_r, _g, _b);
        else             _fillAll(0, 0, 0);
    }

private:
    // ── Task plumbing ─────────────────────────────────────────────────────────
    SemaphoreHandle_t _lock       = nullptr;
    TaskHandle_t      _renderTask = nullptr;

    // Raised by the render task, consumed by dispatchEvents() on loop().
    volatile bool _evRandomResult = false;
    volatile bool _evAnimationEnd = false;
    bool          _evYes  = false;
    int           _evPick = -1;

    static void _renderTaskFn(void* arg)
    {
        RingController* self = static_cast<RingController*>(arg);
        TickType_t last = xTaskGetTickCount();
        for (;;) {
            self->update();
            vTaskDelayUntil(&last, pdMS_TO_TICKS(RING_FRAME_MS));
        }
    }

    void _take() { if (_lock) xSemaphoreTakeRecursive(_lock, portMAX_DELAY); }
    void _give() { if (_lock) xSemaphoreGiveRecursive(_lock); }

    // Recursive, so a locked method calling another one is fine.
    struct Lock {
        RingController* c;
        explicit Lock(RingController* ctrl) : c(ctrl) { c->_take(); }
        ~Lock() { c->_give(); }
    };

    // ── Geometry ──────────────────────────────────────────────────────────────
    uint8_t _origin  = 0;
    bool    _reverse = false;

    // Logical ring position -> physical pixel index.
    int _px(int logical) const
    {
        int i = logical % LED_COUNT;
        if (i < 0) i += LED_COUNT;
        if (_reverse) i = (LED_COUNT - i) % LED_COUNT;
        return (i + _origin) % LED_COUNT;
    }

    void _fillAll(uint8_t r, uint8_t g, uint8_t b)
    {
        uint32_t c = strip.Color(r, g, b);
        for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, c);
        strip.show();
    }

    // ── Base state ────────────────────────────────────────────────────────────
    uint8_t _r = 0, _g = 0, _b = 0;
    bool    _on = false, _blink = false;
    bool          _blinkPhase = false;
    unsigned long _blinkLast  = 0;

    bool          _restoreNeeded  = false;
    bool          _overlayRunning = false;
    unsigned long _overlayUntil   = 0;

    RingEffect _effect = RingEffect::NONE;

    // True while a one-shot animation owns the ring.
    bool _busy() const
    {
        return _overlayRunning || _wipeRunning || _morseRunning || _ynState != YNState::IDLE;
    }

    // Turns `want` on, or off when the caller passed false. `owner` is the
    // effect the caller is switching, so releasing an effect that is not
    // currently running never disturbs another one.
    void _select(RingEffect want, RingEffect owner)
    {
        if (want == RingEffect::NONE)
        {
            if (_effect != owner) return;   // somebody else owns the ring now
            _effect = RingEffect::NONE;
            _restoreNeeded = true;
            return;
        }
        // Starting an effect also drops a running overlay and any queued
        // restore, otherwise the overlay would keep the ring until it expires.
        _cancelOneShots();
        _overlayRunning = false;
        _restoreNeeded  = false;
        _effect = want;
    }

    void _cancelOneShots()
    {
        _wipeRunning  = false;
        _morseRunning = false;
        _ynState      = YNState::IDLE;
    }

    void _cancelAll()
    {
        _cancelOneShots();
        _effect         = RingEffect::NONE;
        _overlayRunning = false;
    }

    // ── Spinner ───────────────────────────────────────────────────────────────
    uint8_t  _spinR = 0, _spinG = 120, _spinB = 255;
    uint8_t  _spinTail  = 4;
    uint16_t _spinSpeed = 60;
    bool     _spinCW    = true;
    int      _spinPos   = 0;
    unsigned long _spinLast = 0;

    void _spinnerTick()
    {
        if (millis() - _spinLast < _spinSpeed) return;
        _spinLast = millis();
        strip.clear();
        for (int t = 0; t <= _spinTail; t++)
        {
            // Brightness falls off linearly along the tail.
            float  f   = 1.0f - (float)t / (float)(_spinTail + 1);
            int    pos = _spinCW ? _spinPos - t : _spinPos + t;
            strip.setPixelColor(_px(pos), strip.Color((uint8_t)(_spinR * f),
                                                      (uint8_t)(_spinG * f),
                                                      (uint8_t)(_spinB * f)));
        }
        strip.show();
        _spinPos = (_spinPos + 1) % LED_COUNT;
    }

    // ── Rainbow ───────────────────────────────────────────────────────────────
    float _rainbowCycle = 5.0f;

    void _rainbowTick()
    {
        unsigned long cycleMs = (unsigned long)(_rainbowCycle * 1000.0f);
        if (cycleMs == 0) cycleMs = 1;
        uint16_t base = (uint16_t)((millis() % cycleMs) * 65536UL / cycleMs);
        for (int i = 0; i < LED_COUNT; i++)
            strip.setPixelColor(_px(i),
                strip.gamma32(strip.ColorHSV(base + (uint16_t)(i * (65536 / LED_COUNT)), 255, 255)));
        strip.show();
    }

    // ── Party ─────────────────────────────────────────────────────────────────
    int           _partyMadness = 5;
    bool          _partyOn[LED_COUNT]   = {false};
    unsigned long _partyNext[LED_COUNT] = {0};

    void _partyInterval(unsigned long& minMs, unsigned long& maxMs)
    {
        maxMs = (unsigned long)map(_partyMadness, 1, 10, 2000, 20);
        minMs = max(5UL, maxMs / 4);
    }

    void _partyStart()
    {
        unsigned long lo, hi;
        _partyInterval(lo, hi);
        for (int i = 0; i < LED_COUNT; i++)
        {
            _partyOn[i]   = false;
            _partyNext[i] = millis() + random(lo, hi + 1);
            strip.setPixelColor(i, 0);
        }
        strip.show();
    }

    void _partyTick()
    {
        bool changed = false;
        unsigned long lo, hi;
        _partyInterval(lo, hi);
        for (int i = 0; i < LED_COUNT; i++)
        {
            if (millis() < _partyNext[i]) continue;
            _partyOn[i] = !_partyOn[i];
            strip.setPixelColor(i, _partyOn[i]
                ? strip.gamma32(strip.ColorHSV(random(65536), 255, 255))
                : 0);
            _partyNext[i] = millis() + random(lo, hi + 1);
            changed = true;
        }
        if (changed) strip.show();
    }

    // ── Progress ──────────────────────────────────────────────────────────────
    uint8_t _progPct = 0;
    uint8_t _progFR = 0,  _progFG = 170, _progFB = 255;
    uint8_t _progBR = 0,  _progBG = 0,   _progBB = 0;
    unsigned long _progLast = 0;

    void _progressTick()
    {
        // Nothing moves on its own; redraw a few times a second so a percentage
        // pushed from MQTT or the printer shows up promptly.
        if (_progLast != 0 && millis() - _progLast < 200) return;
        _progLast = millis();

        float exact = (float)_progPct * LED_COUNT / 100.0f;
        int   full  = (int)exact;
        float frac  = exact - full;
        for (int i = 0; i < LED_COUNT; i++)
        {
            uint8_t r, g, b;
            if (i < full)        { r = _progFR; g = _progFG; b = _progFB; }
            else if (i == full)  { // partially filled pixel, dimmed by the remainder
                r = (uint8_t)(_progBR + (_progFR - _progBR) * frac);
                g = (uint8_t)(_progBG + (_progFG - _progBG) * frac);
                b = (uint8_t)(_progBB + (_progFB - _progBB) * frac);
            }
            else                 { r = _progBR; g = _progBG; b = _progBB; }
            strip.setPixelColor(_px(i), strip.Color(r, g, b));
        }
        strip.show();
    }

    // ── Clock ─────────────────────────────────────────────────────────────────
    unsigned long _clockLast = 0;

    // [hand][rgb] — hours amber, minutes green, seconds dim blue by default.
    uint8_t _clockColors[3][3] = {
        { 200,  40,   0 },
        {   0, 180,   0 },
        {   0,   0,  60 }
    };

    void _clockTick()
    {
        if (_clockLast != 0 && millis() - _clockLast < 200) return;
        _clockLast = millis();

        struct tm t;
        if (!getLocalTime(&t, 0)) return;

        int hp = t.tm_hour % 12;
        int mp = (t.tm_min * LED_COUNT) / 60;
        int sp = (t.tm_sec * LED_COUNT) / 60;

        // Hands are added together so an overlap reads as a mixed colour
        // instead of one hand hiding another. Seconds go down first so the
        // hour hand stays dominant where they land on the same pixel.
        uint8_t buf[LED_COUNT][3] = {{0}};
        _addHand(buf, sp, _clockColors[CLOCK_SECONDS][0], _clockColors[CLOCK_SECONDS][1], _clockColors[CLOCK_SECONDS][2]);
        _addHand(buf, mp, _clockColors[CLOCK_MINUTES][0], _clockColors[CLOCK_MINUTES][1], _clockColors[CLOCK_MINUTES][2]);
        _addHand(buf, hp, _clockColors[CLOCK_HOURS][0],   _clockColors[CLOCK_HOURS][1],   _clockColors[CLOCK_HOURS][2]);
        for (int i = 0; i < LED_COUNT; i++)
            strip.setPixelColor(_px(i), strip.Color(buf[i][0], buf[i][1], buf[i][2]));
        strip.show();
    }

    static void _addHand(uint8_t buf[LED_COUNT][3], int pos, int r, int g, int b)
    {
        if (pos < 0 || pos >= LED_COUNT) return;
        buf[pos][0] = (uint8_t)min(255, buf[pos][0] + r);
        buf[pos][1] = (uint8_t)min(255, buf[pos][1] + g);
        buf[pos][2] = (uint8_t)min(255, buf[pos][2] + b);
    }

    // ── Theater chase ─────────────────────────────────────────────────────────
    uint8_t  _chaseR = 255, _chaseG = 255, _chaseB = 255;
    uint16_t _chaseSpeed = 120;
    int      _chaseStep  = 0;
    unsigned long _chaseLast = 0;

    void _chaseTick()
    {
        if (millis() - _chaseLast < _chaseSpeed) return;
        _chaseLast = millis();
        strip.clear();
        for (int i = _chaseStep % 3; i < LED_COUNT; i += 3)
            strip.setPixelColor(_px(i), strip.Color(_chaseR, _chaseG, _chaseB));
        strip.show();
        _chaseStep++;
    }

    // ── Wipe ──────────────────────────────────────────────────────────────────
    bool     _wipeRunning = false;
    uint8_t  _wipeR = 0, _wipeG = 0, _wipeB = 0;
    uint16_t _wipeSpeed = 60;
    int      _wipeIdx   = 0;
    unsigned long _wipeNext    = 0;
    unsigned long _wipeHoldEnd = 0;

    void _wipeTick()
    {
        if (_wipeHoldEnd > 0)
        {
            if (millis() < _wipeHoldEnd) return;
            _wipeRunning = false;
            _wipeHoldEnd = 0;
            _restoreNeeded = true;
            _evAnimationEnd = true;
            return;
        }
        if (millis() < _wipeNext) return;
        strip.setPixelColor(_px(_wipeIdx), strip.Color(_wipeR, _wipeG, _wipeB));
        strip.show();
        _wipeIdx++;
        _wipeNext = millis() + _wipeSpeed;
        if (_wipeIdx >= LED_COUNT) _wipeHoldEnd = millis() + 1000;   // hold, then restore
    }

    // ── Morse ─────────────────────────────────────────────────────────────────
    struct MorseEvent { bool on; uint16_t ms; };
    static const int MAX_MORSE_EVENTS = 512;
    MorseEvent    _morseSeq[MAX_MORSE_EVENTS];
    int           _morseTotal   = 0;
    int           _morseIdx     = 0;
    unsigned long _morseNext    = 0;
    bool          _morseRunning = false;

    static const char* _morseChar(int idx)
    {
        static const char* tbl[26] = {
            //  A      B       C       D      E     F       G      H
                ".-",  "-...", "-.-.", "-..", ".",  "..-.", "--.", "....",
            //  I      J       K       L      M     N       O      P
                "..",  ".---", "-.-",  ".-..","--", "-.",   "---", ".--.",
            //  Q      R       S       T      U     V       W      X
                "--.-",".-.",  "...",  "-",   "..-","...-", ".--", "-..-",
            //  Y      Z
                "-.--","--.."
        };
        if (idx < 0 || idx >= 26) return nullptr;
        return tbl[idx];
    }

    void _morseTick()
    {
        if (millis() < _morseNext) return;
        if (_morseIdx >= _morseTotal)
        {
            _morseRunning  = false;
            _restoreNeeded = true;
            _evAnimationEnd = true;
            return;
        }
        MorseEvent ev = _morseSeq[_morseIdx++];
        _fillAll(ev.on ? 255 : 0, ev.on ? 255 : 0, ev.on ? 255 : 0);
        _morseNext = millis() + ev.ms;
    }

    // ── Random yes/no ─────────────────────────────────────────────────────────
    enum class YNState : uint8_t { IDLE, SPIN, RESULT };
    YNState       _ynState = YNState::IDLE;
    unsigned long _ynStart = 0;
    unsigned long _ynLast  = 0;
    int           _ynPos   = 0;
    bool          _ynYes   = false;
    int           _ynPick  = -1;

    void _ynTick()
    {
        if (_ynState == YNState::SPIN)
        {
            unsigned long elapsed = millis() - _ynStart;
            if (elapsed >= RANDOM_YN_SPIN_MS)
            {
                _ynState = YNState::RESULT;
                _ynStart = millis();
                _ynYes   = random(2);
                if (_ynYes) _fillAll(0, 255, 0);
                else        _fillAll(255, 0, 0);
                _evYes = _ynYes; _evPick = _ynPick; _evRandomResult = true;
                return;
            }
            // The spin decelerates from 30 ms to 320 ms per step over 5 s.
            unsigned long interval = map((long)elapsed, 0, RANDOM_YN_SPIN_MS, 30, 320);
            if (millis() - _ynLast < interval) return;
            _ynLast = millis();
            strip.clear();
            strip.setPixelColor(_px(_ynPos), strip.Color(255, 200, 0));
            strip.show();
            _ynPos = (_ynPos + 1) % LED_COUNT;
            return;
        }

        // RESULT
        if (millis() - _ynStart < RANDOM_YN_RESULT_MS) return;
        _ynState       = YNState::IDLE;
        _restoreNeeded = true;
        _evAnimationEnd = true;
    }
};
