// DSP primitives shared by instruments and effects.
// Rules: no allocation, locking or I/O outside prepare(); everything is real-time safe.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsp {

constexpr int kQuantum = 64;  // control-rate sub-block; automation/LFO/param ramps update at this rate
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline float db2lin(float db) { return db <= -96.f ? 0.f : std::pow(10.f, db * 0.05f); }
inline float lin2db(float l) { return 20.f * std::log10(std::max(l, 1.0e-6f)); }
inline float noteHz(float n) { return 440.f * std::pow(2.f, (n - 69.f) / 12.f); }
inline float flush(float v) { return std::fabs(v) < 1.0e-15f ? 0.f : v; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float fastTanh(float x) {
    if (x < -3.f) return -1.f;
    if (x > 3.f) return 1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}
// Time constant -> one-pole coefficient (reaches ~63% after `ms`).
inline float onePoleCoef(float ms, float sr) { return ms <= 0.f ? 1.f : 1.f - std::exp(-1.f / (ms * 0.001f * sr)); }

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float uni() { return (float)(next() >> 8) * (1.f / 16777216.f); }
    inline float bi() { return uni() * 2.f - 1.f; }
};

// Deterministic hash -> [0,1). Used for step probability so a render is repeatable.
inline float hash01(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h >> 8) * (1.f / 16777216.f);
}

inline float polyBlep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}

enum Wave { kSaw = 0, kSquare, kTriangle, kSine, kNoise, kPulse };

// Band-limited oscillator (PolyBLEP). One instance per sounding oscillator.
struct Osc {
    float phase = 0.f, tri = 0.f;
    inline float tick(int wave, float dt, float pw = 0.5f) {
        float out = 0.f;
        const float ph = phase;
        switch (wave) {
            case kSine: out = std::sin(ph * kTwoPi); break;
            case kSaw: out = 2.f * ph - 1.f - polyBlep(ph, dt); break;
            case kSquare:
            case kPulse: {
                const float w = wave == kSquare ? 0.5f : clampf(pw, 0.05f, 0.95f);
                out = ph < w ? 1.f : -1.f;
                out += polyBlep(ph, dt);
                float p2 = ph + (1.f - w); if (p2 >= 1.f) p2 -= 1.f;
                out -= polyBlep(p2, dt);
                break;
            }
            case kTriangle: {
                float sq = ph < 0.5f ? 1.f : -1.f;
                sq += polyBlep(ph, dt);
                float p2 = ph + 0.5f; if (p2 >= 1.f) p2 -= 1.f;
                sq -= polyBlep(p2, dt);
                tri = dt * 4.f * sq + (1.f - dt * 0.25f) * tri;
                out = tri;
                break;
            }
            default: break;
        }
        phase += dt;
        if (phase >= 1.f) phase -= 1.f;
        return out;
    }
};

// Zero-delay-feedback state-variable filter (Zavalishin). Stable under fast modulation.
struct SVF {
    float g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
    float lp = 0, bp = 0, hp = 0;
    inline void set(float hz, float q, float sr) {
        hz = clampf(hz, 16.f, sr * 0.45f);
        g = std::tan(kPi * hz / sr);
        k = 1.f / std::max(q, 0.1f);
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    inline void tick(float x) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = flush(2.f * v1 - ic1);
        ic2 = flush(2.f * v2 - ic2);
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
    void reset() { ic1 = ic2 = lp = bp = hp = 0.f; }
};

// RBJ biquad, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    enum Type { LP, HP, BP, Peak, LowShelf, HighShelf, Notch, AllPass };
    void set(int type, float hz, float q, float gainDb, float sr) {
        hz = clampf(hz, 10.f, sr * 0.45f);
        q = std::max(q, 0.05f);
        const float w = kTwoPi * hz / sr, c = std::cos(w), s = std::sin(w), al = s / (2.f * q);
        const float A = std::pow(10.f, gainDb / 40.f);
        float a0 = 1;
        switch (type) {
            case LP: b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
            case HP: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
            case BP: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
            case Notch: b0 = 1; b1 = -2 * c; b2 = 1; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
            case AllPass: b0 = 1 - al; b1 = -2 * c; b2 = 1 + al; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
            case Peak: b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A; break;
            case LowShelf: {
                const float r = 2 * std::sqrt(A) * al;
                b0 = A * ((A + 1) - (A - 1) * c + r); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - r);
                a0 = (A + 1) + (A - 1) * c + r; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - r;
                break;
            }
            case HighShelf: {
                const float r = 2 * std::sqrt(A) * al;
                b0 = A * ((A + 1) + (A - 1) * c + r); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - r);
                a0 = (A + 1) - (A - 1) * c + r; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - r;
                break;
            }
            default: break;
        }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
    inline float tick(float x) {
        const float y = b0 * x + z1;
        z1 = flush(b1 * x - a1 * y + z2);
        z2 = flush(b2 * x - a2 * y);
        return y;
    }
    void reset() { z1 = z2 = 0.f; }
};

