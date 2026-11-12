// Static catalog of instrument engines, effects, factory presets and drum sounds.
#pragma once
#include "Params.h"
#include <vector>

struct ParamTable { const ParamInfo* infos; int count; };

struct EngineInfo {
    const char* id;
    const char* name;
    const char* description;
};

struct InstrumentPreset {
    const char* name;       // shown in the instrument browser
    const char* category;   // Synths | Keys | Instruments | Drums | Sampler
    const char* engine;
    const char* values;     // "param=value,param=value" overrides on top of defaults
};

struct EffectInfo {
    const char* id;
    const char* name;
    const char* category;   // Dynamics | EQ | Time | Distortion | Other
    const char* description;
};

struct EffectPreset { const char* effect; const char* name; const char* values; };

// A ready-made effect chain (vocal / instrument recording chains).
struct ChainPreset {
    const char* name;
    const char* category;   // Vocals | Instruments
    std::vector<std::pair<const char*, const char*>> effects;   // effect id, values
};

struct DrumSoundInfo { const char* id; const char* name; const char* category; };

namespace Catalog {
const std::vector<EngineInfo>& engines();
ParamTable engineParams(const juce::String& engine);
ParamTable arpParams();
ParamTable channelParams();
ParamTable drumPadParams();
ParamTable inputParams();      // recording input stage of audio tracks
const std::vector<ChainPreset>& chainPresets();
const std::vector<InstrumentPreset>& instrumentPresets();
const InstrumentPreset* findPreset(const juce::String& name);

const std::vector<EffectInfo>& effects();
const EffectInfo* findEffect(const juce::String& id);
ParamTable effectParams(const juce::String& id);
const std::vector<EffectPreset>& effectPresets();

const std::vector<DrumSoundInfo>& drumSounds();
// Default kit layouts: 16 builtin sound ids per kit.
const std::vector<std::pair<juce::String, juce::StringArray>>& drumKits();

ParamSetPtr makeParams(ParamTable t);
// Effect Designer: what knob k (0=A,1=B,2=C) of module `type` means, its real value for a 0..1 setting, display text.
const char* designerKnobName(int type, int k);
// amount 1 = new random sound; smaller = mutate (move every control a little). Keeps the result playable.
void randomizeSound(ParamSet& ps, const juce::String& engine, float amount, uint32_t seed);
float designerValue(int type, int k, float v01);
void designerDefaults(int type, float out[4]);   // sensible A, B, C, Mix (in %) when a module is chosen
juce::String designerFormat(int type, int k, float v01);
void applyValues(ParamSet& ps, const juce::String& values);   // "a=1,b=2"
juce::String engineName(const juce::String& engine);
}  // namespace Catalog
