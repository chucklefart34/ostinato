// Platform services implemented in Platform/Apple.mm on Apple systems (stubs elsewhere, in Exporter.cpp).
// Kept free of project headers so Apple's system headers can be included next to it.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

namespace PlatformAudio {
bool encodeAAC(const juce::AudioBuffer<float>& audio, double sampleRate, int kbps, const juce::File& out, juce::String& error);
void haptic(int strength);   // 0 light, 1 medium, 2 heavy; no-op where unsupported
bool hapticsAvailable();
}
