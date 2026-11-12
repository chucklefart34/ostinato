// Instrument engines. Every engine synthesizes or plays back real audio.
#pragma once
#include "Params.h"
#include "Samples.h"
#include <array>
#include <memory>

struct TransportInfo {
    double bpm = 120.0;
    double tick = 0.0;
    bool playing = false;
};

class Instrument {
public:
    virtual ~Instrument() = default;
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    // `detune` is an additional pitch offset in semitones (drum step pitch etc).
    virtual void noteOn(int note, float velocity, float detune = 0.f) = 0;
    virtual void noteOff(int note) = 0;
    virtual void allNotesOff(bool hard) = 0;
    // Adds `n` samples into L/R (n <= dsp::kQuantum is typical but any n works).
    virtual void render(float* L, float* R, int n) = 0;
    virtual int activeVoices() const = 0;

    virtual void setPitchBend(float v) { bend = v; }          // -1..1
    virtual void setModWheel(float v) { modWheel = v; }       // 0..1
    virtual void setSustain(bool on) { sustain = on; }
    void setTransport(const TransportInfo* t) { transport = t; }

protected:
    float bend = 0.f, modWheel = 0.f;
    bool sustain = false;
    const TransportInfo* transport = nullptr;
};

struct InstrumentSetup {
    juce::String engine;
    ParamSetPtr params;
    SamplePtr sample;                          // sampler engine
    int sampleRoot = 60;
    std::array<SamplePtr, 16> padSamples {};   // drum engine
    std::array<ParamSetPtr, 16> padParams {};
};

std::unique_ptr<Instrument> createInstrument(const InstrumentSetup& setup);

// Pads are mapped to MIDI notes 36..51 (C1 = pad 1).
constexpr int kDrumBaseNote = 36;
