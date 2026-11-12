// Real-time audio effects. All 26 effects in the catalog are implemented here.
#pragma once
#include "Params.h"
#include "Instruments.h"
#include <atomic>
#include <memory>

class Effect {
public:
    virtual ~Effect() = default;
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    virtual void process(float* L, float* R, int n) = 0;   // in place, n <= dsp::kQuantum typical
    virtual void reset() {}
    virtual int latencySamples() const { return 0; }
    void setTransport(const TransportInfo* t) { transport = t; }
    // Optional meter for the UI (gain reduction in dB for dynamics, else 0).
    std::atomic<float> meterDb { 0.f };
protected:
    const TransportInfo* transport = nullptr;
    double bpm() const { return transport ? transport->bpm : 120.0; }
};

std::unique_ptr<Effect> createEffect(const juce::String& type, ParamSetPtr params);