struct OnePole {
    float a = 1.f, z = 0.f;
    void setHz(float hz, float sr) { a = 1.f - std::exp(-kTwoPi * clampf(hz, 1.f, sr * 0.49f) / sr); }
    inline float lp(float x) { z += a * (x - z); z = flush(z); return z; }
    inline float hp(float x) { return x - lp(x); }
    void reset() { z = 0.f; }
};

struct DelayLine {
    std::vector<float> buf;
    uint32_t mask = 0, w = 0;
    void allocate(int samples) {
        uint32_t n = 16;
        while ((int)n < samples + 4) n <<= 1;
        buf.assign(n, 0.f);
        mask = n - 1;
        w = 0;
    }
    inline void push(float x) { buf[w] = x; w = (w + 1) & mask; }
    inline float tap(int d) const { return buf[(w - 1 - (uint32_t)d) & mask]; }
    // Fractional read (d samples behind the most recent write), linear interpolation.
    inline float read(float d) const {
        d = std::max(d, 0.f);
        const int i = (int)d;
        const float f = d - (float)i;
        const float a = buf[(w - 1 - (uint32_t)i) & mask], b = buf[(w - 2 - (uint32_t)i) & mask];
        return a + (b - a) * f;
    }
    int capacity() const { return (int)buf.size() - 4; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.f); w = 0; }
};

struct ADSR {
    enum Stage { Idle, Attack, Decay, Sustain, Release } stage = Idle;
    float v = 0, aInc = 1, dCoef = 1, s = 1, rCoef = 1;
    void set(float attackS, float decayS, float sus, float releaseS, float sr) {
        aInc = attackS <= 0.0005f ? 1.f : 1.f / (attackS * sr);
        dCoef = decayS <= 0.0005f ? 1.f : 1.f - std::exp(-4.6f / (decayS * sr));
        s = clampf(sus, 0.f, 1.f);
        rCoef = releaseS <= 0.0005f ? 1.f : 1.f - std::exp(-4.6f / (releaseS * sr));
    }
    void noteOn(bool retriggerFromZero = false) { if (retriggerFromZero) v = 0.f; stage = Attack; }
    void noteOff() { if (stage != Idle) stage = Release; }
    void kill() { stage = Idle; v = 0.f; }
    bool active() const { return stage != Idle; }
    inline float tick() {
        switch (stage) {
            case Idle: return 0.f;
            case Attack: v += aInc; if (v >= 1.f) { v = 1.f; stage = Decay; } break;
            case Decay: v += (s - v) * dCoef; if (std::fabs(v - s) < 1.0e-4f) { v = s; stage = Sustain; } break;
            case Sustain: v = s; if (s <= 0.0001f) { stage = Idle; v = 0.f; } break;
            case Release: v -= v * rCoef; if (v < 1.0e-4f) { v = 0.f; stage = Idle; } break;
        }
        return v;
    }
};

// Smooths a parameter towards a target across samples (used per-sample in mixers).
struct Smoothed {
    float cur = 0.f, target = 0.f, coef = 0.002f;
    bool init = false;
    void prepare(float sr, float ms = 20.f) { coef = onePoleCoef(ms, sr); }
    void setTarget(float t) { target = t; if (!init) { cur = t; init = true; } }
    void snap(float t) { target = cur = t; init = true; }
    inline float next() { cur += (target - cur) * coef; return cur; }
};

struct EnvFollower {
    float env = 0.f, att = 0.1f, rel = 0.001f;
    void set(float attackMs, float releaseMs, float sr) { att = onePoleCoef(attackMs, sr); rel = onePoleCoef(releaseMs, sr); }
    inline float tick(float x) {
        x = std::fabs(x);
        env += (x > env ? att : rel) * (x - env);
        env = flush(env);
        return env;
    }
};

// Shared LFO with tempo sync support.
struct LFO {
    float phase = 0.f;
    float hold = 0.f;
    Rng rng { 12345u };
    inline float advance(float hz, float sr, int n, int shape) {
        const float prev = phase;
        phase += hz * (float)n / sr;
        if (phase >= 1.f) { phase -= std::floor(phase); hold = rng.bi(); }
        (void)prev;
        return value(shape);
    }
    inline float value(int shape) const {
        switch (shape) {
            case 0: return std::sin(phase * kTwoPi);
            case 1: return 1.f - 4.f * std::fabs(phase - 0.5f);
            case 2: return 2.f * phase - 1.f;
            case 3: return phase < 0.5f ? 1.f : -1.f;
            default: return hold;
        }
    }
};

// Tempo-synced division table, shared by delay/LFO/arpeggiator UI and DSP.
inline const char* const kSyncNames = "1/1|1/2|1/2D|1/4|1/4D|1/4T|1/8|1/8D|1/8T|1/16|1/16D|1/16T|1/32";
inline float syncBeats(int idx) {
    static const float t[] = { 4.f, 2.f, 3.f, 1.f, 1.5f, 2.f / 3.f, 0.5f, 0.75f, 1.f / 3.f, 0.25f, 0.375f, 1.f / 6.f, 0.125f };
    return t[std::max(0, std::min(idx, 12))];
}

}  // namespace dsp
