// Parameter metadata + lock-free storage shared by UI (writes) and audio thread (reads).
// UI controls are generated from ParamInfo tables, so every visible knob drives real DSP.
#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <cmath>
#include <memory>

enum class ParamScale { Linear, Log, Int, Bool, Choice };

struct ParamInfo {
    const char* id;
    const char* name;
    float min, max, def;
    const char* unit;      // "", "Hz", "ms", "dB", "%", "st", "ct", "s", "x"
    ParamScale scale;
    const char* options;   // "A|B|C" for Choice
    const char* group;     // UI section
};

class ParamSet {
public:
    ParamSet(const ParamInfo* infos, int count) : info(infos), n(count),
        base(new std::atomic<float>[(size_t)count]), over(new std::atomic<float>[(size_t)count]) {
        for (int i = 0; i < n; ++i) { base[i].store(info[i].def); over[i].store(NAN); }
    }
    int size() const { return n; }
    const ParamInfo& meta(int i) const { return info[i]; }
    int indexOf(const juce::String& id) const {
        for (int i = 0; i < n; ++i) if (id == info[i].id) return i;
        return -1;
    }
    // Effective value: automation override if active, else the user value.
    float get(int i) const { const float o = over[i].load(std::memory_order_relaxed); return std::isnan(o) ? base[i].load(std::memory_order_relaxed) : o; }
    int getInt(int i) const { return (int)std::lround(get(i)); }
    bool getBool(int i) const { return get(i) >= 0.5f; }
    float getBase(int i) const { return base[i].load(std::memory_order_relaxed); }
    bool isAutomated(int i) const { return !std::isnan(over[i].load(std::memory_order_relaxed)); }
    void set(int i, float v) { base[i].store(clamp(i, v), std::memory_order_relaxed); }
    void setOverride(int i, float v) { over[i].store(clamp(i, v), std::memory_order_relaxed); }
    void clearOverrides() { for (int i = 0; i < n; ++i) over[i].store(NAN, std::memory_order_relaxed); }
    void resetToDefaults() { for (int i = 0; i < n; ++i) base[i].store(info[i].def); }

    float clamp(int i, float v) const {
        const auto& m = info[i];
        v = juce::jlimit(m.min, m.max, v);
        if (m.scale == ParamScale::Int || m.scale == ParamScale::Bool || m.scale == ParamScale::Choice) v = std::round(v);
        return v;
    }
    // Normalized 0..1 <-> value (used by knobs and automation lanes).
    float toNorm(int i, float v) const {
        const auto& m = info[i];
        if (m.max <= m.min) return 0.f;
        if (m.scale == ParamScale::Log && m.min > 0.f) return std::log(v / m.min) / std::log(m.max / m.min);
        return (v - m.min) / (m.max - m.min);
    }
    float fromNorm(int i, float t) const {
        const auto& m = info[i];
        t = juce::jlimit(0.f, 1.f, t);
        if (m.scale == ParamScale::Log && m.min > 0.f) return clamp(i, m.min * std::pow(m.max / m.min, t));
        return clamp(i, m.min + (m.max - m.min) * t);
    }
    juce::String format(int i, float v) const {
        const auto& m = info[i];
        if (m.scale == ParamScale::Choice) {
            auto opts = juce::StringArray::fromTokens(m.options, "|", "");
            const int k = (int)std::lround(v);
            return juce::isPositiveAndBelow(k, opts.size()) ? opts[k] : juce::String(k);
        }
        if (m.scale == ParamScale::Bool) return v >= 0.5f ? "On" : "Off";
        const juce::String u(m.unit);
        if (u == "Hz" && v >= 1000.f) return juce::String(v / 1000.f, 2) + " kHz";
        if (u == "ms" && v >= 1000.f) return juce::String(v / 1000.f, 2) + " s";
        if (u == "%") return juce::String(juce::roundToInt(v)) + "%";
        if (m.scale == ParamScale::Int) return juce::String(juce::roundToInt(v)) + (u.isEmpty() ? "" : " " + u);
        const int dp = std::fabs(v) >= 100.f ? 0 : (std::fabs(v) >= 10.f ? 1 : 2);
        return juce::String(v, dp) + (u.isEmpty() ? "" : " " + u);
    }

    juce::var toVar() const {
        auto* o = new juce::DynamicObject();
        for (int i = 0; i < n; ++i) o->setProperty(info[i].id, getBase(i));
        return juce::var(o);
    }
    void fromVar(const juce::var& v) {
        if (auto* o = v.getDynamicObject())
            for (int i = 0; i < n; ++i)
                if (o->hasProperty(info[i].id)) set(i, (float)o->getProperty(info[i].id));
    }
    void copyFrom(const ParamSet& other) {
        for (int i = 0; i < n && i < other.n; ++i) set(i, other.getBase(i));
    }

private:
    const ParamInfo* info;
    int n;
    std::unique_ptr<std::atomic<float>[]> base, over;
};

using ParamSetPtr = std::shared_ptr<ParamSet>;
