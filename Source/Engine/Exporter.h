// Offline (faster than real time) song rendering + file encoding.
#pragma once
#include "Renderer.h"
#include <juce_audio_formats/juce_audio_formats.h>

struct ExportSettings {
    enum Format { WAV = 0, FLAC = 1, OGG = 2, AAC = 3 };
    Format format = WAV;
    double sampleRate = 48000.0;
    int bitDepth = 24;            // WAV: 16, 24, 32 (float); FLAC: 16, 24
    int oggQuality = 6;           // index into OggVorbisAudioFormat::getQualityOptions()
    int aacKbps = 256;
    bool loopRangeOnly = false;
    bool dither = true;           // TPDF dither when writing 16-bit
    double maxTailSeconds = 15.0; // reverb/delay tails are rendered until silent (up to this)
    juce::File destination;

    static juce::StringArray presetNames();
    static ExportSettings preset(int index);
    static bool formatAvailable(Format f);
    juce::String extension() const;
};

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    juce::String error;
    juce::File file;
    double seconds = 0.0;
    float peakDb = -100.f;
    double renderMs = 0.0;
};

class Exporter : public juce::Thread {
public:
    Exporter(const Project& project, ExportSettings settings);
    ~Exporter() override;
    void run() override;

    std::atomic<float> progress { 0.f };
    std::atomic<bool> finished { false };
    ExportResult result;
    std::function<void(const ExportResult&)> onDone;   // message thread

    // Synchronous helper (tests, CLI): renders into a buffer without encoding.
    static bool renderToBuffer(const Project& p, double sampleRate, juce::AudioBuffer<float>& out, double maxTail, juce::String& error,
                               const std::atomic<bool>* cancel = nullptr, std::atomic<float>* prog = nullptr, bool loopOnly = false);

private:
    juce::var projectState;
    ExportSettings settings;
};

// Platform encoders (Platform/*.mm). Return false with an error when unavailable.
namespace PlatformAudio {
bool encodeAAC(const juce::AudioBuffer<float>& audio, double sampleRate, int kbps, const juce::File& out, juce::String& error);
void haptic(int strength);   // 0 light, 1 medium, 2 heavy; no-op where unsupported
bool hapticsAvailable();
}
