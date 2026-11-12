// Audio sample storage, decoding (WAV/AIFF/FLAC/OGG/MP3 + platform codecs), built-in
// synthesized drum sounds and sample packs, and waveform overviews for the UI.
#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

struct SampleData {
    juce::AudioBuffer<float> buffer;   // 1 or 2 channels, native sample rate
    double sampleRate = 48000.0;
    float peak = 0.f;
    juce::String name, path;
    // Min/max overview, one pair per `overviewStep` frames (mono mix), for waveform drawing.
    std::vector<float> ovMin, ovMax;
    static constexpr int overviewStep = 256;

    int frames() const { return buffer.getNumSamples(); }
    double seconds() const { return frames() / sampleRate; }
    // Hermite-interpolated read; out-of-range returns 0.
    inline void read(double pos, float& l, float& r) const {
        const int n = frames();
        const int i = (int)std::floor(pos);
        if (i < -1 || i >= n) { l = r = 0.f; return; }
        const float f = (float)(pos - i);
        auto at = [n](const float* d, int k) { return (k >= 0 && k < n) ? d[k] : 0.f; };
        auto herm = [&](const float* d) {
            const float xm1 = at(d, i - 1), x0 = at(d, i), x1 = at(d, i + 1), x2 = at(d, i + 2);
            const float c = (x1 - xm1) * 0.5f, v = x0 - x1, w = c + v, a = w + v + (x2 - x0) * 0.5f, b = w + a;
            return ((a * f - b) * f + c) * f + x0;
        };
        l = herm(buffer.getReadPointer(0));
        r = buffer.getNumChannels() > 1 ? herm(buffer.getReadPointer(1)) : l;
    }
    void computeOverview();
};
using SamplePtr = std::shared_ptr<const SampleData>;

namespace DrumSynth {
// Procedurally synthesized one-shots (royalty-free by construction).
std::shared_ptr<SampleData> render(const juce::String& id, double sampleRate = 48000.0);
}

class SampleBank {
public:
    static SampleBank& get();

    juce::AudioFormatManager& formats() { return fmt; }
    void setProjectDir(const juce::File& dir);
    juce::File projectDir() const;
    juce::File libraryDir() const;          // built-in packs + imports live here
    juce::File resolve(const juce::String& ref) const;

    // Returns a cached sample, or nullptr and starts loading in the background.
    // `onLoaded` (message thread) fires once any pending load finishes.
    SamplePtr request(const juce::String& ref);
    SamplePtr loadNow(const juce::String& ref);    // blocking (export, tests, previews)
    std::function<void()> onLoaded;

    static std::shared_ptr<SampleData> decodeFile(const juce::File& f, juce::String& error);
    void purgeUnused();
    size_t memoryBytes() const;
    void clear();

    // Creates the royalty-free sample packs in libraryDir() on first run.
    void ensureBuiltinPacks(std::function<void(float)> progress = {});
    static juce::StringArray supportedExtensions();

private:
    SampleBank();
    SamplePtr loadInternal(const juce::String& ref);
    juce::AudioFormatManager fmt;
    mutable std::mutex lock;
    std::map<juce::String, SamplePtr> cache;
    std::map<juce::String, bool> pending;
    juce::File projDir;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withThreadName("SampleLoader").withNumberOfThreads(2) };
};
