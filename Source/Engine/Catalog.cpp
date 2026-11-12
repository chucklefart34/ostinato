#include "Catalog.h"
#include "Dsp.h"

using PS = ParamScale;
#define WAVES "Saw|Square|Triangle|Sine|Noise|Pulse"
#define LFOSHAPES "Sine|Triangle|Saw|Square|S&H"
#define SYNCOFF "Off|1/1|1/2|1/2D|1/4|1/4D|1/4T|1/8|1/8D|1/8T|1/16|1/16D|1/16T|1/32"
#define SYNCFREE "Free|1/1|1/2|1/2D|1/4|1/4D|1/4T|1/8|1/8D|1/8T|1/16|1/16D|1/16T|1/32"

// Shared tail for melodic engines: amp envelope, LFO, glide, voice mode, output.
#define AMP_ENV(a, d, s, r) \
    { "a_a", "Attack", 0.001f, 10.f, a, "s", PS::Log, nullptr, "Amp Envelope" }, \
    { "a_d", "Decay", 0.001f, 10.f, d, "s", PS::Log, nullptr, "Amp Envelope" }, \
    { "a_s", "Sustain", 0.f, 100.f, s, "%", PS::Linear, nullptr, "Amp Envelope" }, \
    { "a_r", "Release", 0.001f, 10.f, r, "s", PS::Log, nullptr, "Amp Envelope" }
#define LFO_BLOCK \
    { "lfo_shape", "LFO Shape", 0, 4, 0, "", PS::Choice, LFOSHAPES, "LFO" }, \
    { "lfo_rate", "LFO Rate", 0.05f, 20.f, 5.f, "Hz", PS::Log, nullptr, "LFO" }, \
    { "lfo_sync", "LFO Sync", 0, 13, 0, "", PS::Choice, SYNCOFF, "LFO" }, \
    { "lfo_pitch", "LFO > Pitch", 0.f, 1200.f, 0.f, "ct", PS::Linear, nullptr, "LFO" }, \
    { "lfo_cutoff", "LFO > Filter", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "LFO" }, \
    { "lfo_amp", "LFO > Amp", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "LFO" }
#define VOICE_BLOCK \
    { "glide", "Portamento", 0.f, 2000.f, 0.f, "ms", PS::Linear, nullptr, "Voice" }, \
    { "mono", "Mono", 0, 1, 0, "", PS::Bool, nullptr, "Voice" }, \
    { "legato", "Legato", 0, 1, 0, "", PS::Bool, nullptr, "Voice" }, \
    { "bend", "Bend Range", 0, 24, 2, "st", PS::Int, nullptr, "Voice" }, \
    { "transpose", "Transpose", -24, 24, 0, "st", PS::Int, nullptr, "Voice" }, \
    { "vel_sens", "Velocity", 0.f, 100.f, 70.f, "%", PS::Linear, nullptr, "Output" }, \
    { "level", "Level", -36.f, 12.f, -6.f, "dB", PS::Linear, nullptr, "Output" }
#define FILTER_BLOCK(cut, res, env) \
    { "flt_type", "Filter", 0, 3, 0, "", PS::Choice, "LP 24|LP 12|HP|BP", "Filter" }, \
    { "cutoff", "Cutoff", 20.f, 20000.f, cut, "Hz", PS::Log, nullptr, "Filter" }, \
    { "reso", "Resonance", 0.f, 100.f, res, "%", PS::Linear, nullptr, "Filter" }, \
    { "flt_env", "Env Amount", -100.f, 100.f, env, "%", PS::Linear, nullptr, "Filter" }, \
    { "flt_key", "Key Track", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Filter" }, \
    { "drive", "Drive", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Filter" }, \
    { "f_a", "F Attack", 0.001f, 10.f, 0.005f, "s", PS::Log, nullptr, "Filter Envelope" }, \
    { "f_d", "F Decay", 0.001f, 10.f, 0.4f, "s", PS::Log, nullptr, "Filter Envelope" }, \
    { "f_s", "F Sustain", 0.f, 100.f, 30.f, "%", PS::Linear, nullptr, "Filter Envelope" }, \
    { "f_r", "F Release", 0.001f, 10.f, 0.3f, "s", PS::Log, nullptr, "Filter Envelope" }
#define UNISON_BLOCK \
    { "unison", "Unison", 1, 7, 1, "", PS::Int, nullptr, "Oscillators" }, \
    { "uni_detune", "Detune", 0.f, 100.f, 20.f, "%", PS::Linear, nullptr, "Oscillators" }, \
    { "uni_width", "Width", 0.f, 100.f, 60.f, "%", PS::Linear, nullptr, "Oscillators" }

// Final tone shaping available on every melodic engine (applied after the engine's own sound).
#define SHAPE_BLOCK \
    { "sh_fine", "Fine Tune", -100.f, 100.f, 0.f, "ct", PS::Linear, nullptr, "Shape" }, \
    { "sh_hp", "Low Cut", 20.f, 2000.f, 20.f, "Hz", PS::Log, nullptr, "Shape" }, \
    { "sh_lp", "High Cut", 200.f, 20000.f, 20000.f, "Hz", PS::Log, nullptr, "Shape" }, \
    { "sh_drive", "Warmth", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Shape" }, \
    { "sh_width", "Stereo Width", 0.f, 200.f, 100.f, "%", PS::Linear, nullptr, "Shape" }

static const ParamInfo kAnalog[] = {
    { "osc1_wave", "Osc 1", 0, 5, 0, "", PS::Choice, WAVES, "Oscillators" },
    { "osc1_pw", "Pulse Width", 5.f, 95.f, 50.f, "%", PS::Linear, nullptr, "Oscillators" },
    { "osc2_wave", "Osc 2", 0, 5, 0, "", PS::Choice, WAVES, "Oscillators" },
    { "osc2_semi", "Osc 2 Pitch", -24, 24, 0, "st", PS::Int, nullptr, "Oscillators" },
    { "osc2_fine", "Osc 2 Fine", -50.f, 50.f, 7.f, "ct", PS::Linear, nullptr, "Oscillators" },
    { "osc_mix", "Osc Mix", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Oscillators" },
    { "sub", "Sub Osc", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Oscillators" },
    { "noise", "Noise", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Oscillators" },
    UNISON_BLOCK,
    FILTER_BLOCK(6000.f, 15.f, 30.f),
    AMP_ENV(0.005f, 0.3f, 80.f, 0.25f),
    LFO_BLOCK,
    VOICE_BLOCK,
    SHAPE_BLOCK,
};

static const ParamInfo kFM[] = {
    { "ratio_c", "Carrier Ratio", 0.5f, 16.f, 1.f, "x", PS::Linear, nullptr, "Operators" },
    { "ratio_m1", "Mod 1 Ratio", 0.5f, 16.f, 2.f, "x", PS::Linear, nullptr, "Operators" },
    { "index_m1", "Mod 1 Depth", 0.f, 10.f, 2.5f, "", PS::Linear, nullptr, "Operators" },
    { "dec_m1", "Mod 1 Decay", 0.01f, 10.f, 0.8f, "s", PS::Log, nullptr, "Operators" },
    { "sus_m1", "Mod 1 Sustain", 0.f, 100.f, 25.f, "%", PS::Linear, nullptr, "Operators" },
    { "ratio_m2", "Mod 2 Ratio", 0.5f, 16.f, 1.f, "x", PS::Linear, nullptr, "Operators" },
    { "index_m2", "Mod 2 Depth", 0.f, 10.f, 0.f, "", PS::Linear, nullptr, "Operators" },
    { "dec_m2", "Mod 2 Decay", 0.01f, 10.f, 0.4f, "s", PS::Log, nullptr, "Operators" },
    { "feedback", "Feedback", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Operators" },
    { "vel_index", "Vel > Depth", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Operators" },
    { "tone", "Tone", 200.f, 20000.f, 20000.f, "Hz", PS::Log, nullptr, "Operators" },
    AMP_ENV(0.002f, 1.2f, 40.f, 0.4f),
    LFO_BLOCK,
    VOICE_BLOCK,
    SHAPE_BLOCK,
};

static const ParamInfo kWavetable[] = {
    { "table", "Wavetable", 0, 4, 0, "", PS::Choice, "Basic|Harmonic|Vocal|Digital|PWM", "Wavetable" },
    { "pos", "Position", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Wavetable" },
    { "pos_env", "Env > Position", -100.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Wavetable" },
    { "pos_lfo", "LFO > Position", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Wavetable" },
    { "sub", "Sub Osc", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Wavetable" },
    UNISON_BLOCK,
    FILTER_BLOCK(12000.f, 10.f, 0.f),
    AMP_ENV(0.005f, 0.4f, 80.f, 0.3f),
    LFO_BLOCK,
    VOICE_BLOCK,
    SHAPE_BLOCK,
};

static const ParamInfo kPiano[] = {
    { "model", "Model", 0, 2, 0, "", PS::Choice, "Grand|Upright|Bright", "Piano" },
    { "bright", "Brightness", 0.f, 100.f, 55.f, "%", PS::Linear, nullptr, "Piano" },
    { "decay", "Decay", 0.5f, 20.f, 7.f, "s", PS::Log, nullptr, "Piano" },
    { "hammer", "Hammer", 0.f, 100.f, 35.f, "%", PS::Linear, nullptr, "Piano" },
    { "detune", "String Detune", 0.f, 10.f, 1.2f, "ct", PS::Linear, nullptr, "Piano" },
    { "a_r", "Release", 0.02f, 3.f, 0.25f, "s", PS::Log, nullptr, "Piano" },
    { "width", "Stereo Width", 0.f, 100.f, 60.f, "%", PS::Linear, nullptr, "Output" },
    { "bend", "Bend Range", 0, 24, 2, "st", PS::Int, nullptr, "Voice" },
    { "transpose", "Transpose", -24, 24, 0, "st", PS::Int, nullptr, "Voice" },
    { "vel_sens", "Velocity", 0.f, 100.f, 80.f, "%", PS::Linear, nullptr, "Output" },
    { "level", "Level", -36.f, 12.f, -4.f, "dB", PS::Linear, nullptr, "Output" },
    SHAPE_BLOCK,
};

static const ParamInfo kEPiano[] = {
    { "tine", "Tine / Bell", 0.f, 100.f, 45.f, "%", PS::Linear, nullptr, "Electric Piano" },
    { "bark", "Bark (Drive)", 0.f, 100.f, 30.f, "%", PS::Linear, nullptr, "Electric Piano" },
    { "decay", "Decay", 0.3f, 15.f, 4.f, "s", PS::Log, nullptr, "Electric Piano" },
    { "a_r", "Release", 0.02f, 3.f, 0.2f, "s", PS::Log, nullptr, "Electric Piano" },
    { "trem_rate", "Tremolo Rate", 0.1f, 12.f, 4.5f, "Hz", PS::Log, nullptr, "Tremolo" },
    { "trem_depth", "Tremolo Depth", 0.f, 100.f, 25.f, "%", PS::Linear, nullptr, "Tremolo" },
    { "tone", "Tone", 500.f, 16000.f, 6000.f, "Hz", PS::Log, nullptr, "Output" },
    { "width", "Stereo Width", 0.f, 100.f, 70.f, "%", PS::Linear, nullptr, "Output" },
    { "bend", "Bend Range", 0, 24, 2, "st", PS::Int, nullptr, "Voice" },
    { "transpose", "Transpose", -24, 24, 0, "st", PS::Int, nullptr, "Voice" },
    { "vel_sens", "Velocity", 0.f, 100.f, 80.f, "%", PS::Linear, nullptr, "Output" },
    { "level", "Level", -36.f, 12.f, -4.f, "dB", PS::Linear, nullptr, "Output" },
    SHAPE_BLOCK,
};

static const ParamInfo kOrgan[] = {
    { "db16", "16'", 0, 8, 8, "", PS::Int, nullptr, "Drawbars" },
    { "db513", "5 1/3'", 0, 8, 8, "", PS::Int, nullptr, "Drawbars" },
    { "db8", "8'", 0, 8, 8, "", PS::Int, nullptr, "Drawbars" },
    { "db4", "4'", 0, 8, 0, "", PS::Int, nullptr, "Drawbars" },
    { "db223", "2 2/3'", 0, 8, 0, "", PS::Int, nullptr, "Drawbars" },
    { "db2", "2'", 0, 8, 0, "", PS::Int, nullptr, "Drawbars" },
    { "db135", "1 3/5'", 0, 8, 0, "", PS::Int, nullptr, "Drawbars" },
    { "db113", "1 1/3'", 0, 8, 0, "", PS::Int, nullptr, "Drawbars" },
    { "db1", "1'", 0, 8, 0, "", PS::Int, nullptr, "Drawbars" },
    { "perc", "Percussion", 0, 1, 1, "", PS::Bool, nullptr, "Percussion" },
    { "perc_harm", "Perc Harmonic", 0, 1, 1, "", PS::Choice, "2nd|3rd", "Percussion" },
    { "perc_decay", "Perc Decay", 0.1f, 2.f, 0.4f, "s", PS::Log, nullptr, "Percussion" },
    { "click", "Key Click", 0.f, 100.f, 30.f, "%", PS::Linear, nullptr, "Percussion" },
    { "rotary", "Rotary", 0, 2, 1, "", PS::Choice, "Off|Slow|Fast", "Rotary" },
    { "drive", "Drive", 0.f, 100.f, 15.f, "%", PS::Linear, nullptr, "Rotary" },
    { "a_r", "Release", 0.005f, 1.f, 0.03f, "s", PS::Log, nullptr, "Output" },
    { "transpose", "Transpose", -24, 24, 0, "st", PS::Int, nullptr, "Voice" },
    { "level", "Level", -36.f, 12.f, -8.f, "dB", PS::Linear, nullptr, "Output" },
    SHAPE_BLOCK,
};

static const ParamInfo kString[] = {
    { "model", "Model", 0, 3, 0, "", PS::Choice, "Acoustic|Electric|Bass|Nylon", "String" },
    { "pluck", "Pluck Position", 0.f, 100.f, 30.f, "%", PS::Linear, nullptr, "String" },
    { "bright", "Brightness", 0.f, 100.f, 60.f, "%", PS::Linear, nullptr, "String" },
    { "decay", "Sustain", 0.2f, 15.f, 3.5f, "s", PS::Log, nullptr, "String" },
    { "body", "Body", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Body & Amp" },
    { "drive", "Drive", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Body & Amp" },
    { "tone", "Tone", 500.f, 16000.f, 7000.f, "Hz", PS::Log, nullptr, "Body & Amp" },
    { "a_r", "Release", 0.01f, 3.f, 0.15f, "s", PS::Log, nullptr, "Output" },
    { "width", "Stereo Width", 0.f, 100.f, 40.f, "%", PS::Linear, nullptr, "Output" },
    VOICE_BLOCK,
    SHAPE_BLOCK,
};

static const ParamInfo kEnsemble[] = {
    { "model", "Model", 0, 2, 0, "", PS::Choice, "Strings|Brass|Choir", "Ensemble" },
    { "detune", "Ensemble", 0.f, 100.f, 40.f, "%", PS::Linear, nullptr, "Ensemble" },
    { "bright", "Brightness", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Ensemble" },
    { "vowel", "Vowel", 0.f, 4.f, 0.f, "", PS::Linear, nullptr, "Ensemble" },
    { "breath", "Breath", 0.f, 100.f, 10.f, "%", PS::Linear, nullptr, "Ensemble" },
    { "vib_rate", "Vibrato Rate", 0.5f, 10.f, 5.f, "Hz", PS::Log, nullptr, "Vibrato" },
    { "vib_depth", "Vibrato Depth", 0.f, 100.f, 15.f, "ct", PS::Linear, nullptr, "Vibrato" },
    { "vib_delay", "Vibrato Delay", 0.f, 2.f, 0.4f, "s", PS::Linear, nullptr, "Vibrato" },
    AMP_ENV(0.25f, 0.5f, 85.f, 0.6f),
    { "width", "Stereo Width", 0.f, 100.f, 80.f, "%", PS::Linear, nullptr, "Output" },
    VOICE_BLOCK,
    SHAPE_BLOCK,
};

static const ParamInfo kSampler[] = {
    { "tune", "Tune", -24, 24, 0, "st", PS::Int, nullptr, "Sample" },
    { "fine", "Fine", -100.f, 100.f, 0.f, "ct", PS::Linear, nullptr, "Sample" },
    { "start", "Start", 0.f, 99.f, 0.f, "%", PS::Linear, nullptr, "Sample" },
    { "loop", "Loop", 0, 1, 0, "", PS::Bool, nullptr, "Sample" },
    { "reverse", "Reverse", 0, 1, 0, "", PS::Bool, nullptr, "Sample" },
    { "keytrack", "Key Track", 0, 1, 1, "", PS::Bool, nullptr, "Sample" },
    { "cutoff", "Cutoff", 20.f, 20000.f, 20000.f, "Hz", PS::Log, nullptr, "Filter" },
    { "reso", "Resonance", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Filter" },
    AMP_ENV(0.001f, 1.f, 100.f, 0.2f),
    VOICE_BLOCK,
    SHAPE_BLOCK,
};


// ---------------------------------------------------------------- Studio Synth
#define SWAVES "Sine|Saw|Square|Triangle|Pulse|Noise|Supersaw (7+ voices)|Wavetable"
#define STUDIO_OSC(n, on, wave, lvl) \
    { "o" #n "_on", "Osc " #n " On", 0, 1, on, "", PS::Bool, nullptr, "Osc " #n }, \
    { "o" #n "_wave", "Wave", 0, 7, wave, "", PS::Choice, SWAVES, "Osc " #n }, \
    { "o" #n "_table", "Wavetable", 0, 4, 0, "", PS::Choice, "Basic|Harmonic|Vocal|Digital|PWM", "Osc " #n }, \
    { "o" #n "_pos", "Table Position", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Osc " #n }, \
    { "o" #n "_pw", "Pulse Width", 5.f, 95.f, 50.f, "%", PS::Linear, nullptr, "Osc " #n }, \
    { "o" #n "_oct", "Octave", -3, 3, 0, "", PS::Int, nullptr, "Osc " #n }, \
    { "o" #n "_semi", "Semitones", -12, 12, 0, "st", PS::Int, nullptr, "Osc " #n }, \
    { "o" #n "_fine", "Fine", -100.f, 100.f, 0.f, "ct", PS::Linear, nullptr, "Osc " #n }, \
    { "o" #n "_level", "Level", 0.f, 100.f, lvl, "%", PS::Linear, nullptr, "Osc " #n }, \
    { "o" #n "_pan", "Pan", -100.f, 100.f, 0.f, "", PS::Linear, nullptr, "Osc " #n }, \
    { "o" #n "_uni", "Unison Voices", 1, 8, 1, "", PS::Int, nullptr, "Osc " #n }, \
    { "o" #n "_det", "Unison Detune", 0.f, 100.f, 25.f, "%", PS::Linear, nullptr, "Osc " #n }, \
    { "o" #n "_spread", "Unison Spread", 0.f, 100.f, 70.f, "%", PS::Linear, nullptr, "Osc " #n }, \
    { "o" #n "_phase", "Free Phase", 0, 1, 1, "", PS::Bool, nullptr, "Osc " #n }
#define STUDIO_FILTER(n, type, cut, res, env) \
    { "f" #n "_type", "Filter " #n " Type", 0, 8, type, "", PS::Choice, "Low-pass 12|Low-pass 24|High-pass 12|High-pass 24|Band-pass|Notch|Peak|Comb|Formant (Cutoff = vowel)", "Filter " #n }, \
    { "f" #n "_cut", "Cutoff", 20.f, 20000.f, cut, "Hz", PS::Log, nullptr, "Filter " #n }, \
    { "f" #n "_res", "Resonance", 0.f, 100.f, res, "%", PS::Linear, nullptr, "Filter " #n }, \
    { "f" #n "_drive", "Drive", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Filter " #n }, \
    { "f" #n "_key", "Key Track", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Filter " #n }, \
    { "f" #n "_env", "Env 2 Amount", -100.f, 100.f, env, "%", PS::Linear, nullptr, "Filter " #n }, \
    { "f" #n "_vel", "Velocity", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Filter " #n }
#define STUDIO_ENV(p, grp, a, d, s, r) \
    { #p "_a", "Attack", 0.001f, 10.f, a, "s", PS::Log, nullptr, grp }, \
    { #p "_d", "Decay", 0.001f, 10.f, d, "s", PS::Log, nullptr, grp }, \
    { #p "_s", "Sustain", 0.f, 100.f, s, "%", PS::Linear, nullptr, grp }, \
    { #p "_r", "Release", 0.001f, 10.f, r, "s", PS::Log, nullptr, grp }
#define STUDIO_LFO(n, rate) \
    { "l" #n "_shape", "LFO " #n " Shape", 0, 5, 0, "", PS::Choice, "Sine|Triangle|Saw|Square|Sample & Hold|Smooth Random", "LFO " #n }, \
    { "l" #n "_rate", "Rate", 0.02f, 40.f, rate, "Hz", PS::Log, nullptr, "LFO " #n }, \
    { "l" #n "_sync", "Tempo Sync", 0, 13, 0, "", PS::Choice, SYNCOFF, "LFO " #n }, \
    { "l" #n "_mode", "Mode", 0, 1, 0, "", PS::Choice, "Per note (restarts)|Shared (free running)", "LFO " #n }, \
    { "l" #n "_delay", "Delay", 0.f, 4.f, 0.f, "s", PS::Linear, nullptr, "LFO " #n }, \
    { "l" #n "_fade", "Fade In", 0.f, 4.f, 0.f, "s", PS::Linear, nullptr, "LFO " #n }
#define MOD_SOURCES "None|Envelope 2|Envelope 3|LFO 1|LFO 2|LFO 3|Velocity|Key|Mod Wheel|Pitch Bend|Random (per note)|Macro 1|Macro 2|Macro 3|Macro 4|Amp Envelope"
#define MOD_DESTS "None|Pitch|Osc 1 Pitch|Osc 2 Pitch|Osc 3 Pitch|Osc 1 Level|Osc 2 Level|Osc 3 Level|Osc 1 Table Pos|Osc 2 Table Pos|Osc 3 Table Pos|Pulse Width|FM Amount|Ring Mod|Unison Detune|Sub Level|Noise Level|Filter 1 Cutoff|Filter 1 Resonance|Filter 2 Cutoff|Filter 2 Resonance|Filter Drive|Pan|Volume|LFO 1 Rate|LFO 2 Rate|LFO 3 Rate"
#define STUDIO_MOD(n) \
    { "m" #n "_src", "Source", 0, 15, 0, "", PS::Choice, MOD_SOURCES, "Mod " #n }, \
    { "m" #n "_dst", "Destination", 0, 26, 0, "", PS::Choice, MOD_DESTS, "Mod " #n }, \
    { "m" #n "_amt", "Amount", -100.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Mod " #n }

static const ParamInfo kStudio[] = {
    STUDIO_OSC(1, 1, 1, 80.f),
    STUDIO_OSC(2, 0, 1, 60.f),
    STUDIO_OSC(3, 0, 0, 60.f),
    { "fm", "FM (Osc 2 > Osc 1)", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Osc Interaction" },
    { "sync", "Sync Osc 2 to Osc 1", 0, 1, 0, "", PS::Bool, nullptr, "Osc Interaction" },
    { "ring", "Ring Mod (Osc 1 x 2)", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Osc Interaction" },
    { "sub_level", "Sub Level", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Sub & Noise" },
    { "sub_oct", "Sub Octave", 0, 1, 0, "", PS::Choice, "1 octave down|2 octaves down", "Sub & Noise" },
    { "sub_wave", "Sub Wave", 0, 2, 0, "", PS::Choice, "Sine|Triangle|Square", "Sub & Noise" },
    { "noise_level", "Noise Level", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Sub & Noise" },
    { "noise_color", "Noise Colour", -100.f, 100.f, 0.f, "", PS::Linear, nullptr, "Sub & Noise" },
    STUDIO_FILTER(1, 1, 8000.f, 15.f, 20.f),
    { "f2_on", "Filter 2 On", 0, 1, 0, "", PS::Bool, nullptr, "Filter 2" },
    STUDIO_FILTER(2, 2, 200.f, 10.f, 0.f),
    { "f_route", "Routing", 0, 1, 0, "", PS::Choice, "Serial (1 then 2)|Parallel (1 + 2)", "Filter Routing" },
    STUDIO_ENV(a, "Amp Envelope", 0.005f, 0.3f, 80.f, 0.3f),
    STUDIO_ENV(e2, "Envelope 2", 0.005f, 0.5f, 30.f, 0.4f),
    STUDIO_ENV(e3, "Envelope 3", 0.01f, 1.f, 0.f, 0.5f),
    STUDIO_LFO(1, 5.f),
    STUDIO_LFO(2, 0.5f),
    STUDIO_LFO(3, 2.f),
    STUDIO_MOD(1), STUDIO_MOD(2), STUDIO_MOD(3), STUDIO_MOD(4),
    STUDIO_MOD(5), STUDIO_MOD(6), STUDIO_MOD(7), STUDIO_MOD(8),
    { "mac1", "Macro 1", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Macros" },
    { "mac2", "Macro 2", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Macros" },
    { "mac3", "Macro 3", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Macros" },
    { "mac4", "Macro 4", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Macros" },
    { "voices", "Voices", 1, 16, 8, "", PS::Int, nullptr, "Voice" },
    { "mode", "Play Mode", 0, 2, 0, "", PS::Choice, "Poly|Mono|Mono Legato", "Voice" },
    { "glide", "Glide", 0.f, 2000.f, 0.f, "ms", PS::Linear, nullptr, "Voice" },
    { "bend", "Bend Range", 0, 24, 2, "st", PS::Int, nullptr, "Voice" },
    { "transpose", "Transpose", -24, 24, 0, "st", PS::Int, nullptr, "Voice" },
    { "vel_sens", "Velocity", 0.f, 100.f, 70.f, "%", PS::Linear, nullptr, "Output" },
    { "level", "Level", -36.f, 12.f, -6.f, "dB", PS::Linear, nullptr, "Output" },
    SHAPE_BLOCK,
};

static const ParamInfo kDrums[] = {
    { "level", "Kit Level", -36.f, 12.f, -9.f, "dB", PS::Linear, nullptr, "Kit" },
    { "vel_sens", "Velocity", 0.f, 100.f, 80.f, "%", PS::Linear, nullptr, "Kit" },
    { "tune", "Kit Tune", -12, 12, 0, "st", PS::Int, nullptr, "Kit" },
    { "humanize", "Humanize", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Kit" },
};

static const ParamInfo kDrumPad[] = {
    { "tune", "Tune", -24.f, 24.f, 0.f, "st", PS::Linear, nullptr, "Pad" },
    { "gain", "Gain", -36.f, 12.f, 0.f, "dB", PS::Linear, nullptr, "Pad" },
    { "pan", "Pan", -100.f, 100.f, 0.f, "", PS::Linear, nullptr, "Pad" },
    { "decay", "Length", 0.02f, 4.f, 4.f, "s", PS::Log, nullptr, "Pad" },
    { "cutoff", "Cutoff", 100.f, 20000.f, 20000.f, "Hz", PS::Log, nullptr, "Pad" },
    { "choke", "Choke Group", 0, 4, 0, "", PS::Choice, "None|1|2|3|4", "Pad" },
    { "reverse", "Reverse", 0, 1, 0, "", PS::Bool, nullptr, "Pad" },
    { "mute", "Mute", 0, 1, 0, "", PS::Bool, nullptr, "Pad" },
    { "start", "Start", 0.f, 90.f, 0.f, "%", PS::Linear, nullptr, "Shape" },
    { "attack", "Attack", 0.f, 200.f, 0.f, "ms", PS::Linear, nullptr, "Shape" },
    { "pitch_env", "Pitch Drop", -24.f, 48.f, 0.f, "st", PS::Linear, nullptr, "Shape" },
    { "pitch_dec", "Drop Time", 5.f, 1000.f, 60.f, "ms", PS::Log, nullptr, "Shape" },
    { "hp", "Low Cut", 20.f, 8000.f, 20.f, "Hz", PS::Log, nullptr, "Shape" },
    { "reso", "Filter Resonance", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Shape" },
    { "drive", "Drive", 0.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Shape" },
};

static const ParamInfo kArp[] = {
    { "on", "Arpeggiator", 0, 1, 0, "", PS::Bool, nullptr, "Arpeggiator" },
    { "mode", "Mode", 0, 4, 0, "", PS::Choice, "Up|Down|Up-Down|Random|As Played", "Arpeggiator" },
    { "rate", "Rate", 0, 12, 9, "", PS::Choice, dsp::kSyncNames, "Arpeggiator" },
    { "octaves", "Octaves", 1, 4, 1, "", PS::Int, nullptr, "Arpeggiator" },
    { "gate", "Gate", 10.f, 100.f, 70.f, "%", PS::Linear, nullptr, "Arpeggiator" },
    { "chord", "Chord Trigger", 0, 7, 0, "", PS::Choice, "Off|Major|Minor|Power|Sus4|Maj7|Min7|Octave", "Chord" },
};

static const ParamInfo kChannel[] = {
    { "vol", "Volume", -60.f, 6.f, 0.f, "dB", PS::Linear, nullptr, "Channel" },
    { "pan", "Pan", -100.f, 100.f, 0.f, "", PS::Linear, nullptr, "Channel" },
    { "mute", "Mute", 0, 1, 0, "", PS::Bool, nullptr, "Channel" },
    { "solo", "Solo", 0, 1, 0, "", PS::Bool, nullptr, "Channel" },
    { "phase", "Phase Invert", 0, 1, 0, "", PS::Bool, nullptr, "Channel" },
    { "mono", "Mono", 0, 1, 0, "", PS::Bool, nullptr, "Channel" },
    { "send1", "Send A", -60.f, 6.f, -60.f, "dB", PS::Linear, nullptr, "Sends" },
    { "send2", "Send B", -60.f, 6.f, -60.f, "dB", PS::Linear, nullptr, "Sends" },
    { "send3", "Send C", -60.f, 6.f, -60.f, "dB", PS::Linear, nullptr, "Sends" },
    { "send4", "Send D", -60.f, 6.f, -60.f, "dB", PS::Linear, nullptr, "Sends" },
    { "pre1", "Send A Pre", 0, 1, 0, "", PS::Bool, nullptr, "Sends" },
    { "pre2", "Send B Pre", 0, 1, 0, "", PS::Bool, nullptr, "Sends" },
    { "pre3", "Send C Pre", 0, 1, 0, "", PS::Bool, nullptr, "Sends" },
    { "pre4", "Send D Pre", 0, 1, 0, "", PS::Bool, nullptr, "Sends" },
};


static const ParamInfo kDeEsser[] = {
    { "freq", "Frequency", 2000.f, 12000.f, 6000.f, "Hz", PS::Log, nullptr, "De-esser" },
    { "threshold", "Threshold", -60.f, 0.f, -28.f, "dB", PS::Linear, nullptr, "De-esser" },
    { "range", "Max Reduction", 0.f, 24.f, 10.f, "dB", PS::Linear, nullptr, "De-esser" },
    { "mode", "Mode", 0, 1, 0, "", PS::Choice, "Highs only|Whole signal", "De-esser" },
    { "listen", "Listen to S sounds", 0, 1, 0, "", PS::Bool, nullptr, "De-esser" },
};

// Effect Designer: six module slots in series plus a modulation section.
#define DES_MODULES "Empty|Low-pass|High-pass|Band-pass|Notch|Peak EQ|Drive|Wavefold|Bitcrush|Delay|Chorus|Ring Mod|Comb|Gain & Pan|Small Reverb|Tremolo"
#define DES_SLOT(n, type, a, b, c) \
    { "s" #n "_type", "Module " #n, 0, 15, type, "", PS::Choice, DES_MODULES, "Module " #n }, \
    { "s" #n "_a", "A", 0.f, 100.f, a, "%", PS::Linear, nullptr, "Module " #n }, \
    { "s" #n "_b", "B", 0.f, 100.f, b, "%", PS::Linear, nullptr, "Module " #n }, \
    { "s" #n "_c", "C", 0.f, 100.f, c, "%", PS::Linear, nullptr, "Module " #n }, \
    { "s" #n "_mix", "Mix", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Module " #n }
#define DES_TARGETS "None|1 A|1 B|1 C|1 Mix|2 A|2 B|2 C|2 Mix|3 A|3 B|3 C|3 Mix|4 A|4 B|4 C|4 Mix|5 A|5 B|5 C|5 Mix|6 A|6 B|6 C|6 Mix|Output"
#define DES_MOD(n) \
    { "m" #n "_src", "Source", 0, 3, 0, "", PS::Choice, "None|LFO 1|LFO 2|Envelope Follower", "Mod " #n }, \
    { "m" #n "_dst", "Target", 0, 25, 0, "", PS::Choice, DES_TARGETS, "Mod " #n }, \
    { "m" #n "_amt", "Amount", -100.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Mod " #n }
static const ParamInfo kDesigner[] = {
    { "in", "Input", -24.f, 24.f, 0.f, "dB", PS::Linear, nullptr, "Input & Output" },
    { "out", "Output", -24.f, 24.f, 0.f, "dB", PS::Linear, nullptr, "Input & Output" },
    { "mix", "Dry / Wet", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Input & Output" },
    DES_SLOT(1, 6, 40.f, 70.f, 0.f), DES_SLOT(2, 1, 60.f, 20.f, 0.f), DES_SLOT(3, 0, 50.f, 50.f, 50.f),
    DES_SLOT(4, 0, 50.f, 50.f, 50.f), DES_SLOT(5, 0, 50.f, 50.f, 50.f), DES_SLOT(6, 0, 50.f, 50.f, 50.f),
    { "l1_shape", "LFO 1 Shape", 0, 4, 0, "", PS::Choice, "Sine|Triangle|Saw|Square|Random", "LFO 1" },
    { "l1_rate", "Rate", 0.02f, 20.f, 1.f, "Hz", PS::Log, nullptr, "LFO 1" },
    { "l1_sync", "Tempo Sync", 0, 13, 0, "", PS::Choice, SYNCOFF, "LFO 1" },
    { "l2_shape", "LFO 2 Shape", 0, 4, 1, "", PS::Choice, "Sine|Triangle|Saw|Square|Random", "LFO 2" },
    { "l2_rate", "Rate", 0.02f, 20.f, 0.25f, "Hz", PS::Log, nullptr, "LFO 2" },
    { "l2_sync", "Tempo Sync", 0, 13, 0, "", PS::Choice, SYNCOFF, "LFO 2" },
    { "ef_att", "Follower Attack", 0.1f, 200.f, 5.f, "ms", PS::Log, nullptr, "Envelope Follower" },
    { "ef_rel", "Follower Release", 5.f, 2000.f, 150.f, "ms", PS::Log, nullptr, "Envelope Follower" },
    { "ef_gain", "Follower Sensitivity", 0.f, 36.f, 12.f, "dB", PS::Linear, nullptr, "Envelope Follower" },
    DES_MOD(1), DES_MOD(2), DES_MOD(3), DES_MOD(4),
};

// Recording input stage (per audio track): gain, monitoring, what gets recorded.
static const ParamInfo kInput[] = {
    { "gain", "Input Gain", -24.f, 24.f, 0.f, "dB", PS::Linear, nullptr, "Input" },
    { "monitor", "Hear Yourself", 0, 2, 0, "", PS::Choice, "Off|Direct (interface)|Through effects", "Input" },
    { "mon_level", "Monitor Level", -60.f, 6.f, 0.f, "dB", PS::Linear, nullptr, "Input" },
    { "print", "Record", 0, 1, 0, "", PS::Choice, "Clean (effects added after)|With effects", "Input" },
};

#define T(arr) ParamTable { arr, (int)(sizeof(arr) / sizeof(arr[0])) }

// ---------------------------------------------------------------- effects
static const ParamInfo kCompressor[] = {
    { "threshold", "Threshold", -60.f, 0.f, -18.f, "dB", PS::Linear, nullptr, "Compressor" },
    { "ratio", "Ratio", 1.f, 20.f, 4.f, "x", PS::Log, nullptr, "Compressor" },
    { "attack", "Attack", 0.1f, 200.f, 10.f, "ms", PS::Log, nullptr, "Compressor" },
    { "release", "Release", 10.f, 2000.f, 120.f, "ms", PS::Log, nullptr, "Compressor" },
    { "knee", "Knee", 0.f, 24.f, 6.f, "dB", PS::Linear, nullptr, "Compressor" },
    { "makeup", "Makeup", -12.f, 24.f, 0.f, "dB", PS::Linear, nullptr, "Compressor" },
    { "mix", "Mix", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Compressor" },
};
static const ParamInfo kLimiter[] = {
    { "gain", "Input Gain", 0.f, 24.f, 0.f, "dB", PS::Linear, nullptr, "Limiter" },
    { "ceiling", "Ceiling", -24.f, 0.f, -0.3f, "dB", PS::Linear, nullptr, "Limiter" },
    { "release", "Release", 1.f, 1000.f, 60.f, "ms", PS::Log, nullptr, "Limiter" },
};
static const ParamInfo kGate[] = {
    { "threshold", "Threshold", -90.f, 0.f, -45.f, "dB", PS::Linear, nullptr, "Gate" },
    { "range", "Range", -90.f, 0.f, -80.f, "dB", PS::Linear, nullptr, "Gate" },
    { "attack", "Attack", 0.1f, 50.f, 1.f, "ms", PS::Log, nullptr, "Gate" },
    { "hold", "Hold", 0.f, 500.f, 30.f, "ms", PS::Linear, nullptr, "Gate" },
    { "release", "Release", 5.f, 2000.f, 100.f, "ms", PS::Log, nullptr, "Gate" },
};
static const ParamInfo kExpander[] = {
    { "threshold", "Threshold", -80.f, 0.f, -40.f, "dB", PS::Linear, nullptr, "Expander" },
    { "ratio", "Ratio", 1.f, 10.f, 2.f, "x", PS::Log, nullptr, "Expander" },
    { "range", "Range", -80.f, 0.f, -40.f, "dB", PS::Linear, nullptr, "Expander" },
    { "attack", "Attack", 0.1f, 100.f, 2.f, "ms", PS::Log, nullptr, "Expander" },
    { "release", "Release", 5.f, 2000.f, 150.f, "ms", PS::Log, nullptr, "Expander" },
};
static const ParamInfo kEQ3[] = {
    { "low", "Low", -18.f, 18.f, 0.f, "dB", PS::Linear, nullptr, "Low" },
    { "low_f", "Low Freq", 40.f, 500.f, 150.f, "Hz", PS::Log, nullptr, "Low" },
    { "mid", "Mid", -18.f, 18.f, 0.f, "dB", PS::Linear, nullptr, "Mid" },
    { "mid_f", "Mid Freq", 200.f, 8000.f, 1000.f, "Hz", PS::Log, nullptr, "Mid" },
    { "mid_q", "Mid Q", 0.3f, 5.f, 0.8f, "", PS::Log, nullptr, "Mid" },
    { "high", "High", -18.f, 18.f, 0.f, "dB", PS::Linear, nullptr, "High" },
    { "high_f", "High Freq", 2000.f, 16000.f, 5000.f, "Hz", PS::Log, nullptr, "High" },
    { "output", "Output", -12.f, 12.f, 0.f, "dB", PS::Linear, nullptr, "Output" },
};
static const ParamInfo kParametric[] = {
    { "lc_on", "Low Cut", 0, 1, 0, "", PS::Bool, nullptr, "Low Cut" },
    { "lc_f", "Low Cut Freq", 20.f, 1000.f, 60.f, "Hz", PS::Log, nullptr, "Low Cut" },
    { "b1_f", "Low Shelf", 30.f, 800.f, 120.f, "Hz", PS::Log, nullptr, "Band 1" },
    { "b1_g", "LS Gain", -18.f, 18.f, 0.f, "dB", PS::Linear, nullptr, "Band 1" },
    { "b2_f", "Peak 1", 100.f, 6000.f, 500.f, "Hz", PS::Log, nullptr, "Band 2" },
    { "b2_g", "Peak 1 Gain", -18.f, 18.f, 0.f, "dB", PS::Linear, nullptr, "Band 2" },
    { "b2_q", "Peak 1 Q", 0.2f, 10.f, 1.f, "", PS::Log, nullptr, "Band 2" },
    { "b3_f", "Peak 2", 500.f, 16000.f, 3000.f, "Hz", PS::Log, nullptr, "Band 3" },
    { "b3_g", "Peak 2 Gain", -18.f, 18.f, 0.f, "dB", PS::Linear, nullptr, "Band 3" },
    { "b3_q", "Peak 2 Q", 0.2f, 10.f, 1.f, "", PS::Log, nullptr, "Band 3" },
    { "b4_f", "High Shelf", 1500.f, 18000.f, 8000.f, "Hz", PS::Log, nullptr, "Band 4" },
    { "b4_g", "HS Gain", -18.f, 18.f, 0.f, "dB", PS::Linear, nullptr, "Band 4" },
    { "hc_on", "High Cut", 0, 1, 0, "", PS::Bool, nullptr, "High Cut" },
    { "hc_f", "High Cut Freq", 1000.f, 20000.f, 16000.f, "Hz", PS::Log, nullptr, "High Cut" },
    { "output", "Output", -12.f, 12.f, 0.f, "dB", PS::Linear, nullptr, "Output" },
};
#define FILTER_FX(def) \
    { "cutoff", "Cutoff", 20.f, 20000.f, def, "Hz", PS::Log, nullptr, "Filter" }, \
    { "reso", "Resonance", 0.f, 100.f, 10.f, "%", PS::Linear, nullptr, "Filter" }, \
    { "slope", "Slope", 0, 1, 0, "", PS::Choice, "12 dB|24 dB", "Filter" }, \
    { "mix", "Mix", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Filter" }
static const ParamInfo kHPF[] = { FILTER_FX(120.f) };
static const ParamInfo kLPF[] = { FILTER_FX(4000.f) };
static const ParamInfo kBPF[] = { FILTER_FX(1000.f) };
static const ParamInfo kReverb[] = {
    { "size", "Size", 0.f, 100.f, 60.f, "%", PS::Linear, nullptr, "Reverb" },
    { "decay", "Decay", 0.2f, 15.f, 2.5f, "s", PS::Log, nullptr, "Reverb" },
    { "damping", "Damping", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Reverb" },
    { "predelay", "Pre-delay", 0.f, 200.f, 20.f, "ms", PS::Linear, nullptr, "Reverb" },
    { "lowcut", "Low Cut", 20.f, 1000.f, 120.f, "Hz", PS::Log, nullptr, "Reverb" },
    { "width", "Width", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Reverb" },
    { "mix", "Mix", 0.f, 100.f, 25.f, "%", PS::Linear, nullptr, "Reverb" },
};
#define DELAY_CORE(ms) \
    { "sync", "Sync", 0, 13, 4, "", PS::Choice, SYNCFREE, "Delay" }, \
    { "time", "Time (Free)", 1.f, 2000.f, ms, "ms", PS::Log, nullptr, "Delay" }, \
    { "feedback", "Feedback", 0.f, 95.f, 40.f, "%", PS::Linear, nullptr, "Delay" }, \
    { "tone", "Tone", 500.f, 20000.f, 8000.f, "Hz", PS::Log, nullptr, "Delay" }, \
    { "mix", "Mix", 0.f, 100.f, 30.f, "%", PS::Linear, nullptr, "Delay" }
static const ParamInfo kDelay[] = { DELAY_CORE(375.f) };
static const ParamInfo kPingPong[] = { DELAY_CORE(250.f), { "width", "Width", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Delay" } };
static const ParamInfo kEcho[] = {
    { "sync", "Sync", 0, 13, 0, "", PS::Choice, SYNCFREE, "Echo" },
    { "time", "Time (Free)", 20.f, 1500.f, 320.f, "ms", PS::Log, nullptr, "Echo" },
    { "feedback", "Feedback", 0.f, 110.f, 55.f, "%", PS::Linear, nullptr, "Echo" },
    { "wow", "Wow & Flutter", 0.f, 100.f, 30.f, "%", PS::Linear, nullptr, "Tape" },
    { "sat", "Saturation", 0.f, 100.f, 40.f, "%", PS::Linear, nullptr, "Tape" },
    { "tone", "Tone", 500.f, 10000.f, 3500.f, "Hz", PS::Log, nullptr, "Tape" },
    { "lowcut", "Low Cut", 20.f, 1000.f, 150.f, "Hz", PS::Log, nullptr, "Tape" },
    { "mix", "Mix", 0.f, 100.f, 30.f, "%", PS::Linear, nullptr, "Echo" },
};
static const ParamInfo kChorus[] = {
    { "rate", "Rate", 0.05f, 5.f, 0.8f, "Hz", PS::Log, nullptr, "Chorus" },
    { "depth", "Depth", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Chorus" },
    { "delay", "Delay", 5.f, 30.f, 12.f, "ms", PS::Linear, nullptr, "Chorus" },
    { "voices", "Voices", 1, 4, 2, "", PS::Int, nullptr, "Chorus" },
    { "width", "Width", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Chorus" },
    { "mix", "Mix", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Chorus" },
};
static const ParamInfo kFlanger[] = {
    { "rate", "Rate", 0.02f, 5.f, 0.25f, "Hz", PS::Log, nullptr, "Flanger" },
    { "depth", "Depth", 0.f, 100.f, 70.f, "%", PS::Linear, nullptr, "Flanger" },
    { "delay", "Delay", 0.3f, 8.f, 1.5f, "ms", PS::Linear, nullptr, "Flanger" },
    { "feedback", "Feedback", -95.f, 95.f, 55.f, "%", PS::Linear, nullptr, "Flanger" },
    { "mix", "Mix", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Flanger" },
};
static const ParamInfo kPhaser[] = {
    { "rate", "Rate", 0.02f, 8.f, 0.4f, "Hz", PS::Log, nullptr, "Phaser" },
    { "depth", "Depth", 0.f, 100.f, 70.f, "%", PS::Linear, nullptr, "Phaser" },
    { "stages", "Stages", 2, 12, 6, "", PS::Int, nullptr, "Phaser" },
    { "feedback", "Feedback", 0.f, 95.f, 40.f, "%", PS::Linear, nullptr, "Phaser" },
    { "center", "Center", 100.f, 4000.f, 700.f, "Hz", PS::Log, nullptr, "Phaser" },
    { "mix", "Mix", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Phaser" },
};
static const ParamInfo kSaturation[] = {
    { "drive", "Drive", 0.f, 24.f, 6.f, "dB", PS::Linear, nullptr, "Saturation" },
    { "tone", "Tone", 1000.f, 20000.f, 14000.f, "Hz", PS::Log, nullptr, "Saturation" },
    { "output", "Output", -24.f, 12.f, -3.f, "dB", PS::Linear, nullptr, "Saturation" },
    { "mix", "Mix", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Saturation" },
};
static const ParamInfo kOverdrive[] = {
    { "drive", "Drive", 0.f, 100.f, 40.f, "%", PS::Linear, nullptr, "Overdrive" },
    { "tone", "Tone", 500.f, 8000.f, 3000.f, "Hz", PS::Log, nullptr, "Overdrive" },
    { "output", "Output", -24.f, 12.f, -6.f, "dB", PS::Linear, nullptr, "Overdrive" },
    { "mix", "Mix", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Overdrive" },
};
static const ParamInfo kDistortion[] = {
    { "type", "Type", 0, 3, 0, "", PS::Choice, "Hard Clip|Fold|Fuzz|Rectify", "Distortion" },
    { "drive", "Drive", 0.f, 48.f, 18.f, "dB", PS::Linear, nullptr, "Distortion" },
    { "tone", "Tone", 500.f, 16000.f, 5000.f, "Hz", PS::Log, nullptr, "Distortion" },
    { "output", "Output", -36.f, 12.f, -12.f, "dB", PS::Linear, nullptr, "Distortion" },
    { "mix", "Mix", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Distortion" },
};
static const ParamInfo kBitcrusher[] = {
    { "bits", "Bits", 1, 16, 8, "", PS::Int, nullptr, "Bitcrusher" },
    { "down", "Downsample", 1, 32, 4, "x", PS::Int, nullptr, "Bitcrusher" },
    { "output", "Output", -24.f, 12.f, 0.f, "dB", PS::Linear, nullptr, "Bitcrusher" },
    { "mix", "Mix", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Bitcrusher" },
};
static const ParamInfo kWidener[] = {
    { "width", "Width", 0.f, 200.f, 140.f, "%", PS::Linear, nullptr, "Widener" },
    { "haas", "Haas Delay", 0.f, 30.f, 0.f, "ms", PS::Linear, nullptr, "Widener" },
    { "bass_mono", "Mono Below", 20.f, 500.f, 120.f, "Hz", PS::Log, nullptr, "Widener" },
};
static const ParamInfo kImager[] = {
    { "xover", "Crossover", 100.f, 5000.f, 600.f, "Hz", PS::Log, nullptr, "Imager" },
    { "low_w", "Low Width", 0.f, 200.f, 80.f, "%", PS::Linear, nullptr, "Imager" },
    { "high_w", "High Width", 0.f, 200.f, 130.f, "%", PS::Linear, nullptr, "Imager" },
    { "balance", "Balance", -100.f, 100.f, 0.f, "", PS::Linear, nullptr, "Imager" },
};
static const ParamInfo kPitch[] = {
    { "semis", "Semitones", -24, 24, 0, "st", PS::Int, nullptr, "Pitch" },
    { "cents", "Cents", -100.f, 100.f, 0.f, "ct", PS::Linear, nullptr, "Pitch" },
    { "window", "Window", 20.f, 200.f, 60.f, "ms", PS::Linear, nullptr, "Pitch" },
    { "mix", "Mix", 0.f, 100.f, 100.f, "%", PS::Linear, nullptr, "Pitch" },
};
static const ParamInfo kAutoPan[] = {
    { "sync", "Sync", 0, 13, 0, "", PS::Choice, SYNCFREE, "Auto-pan" },
    { "rate", "Rate (Free)", 0.05f, 20.f, 1.f, "Hz", PS::Log, nullptr, "Auto-pan" },
    { "depth", "Depth", 0.f, 100.f, 70.f, "%", PS::Linear, nullptr, "Auto-pan" },
    { "shape", "Shape", 0, 3, 0, "", PS::Choice, "Sine|Triangle|Saw|Square", "Auto-pan" },
};
static const ParamInfo kTremolo[] = {
    { "sync", "Sync", 0, 13, 0, "", PS::Choice, SYNCFREE, "Tremolo" },
    { "rate", "Rate (Free)", 0.1f, 20.f, 5.f, "Hz", PS::Log, nullptr, "Tremolo" },
    { "depth", "Depth", 0.f, 100.f, 50.f, "%", PS::Linear, nullptr, "Tremolo" },
    { "shape", "Shape", 0, 3, 0, "", PS::Choice, "Sine|Triangle|Saw|Square", "Tremolo" },
    { "stereo", "Stereo Phase", 0.f, 180.f, 0.f, "deg", PS::Linear, nullptr, "Tremolo" },
};
static const ParamInfo kTransient[] = {
    { "attack", "Attack", -100.f, 100.f, 30.f, "%", PS::Linear, nullptr, "Transient" },
    { "sustain", "Sustain", -100.f, 100.f, 0.f, "%", PS::Linear, nullptr, "Transient" },
    { "output", "Output", -12.f, 12.f, 0.f, "dB", PS::Linear, nullptr, "Transient" },
};

namespace Catalog {

const std::vector<EngineInfo>& engines() {
    static const std::vector<EngineInfo> e = {
        { "studio", "Studio Synth", "Sound-design synth: 3 oscillators (wavetable, supersaw, FM, sync, ring), 2 multimode filters, 3 envelopes, 3 LFOs, 8-slot mod matrix, macros." },
        { "analog", "Subtractive Synth", "Two band-limited oscillators, sub, noise, unison, multimode filter with envelope, LFO." },
        { "fm", "FM Synth", "Three-operator FM stack with feedback: bells, electric pianos, basses, mallets." },
        { "wavetable", "Wavetable Synth", "Morphing band-limited wavetables with position envelope/LFO and filter." },
        { "piano", "Piano", "Modelled piano: inharmonic partials, hammer noise, string detune, frequency-dependent decay." },
        { "epiano", "Electric Piano", "Tine electric piano (Rhodes-style) with bark and stereo tremolo." },
        { "organ", "Organ", "Nine-drawbar tonewheel organ with percussion, key click and rotary speaker." },
        { "string", "Plucked Strings", "Physical string model: acoustic, electric, bass and nylon guitars." },
        { "ensemble", "Ensemble", "Strings, brass and choir sections with ensemble detune, formants and vibrato." },
        { "sampler", "Sampler", "Plays any audio sample across the keyboard with envelope and filter." },
        { "drums", "Drum Kit", "16-pad drum machine with per-pad tune, gain, pan, length, filter and choke groups." },
    };
    return e;
}

juce::String engineName(const juce::String& engine) {
    for (auto& e : engines()) if (engine == e.id) return e.name;
    return engine;
}

ParamTable engineParams(const juce::String& e) {
    if (e == "fm") return T(kFM);
    if (e == "wavetable") return T(kWavetable);
    if (e == "piano") return T(kPiano);
    if (e == "epiano") return T(kEPiano);
    if (e == "organ") return T(kOrgan);
    if (e == "string") return T(kString);
    if (e == "ensemble") return T(kEnsemble);
    if (e == "sampler") return T(kSampler);
    if (e == "drums") return T(kDrums);
    if (e == "studio") return T(kStudio);
    return T(kAnalog);
}
ParamTable arpParams() { return T(kArp); }
ParamTable channelParams() { return T(kChannel); }
ParamTable drumPadParams() { return T(kDrumPad); }
ParamTable inputParams() { return T(kInput); }

const std::vector<InstrumentPreset>& instrumentPresets() {
    static const std::vector<InstrumentPreset> p = {
        // Studio Synth (sound design)
        { "Studio Synth", "Synths", "studio", "" },
        { "Studio Supersaw Lead", "Synths", "studio", "o1_wave=6,o1_det=45,o1_spread=100,o2_on=1,o2_wave=1,o2_oct=1,o2_level=35,f1_type=1,f1_cut=6500,f1_res=10,f1_env=15,mode=1,glide=40,m1_src=3,m1_dst=1,m1_amt=2,l1_delay=0.3,l1_fade=0.5,level=-9" },
        { "Studio Reese Bass", "Synths", "studio", "o1_uni=2,o1_det=30,o2_on=1,o2_wave=1,o2_fine=14,o2_level=80,sub_level=50,f1_type=1,f1_cut=700,f1_res=20,f1_env=10,a_s=100,a_r=0.1,mode=2,glide=30,m1_src=4,m1_dst=17,m1_amt=12,l2_rate=0.3,level=-6" },
        { "Studio Sub Bass", "Synths", "studio", "o1_wave=0,o2_on=1,o2_wave=3,o2_level=25,f1_type=1,f1_cut=500,f1_res=0,f1_env=0,a_a=0.003,a_s=100,a_r=0.08,mode=1,sh_drive=20,level=-4" },
        { "Studio Evolving Pad", "Synths", "studio", "o1_wave=7,o1_table=2,o1_pos=20,o1_uni=3,o1_det=20,o2_on=1,o2_wave=1,o2_uni=4,o2_det=30,o2_level=40,o2_oct=-1,f1_type=0,f1_cut=2500,f1_res=15,f1_env=10,a_a=1.5,a_d=1,a_s=85,a_r=3,e2_a=2,e2_d=3,m1_src=4,m1_dst=8,m1_amt=40,l2_rate=0.12,m2_src=5,m2_dst=17,m2_amt=8,l3_rate=0.2,level=-10" },
        { "Studio FM Bell", "Synths", "studio", "o1_wave=0,o1_level=80,o2_on=1,o2_wave=0,o2_oct=1,o2_semi=7,o2_level=0,fm=20,f1_type=0,f1_cut=20000,f1_env=0,a_a=0.001,a_d=3,a_s=0,a_r=2,e2_a=0.001,e2_d=1.2,e2_s=0,m1_src=1,m1_dst=12,m1_amt=45,m2_src=6,m2_dst=12,m2_amt=20,level=-8" },
        { "Studio Sync Lead", "Synths", "studio", "o1_wave=1,o1_level=0,o2_on=1,o2_wave=1,o2_level=80,sync=1,f1_type=1,f1_cut=7000,f1_env=0,e2_a=0.001,e2_d=0.6,e2_s=20,m1_src=1,m1_dst=3,m1_amt=24,m2_src=8,m2_dst=3,m2_amt=30,mode=1,glide=25,level=-9" },
        { "Studio Pluck", "Synths", "studio", "o1_wave=1,o2_on=1,o2_wave=2,o2_fine=8,o2_level=50,f1_type=1,f1_cut=400,f1_res=30,f1_env=70,e2_a=0.001,e2_d=0.25,e2_s=0,a_a=0.001,a_d=0.5,a_s=0,a_r=0.3,level=-7" },
        { "Studio Wobble Bass", "Synths", "studio", "o1_wave=1,o1_uni=3,o1_det=15,o2_on=1,o2_wave=2,o2_oct=-1,o2_level=60,f1_type=1,f1_cut=300,f1_res=45,f1_env=0,f1_drive=40,l1_sync=7,l1_mode=1,m1_src=3,m1_dst=17,m1_amt=55,a_s=100,mode=1,level=-6" },
        { "Studio Acid Bass", "Synths", "studio", "o1_wave=1,f1_type=1,f1_cut=350,f1_res=75,f1_env=60,f1_drive=30,e2_a=0.001,e2_d=0.2,e2_s=0,a_s=90,a_r=0.05,mode=2,glide=60,level=-6" },
        { "Studio Talking Formant", "Synths", "studio", "o1_wave=1,o1_uni=2,o1_det=10,f1_type=8,f1_cut=300,f1_res=60,f1_env=0,m1_src=4,m1_dst=17,m1_amt=40,l2_rate=0.6,l2_mode=1,a_s=90,level=-6" },
        { "Studio Comb Pluck", "Synths", "studio", "o1_on=0,noise_level=0,f1_type=7,f1_cut=261.63,f1_key=100,f1_res=94,f1_env=0,e2_a=0.001,e2_d=0.015,e2_s=0,m1_src=1,m1_dst=16,m1_amt=100,a_a=0.001,a_d=2.5,a_s=0,a_r=1,level=-4" },
        { "Studio Brass Stab", "Synths", "studio", "o1_wave=1,o1_uni=3,o1_det=12,o2_on=1,o2_wave=1,o2_fine=-7,o2_level=70,f1_type=1,f1_cut=900,f1_res=10,f1_env=55,f1_vel=40,e2_a=0.04,e2_d=0.35,e2_s=30,a_a=0.02,a_d=0.4,a_s=70,a_r=0.25,level=-8" },
        // Synths
        { "Subtractive Synth", "Synths", "analog", "" },
        { "FM Synth", "Synths", "fm", "" },
        { "Wavetable Synth", "Synths", "wavetable", "table=1,pos=30,pos_lfo=20,lfo_rate=0.3" },
        { "Analog Synth", "Synths", "analog", "osc1_wave=0,osc2_wave=1,osc2_fine=9,cutoff=3500,reso=25,flt_env=45,drive=20" },
        { "Bass Synth", "Synths", "analog", "osc1_wave=0,osc2_wave=1,osc2_semi=-12,sub=60,cutoff=700,reso=30,flt_env=55,f_d=0.25,f_s=10,a_s=90,a_r=0.08,mono=1,legato=1,glide=40,level=-4" },
        { "Lead Synth", "Synths", "analog", "osc1_wave=0,osc2_wave=0,osc2_fine=12,unison=3,uni_detune=25,cutoff=5000,reso=20,flt_env=25,lfo_pitch=25,lfo_rate=5.5,mono=1,glide=60" },
        { "Pad Synth", "Synths", "analog", "osc1_wave=0,osc2_wave=2,osc2_semi=12,unison=5,uni_detune=35,uni_width=100,cutoff=2200,reso=10,flt_env=20,f_a=1.5,f_d=2,a_a=1.2,a_d=1,a_s=85,a_r=2.5,lfo_cutoff=15,lfo_rate=0.25,level=-9" },
        { "Pluck Synth", "Synths", "analog", "osc1_wave=0,osc2_wave=1,osc2_fine=5,cutoff=900,reso=25,flt_env=70,f_d=0.18,f_s=0,a_a=0.001,a_d=0.45,a_s=0,a_r=0.3" },
        { "Wobble Bass", "Synths", "wavetable", "table=3,pos=20,pos_lfo=60,lfo_sync=7,cutoff=1200,reso=40,lfo_cutoff=60,mono=1,sub=50,level=-5" },
        { "FM Bass", "Synths", "fm", "ratio_m1=1,index_m1=4,dec_m1=0.25,sus_m1=10,feedback=20,a_d=0.5,a_s=70,a_r=0.08,mono=1,level=-4" },
        { "Supersaw", "Synths", "analog", "unison=7,uni_detune=45,uni_width=100,osc2_wave=0,osc2_semi=12,cutoff=9000,flt_env=0,a_r=0.6,level=-10" },
        // Keys
        { "Grand Piano", "Keys", "piano", "" },
        { "Upright Piano", "Keys", "piano", "model=1,bright=45,decay=5" },
        { "Electric Piano", "Keys", "fm", "ratio_c=1,ratio_m1=14,index_m1=1.3,dec_m1=0.35,sus_m1=0,ratio_m2=1,index_m2=1.6,dec_m2=1.5,a_d=2.5,a_s=0,a_r=0.35,vel_index=80,level=-5" },
        { "Rhodes-style Keys", "Keys", "epiano", "" },
        { "Organ", "Keys", "organ", "" },
        { "Gospel Organ", "Keys", "organ", "db16=8,db513=8,db8=8,db4=8,db223=6,db2=6,db135=4,db113=4,db1=8,rotary=2,drive=30" },
        // Other instruments
        { "Acoustic Guitar", "Instruments", "string", "" },
        { "Electric Guitar", "Instruments", "string", "model=1,drive=55,tone=4500,decay=4,body=20,level=-9" },
        { "Clean Electric", "Instruments", "string", "model=1,drive=5,tone=6000,decay=4,body=20" },
        { "Bass Guitar", "Instruments", "string", "model=2,bright=35,decay=3,tone=2500,body=60,mono=1,level=-3" },
        { "Nylon Guitar", "Instruments", "string", "model=3,bright=40,decay=3" },
        { "Strings", "Instruments", "ensemble", "" },
        { "Pizzicato Strings", "Instruments", "ensemble", "a_a=0.003,a_d=0.35,a_s=0,a_r=0.25,vib_depth=0,bright=60" },
        { "Brass", "Instruments", "ensemble", "model=1,a_a=0.05,a_d=0.3,a_s=80,a_r=0.25,bright=55,vib_depth=8,detune=25" },
        { "Choir", "Instruments", "ensemble", "model=2,a_a=0.4,a_r=1.0,vowel=0,breath=20,detune=50" },
        { "Bells", "Instruments", "fm", "ratio_c=1,ratio_m1=3.5,index_m1=3,dec_m1=2.5,sus_m1=0,ratio_m2=7,index_m2=0.6,dec_m2=1,a_a=0.001,a_d=4,a_s=0,a_r=2.5,level=-8" },
        { "Mallets", "Instruments", "fm", "ratio_c=1,ratio_m1=4,index_m1=2.2,dec_m1=0.08,sus_m1=0,a_a=0.001,a_d=0.9,a_s=0,a_r=0.5,level=-4" },
        { "Vibraphone", "Instruments", "fm", "ratio_c=1,ratio_m1=4,index_m1=1.2,dec_m1=0.15,sus_m1=0,a_d=3,a_s=0,a_r=1,lfo_amp=30,lfo_rate=5,level=-5" },
        // Drums
        { "Boom Kit", "Drums", "drums", "" },
        { "Trap Kit", "Drums", "drums", "" },
        { "Electro Kit", "Drums", "drums", "" },
        { "Studio Kit", "Drums", "drums", "" },
        // Sampler
        { "Sampler", "Sampler", "sampler", "" },
    };
    return p;
}

const InstrumentPreset* findPreset(const juce::String& name) {
    for (auto& p : instrumentPresets()) if (name == p.name) return &p;
    return nullptr;
}

const std::vector<EffectInfo>& effects() {
    static const std::vector<EffectInfo> e = {
        { "compressor", "Compressor", "Dynamics", "Feed-forward RMS/peak compressor with soft knee and parallel mix." },
        { "limiter", "Limiter", "Dynamics", "Look-ahead brickwall limiter." },
        { "gate", "Gate", "Dynamics", "Noise gate with hold and adjustable range." },
        { "expander", "Expander", "Dynamics", "Downward expander." },
        { "eq3", "3-Band EQ", "EQ", "Low shelf, sweepable mid, high shelf." },
        { "parametric", "Parametric EQ", "EQ", "Low/high cut, two shelves and two fully parametric bands." },
        { "highpass", "High-pass Filter", "EQ", "Resonant 12/24 dB high-pass." },
        { "lowpass", "Low-pass Filter", "EQ", "Resonant 12/24 dB low-pass." },
        { "bandpass", "Band-pass Filter", "EQ", "Resonant band-pass." },
        { "reverb", "Reverb", "Time", "8-line feedback delay network reverb." },
        { "delay", "Delay", "Time", "Tempo-synced stereo delay with tone control." },
        { "pingpong", "Ping-Pong Delay", "Time", "Delay bouncing between left and right." },
        { "echo", "Tape Echo", "Time", "Saturated, wobbling tape-style echo." },
        { "chorus", "Chorus", "Time", "Multi-voice stereo chorus." },
        { "flanger", "Flanger", "Time", "Short modulated delay with feedback." },
        { "phaser", "Phaser", "Time", "Swept all-pass phaser, 2-12 stages." },
        { "saturation", "Saturation", "Distortion", "Warm tanh saturation." },
        { "overdrive", "Overdrive", "Distortion", "Asymmetric amp-style overdrive." },
        { "distortion", "Distortion", "Distortion", "Hard clip, wavefold, fuzz and rectify." },
        { "bitcrusher", "Bitcrusher", "Distortion", "Bit depth and sample-rate reduction." },
        { "widener", "Stereo Widener", "Other", "Mid/side width with Haas delay and mono bass." },
        { "imager", "Stereo Imager", "Other", "Two-band width control and balance." },
        { "pitch", "Pitch Shifter", "Other", "Real-time pitch shifting (dual-window)." },
        { "autopan", "Auto-pan", "Other", "Tempo-synced panning." },
        { "tremolo", "Tremolo", "Other", "Amplitude modulation with stereo phase." },
        { "transient", "Transient Shaper", "Other", "Boost or soften attacks and sustain." },
        { "deesser", "De-esser", "Dynamics", "Tames harsh S and T sounds in vocals." },
        { "designer", "Effect Designer", "Other", "Build your own effect: six modules in a row (filters, drive, fold, crush, delay, chorus, ring mod, comb, reverb...) with LFOs and an envelope follower to move any knob." },
    };
    return e;
}

const EffectInfo* findEffect(const juce::String& id) {
    for (auto& e : effects()) if (id == e.id) return &e;
    return nullptr;
}

ParamTable effectParams(const juce::String& id) {
    if (id == "compressor") return T(kCompressor);
    if (id == "limiter") return T(kLimiter);
    if (id == "gate") return T(kGate);
    if (id == "expander") return T(kExpander);
    if (id == "eq3") return T(kEQ3);
    if (id == "parametric") return T(kParametric);
    if (id == "highpass") return T(kHPF);
    if (id == "lowpass") return T(kLPF);
    if (id == "bandpass") return T(kBPF);
    if (id == "reverb") return T(kReverb);
    if (id == "delay") return T(kDelay);
    if (id == "pingpong") return T(kPingPong);
    if (id == "echo") return T(kEcho);
    if (id == "chorus") return T(kChorus);
    if (id == "flanger") return T(kFlanger);
    if (id == "phaser") return T(kPhaser);
    if (id == "saturation") return T(kSaturation);
    if (id == "overdrive") return T(kOverdrive);
    if (id == "distortion") return T(kDistortion);
    if (id == "bitcrusher") return T(kBitcrusher);
    if (id == "widener") return T(kWidener);
    if (id == "imager") return T(kImager);
    if (id == "pitch") return T(kPitch);
    if (id == "autopan") return T(kAutoPan);
    if (id == "tremolo") return T(kTremolo);
    if (id == "transient") return T(kTransient);
    if (id == "deesser") return T(kDeEsser);
    if (id == "designer") return T(kDesigner);
    return { nullptr, 0 };
}

const std::vector<EffectPreset>& effectPresets() {
    static const std::vector<EffectPreset> p = {
        { "compressor", "Vocal Leveler", "threshold=-20,ratio=3,attack=5,release=80,knee=8,makeup=4" },
        { "compressor", "Drum Bus Glue", "threshold=-14,ratio=2,attack=30,release=150,knee=6,makeup=2" },
        { "compressor", "Parallel Smash", "threshold=-35,ratio=12,attack=1,release=60,makeup=12,mix=35" },
        { "limiter", "Master Loud", "gain=6,ceiling=-0.3,release=80" },
        { "limiter", "Safety", "gain=0,ceiling=-1,release=50" },
        { "gate", "Tight Drums", "threshold=-30,range=-60,attack=0.5,hold=20,release=60" },
        { "gate", "Vocal Cleanup", "threshold=-50,range=-25,attack=2,hold=80,release=200" },
        { "expander", "Soft Noise Reduction", "threshold=-50,ratio=2,range=-20" },
        { "eq3", "Warm", "low=3,mid=-2,high=-2" },
        { "eq3", "Bright", "low=-1,high=4,high_f=6000" },
        { "parametric", "Vocal Presence", "lc_on=1,lc_f=90,b2_f=300,b2_g=-3,b3_f=3500,b3_g=3,b4_g=2" },
        { "parametric", "Kick Punch", "b1_f=60,b1_g=4,b2_f=350,b2_g=-5,b3_f=4000,b3_g=3" },
        { "lowpass", "Muffled", "cutoff=600,reso=20" },
        { "highpass", "Thin Out", "cutoff=400,reso=10" },
        { "reverb", "Small Room", "size=25,decay=0.7,damping=60,predelay=5,mix=20" },
        { "reverb", "Large Hall", "size=90,decay=4.5,damping=40,predelay=35,mix=30" },
        { "reverb", "Plate", "size=55,decay=2,damping=20,predelay=10,mix=25" },
        { "reverb", "Send (100% wet)", "size=70,decay=2.8,mix=100" },
        { "delay", "Quarter Note", "sync=4,feedback=35,mix=25" },
        { "delay", "Dotted Eighth", "sync=8,feedback=45,mix=30" },
        { "delay", "Send (100% wet)", "sync=8,feedback=40,mix=100" },
        { "pingpong", "Wide Eighths", "sync=7,feedback=45,mix=30" },
        { "echo", "Slapback", "time=110,feedback=10,mix=35" },
        { "echo", "Dub Echo", "sync=8,feedback=85,wow=50,sat=60,mix=40" },
        { "chorus", "Lush", "rate=0.5,depth=70,voices=3,mix=55" },
        { "flanger", "Jet", "rate=0.12,depth=90,feedback=80" },
        { "phaser", "Slow Swirl", "rate=0.2,stages=8,feedback=60" },
        { "saturation", "Tape Warmth", "drive=8,tone=10000,output=-4" },
        { "overdrive", "Crunch", "drive=60,tone=2500" },
        { "distortion", "Fuzz Box", "type=2,drive=30,tone=3000,output=-18" },
        { "bitcrusher", "8-bit Console", "bits=6,down=6" },
        { "widener", "Super Wide", "width=180,haas=8" },
        { "pitch", "Octave Down", "semis=-12" },
        { "pitch", "Fifth Up Blend", "semis=7,mix=40" },
        { "autopan", "Eighth Note Pan", "sync=7,depth=80" },
        { "tremolo", "Surf Tremolo", "rate=6,depth=60" },
        { "transient", "Punchy", "attack=60,sustain=-20" },
        { "transient", "Softer Attacks", "attack=-50,sustain=10" },
        { "deesser", "Gentle", "freq=6500,threshold=-26,range=6" },
        { "deesser", "Strong", "freq=5500,threshold=-34,range=14" },
        { "designer", "Auto-Wah", "s1_type=1,s1_a=35,s1_b=70,s1_c=10,s2_type=0,m1_src=3,m1_dst=1,m1_amt=55,ef_gain=18" },
        { "designer", "Lo-fi Radio", "s1_type=2,s1_a=55,s1_b=20,s2_type=1,s2_a=72,s2_b=25,s3_type=8,s3_a=45,s3_b=25,s3_c=60,s4_type=6,s4_a=35,s4_b=50" },
        { "designer", "Space Echo Wash", "s1_type=9,s1_a=62,s1_b=55,s1_c=45,s1_mix=45,s2_type=10,s2_a=20,s2_b=50,s3_type=14,s3_a=70,s3_b=40,s3_c=80,s3_mix=35,m1_src=2,m1_dst=1,m1_amt=4,l2_rate=0.3" },
        { "designer", "Gated Stutter", "s1_type=15,s1_a=83,s1_b=100,s1_c=100,s2_type=0" },
        { "designer", "Metallic Comb", "s1_type=12,s1_a=40,s1_b=80,s1_c=30,s1_mix=60,m1_src=1,m1_dst=1,m1_amt=20,l1_rate=0.2,s2_type=0" },
        { "designer", "Fuzz Sweep", "s1_type=7,s1_a=55,s1_b=20,s1_c=70,s2_type=1,s2_a=60,s2_b=20,m1_src=1,m1_dst=5,m1_amt=30,l1_rate=0.4" },
    };
    return p;
}

const std::vector<DrumSoundInfo>& drumSounds() {
    static const std::vector<DrumSoundInfo> d = {
        { "kick_boom", "Boom Kick", "Kick" }, { "kick_punch", "Punch Kick", "Kick" }, { "kick_808", "Long Sub Kick", "Kick" }, { "kick_acoustic", "Studio Kick", "Kick" },
        { "snare_tight", "Tight Snare", "Snare" }, { "snare_fat", "Fat Snare", "Snare" }, { "snare_rim", "Rim Shot", "Snare" }, { "snare_acoustic", "Studio Snare", "Snare" },
        { "clap", "Clap", "Clap" }, { "clap_big", "Big Clap", "Clap" }, { "snap", "Finger Snap", "Clap" },
        { "hat_closed", "Closed Hat", "Hi-hat" }, { "hat_closed_tight", "Tight Hat", "Hi-hat" }, { "hat_open", "Open Hat", "Hi-hat" }, { "hat_pedal", "Pedal Hat", "Hi-hat" },
        { "tom_low", "Low Tom", "Tom" }, { "tom_mid", "Mid Tom", "Tom" }, { "tom_high", "High Tom", "Tom" },
        { "crash", "Crash", "Cymbal" }, { "ride", "Ride", "Cymbal" }, { "ride_bell", "Ride Bell", "Cymbal" }, { "china", "Trash Cymbal", "Cymbal" },
        { "shaker", "Shaker", "Percussion" }, { "cowbell", "Cowbell", "Percussion" }, { "clave", "Clave", "Percussion" }, { "conga_hi", "Conga High", "Percussion" },
        { "conga_lo", "Conga Low", "Percussion" }, { "tamb", "Tambourine", "Percussion" }, { "woodblock", "Woodblock", "Percussion" }, { "triangle", "Triangle", "Percussion" },
        { "zap", "Zap", "FX" }, { "noise_sweep", "Riser Noise", "FX" },
    };
    return d;
}

const std::vector<std::pair<juce::String, juce::StringArray>>& drumKits() {
    static const std::vector<std::pair<juce::String, juce::StringArray>> k = {
        { "Boom Kit", { "kick_boom", "snare_fat", "clap", "hat_closed", "hat_open", "tom_low", "tom_mid", "tom_high",
                        "crash", "ride", "shaker", "cowbell", "snare_rim", "conga_hi", "tamb", "snap" } },
        { "Trap Kit", { "kick_808", "snare_tight", "clap_big", "hat_closed_tight", "hat_open", "tom_low", "tom_mid", "tom_high",
                        "crash", "ride_bell", "snap", "cowbell", "snare_rim", "shaker", "zap", "noise_sweep" } },
        { "Electro Kit", { "kick_punch", "snare_tight", "clap", "hat_closed", "hat_open", "tom_low", "tom_mid", "tom_high",
                           "china", "ride", "clave", "cowbell", "snare_rim", "zap", "shaker", "woodblock" } },
        { "Studio Kit", { "kick_acoustic", "snare_acoustic", "clap", "hat_closed", "hat_open", "tom_low", "tom_mid", "tom_high",
                          "crash", "ride", "hat_pedal", "ride_bell", "snare_rim", "tamb", "conga_lo", "triangle" } },
    };
    return k;
}

const std::vector<ChainPreset>& chainPresets() {
    static const std::vector<ChainPreset> c = {
        { "Clean Vocal", "Vocals", { { "highpass", "cutoff=80,reso=5" }, { "compressor", "threshold=-20,ratio=3,attack=5,release=80,knee=8,makeup=4" },
                                     { "deesser", "freq=6500,threshold=-26,range=6" }, { "parametric", "b2_f=300,b2_g=-2,b3_f=3500,b3_g=2" } } },
        { "Pop Vocal", "Vocals", { { "highpass", "cutoff=100,reso=5" }, { "compressor", "threshold=-24,ratio=4,attack=3,release=60,knee=6,makeup=6" },
                                   { "deesser", "freq=6500,threshold=-30,range=8" }, { "parametric", "b2_f=250,b2_g=-3,b3_f=4000,b3_g=3,b4_f=10000,b4_g=3" },
                                   { "delay", "sync=8,feedback=25,tone=5000,mix=12" }, { "reverb", "size=55,decay=1.8,damping=40,predelay=25,mix=18" } } },
        { "Rap Vocal", "Vocals", { { "highpass", "cutoff=110,reso=5" }, { "compressor", "threshold=-26,ratio=6,attack=2,release=50,knee=4,makeup=8" },
                                   { "deesser", "freq=6000,threshold=-30,range=10" }, { "parametric", "b2_f=350,b2_g=-3,b3_f=3000,b3_g=4" },
                                   { "saturation", "drive=4,tone=12000,output=-2" }, { "reverb", "size=25,decay=0.8,damping=60,predelay=10,mix=10" } } },
        { "Radio Voice", "Vocals", { { "bandpass", "cutoff=1500,reso=20" }, { "overdrive", "drive=35,tone=2500,output=-6" }, { "compressor", "threshold=-25,ratio=8,attack=1,release=50,makeup=6" } } },
        { "Podcast", "Vocals", { { "highpass", "cutoff=80,reso=5" }, { "gate", "threshold=-50,range=-25,attack=2,hold=80,release=200" },
                                 { "compressor", "threshold=-22,ratio=3,attack=8,release=120,knee=10,makeup=5" }, { "deesser", "freq=6500,threshold=-28,range=6" },
                                 { "limiter", "gain=2,ceiling=-1" } } },
        { "Airy Ballad", "Vocals", { { "highpass", "cutoff=90,reso=5" }, { "compressor", "threshold=-22,ratio=3,attack=8,release=100,makeup=4" },
                                     { "parametric", "b4_f=12000,b4_g=4,b2_f=300,b2_g=-2" }, { "chorus", "rate=0.3,depth=25,voices=2,mix=15" },
                                     { "reverb", "size=85,decay=3.5,damping=30,predelay=40,mix=28" } } },
        { "Lo-fi Vocal", "Vocals", { { "bandpass", "cutoff=1200,reso=15" }, { "bitcrusher", "bits=10,down=3,mix=60" }, { "echo", "time=180,feedback=30,wow=60,sat=50,mix=25" } } },
        { "Acoustic Guitar", "Instruments", { { "highpass", "cutoff=70,reso=5" }, { "compressor", "threshold=-20,ratio=2.5,attack=15,release=120,makeup=3" },
                                              { "eq3", "low=-2,mid=-1,high=3" }, { "reverb", "size=40,decay=1.2,mix=15" } } },
        { "Electric Crunch", "Instruments", { { "overdrive", "drive=60,tone=2800,output=-8" }, { "eq3", "low=1,mid=2,mid_f=900,high=-2" }, { "reverb", "size=30,decay=0.9,mix=12" } } },
        { "Electric Clean", "Instruments", { { "compressor", "threshold=-18,ratio=3,attack=10,release=100,makeup=3" }, { "chorus", "rate=0.6,depth=35,mix=30" },
                                             { "delay", "sync=8,feedback=25,mix=15" }, { "reverb", "size=45,decay=1.6,mix=18" } } },
        { "Bass DI", "Instruments", { { "compressor", "threshold=-22,ratio=4,attack=20,release=150,makeup=4" }, { "eq3", "low=3,low_f=90,mid=-2,mid_f=500,high=1" }, { "saturation", "drive=3,output=-2" } } },
        { "Keys / Synth In", "Instruments", { { "compressor", "threshold=-16,ratio=2,attack=20,release=150,makeup=2" }, { "widener", "width=130" } } },
        { "Raw (no effects)", "Instruments", {} },
    };
    return c;
}

// ---- Effect Designer knob meaning: shared by the DSP and the editor so labels never lie.
const char* designerKnobName(int type, int k) {
    static const char* names[16][3] = {
        { "-", "-", "-" },
        { "Cutoff", "Resonance", "Drive" }, { "Cutoff", "Resonance", "Drive" }, { "Centre", "Resonance", "Drive" }, { "Centre", "Width", "Drive" },
        { "Frequency", "Gain", "Q" },
        { "Drive", "Tone", "Asymmetry" }, { "Fold", "Bias", "Tone" }, { "Bits", "Downsample", "Tone" },
        { "Time", "Feedback", "Tone" }, { "Rate", "Depth", "Delay" }, { "Frequency", "Shape", "Stereo Offset" },
        { "Pitch", "Feedback", "Damping" }, { "Gain", "Pan", "Width" }, { "Size", "Damping", "Width" }, { "Rate", "Depth", "Shape" },
    };
    return names[juce::jlimit(0, 15, type)][juce::jlimit(0, 2, k)];
}

void designerDefaults(int type, float out[4]) {
    static const float d[16][4] = {
        { 50, 50, 50, 100 }, { 60, 20, 0, 100 }, { 30, 20, 0, 100 }, { 50, 30, 0, 100 }, { 50, 30, 0, 100 }, { 50, 65, 40, 100 },
        { 40, 70, 0, 100 }, { 30, 50, 70, 100 }, { 40, 10, 80, 100 }, { 72, 40, 70, 35 }, { 40, 50, 30, 50 }, { 50, 0, 0, 100 },
        { 50, 75, 30, 60 }, { 50, 50, 50, 100 }, { 60, 40, 80, 30 }, { 70, 50, 0, 100 },
    };
    for (int k = 0; k < 4; ++k) out[k] = d[juce::jlimit(0, 15, type)][k];
}

float designerValue(int type, int k, float v) {
    v = juce::jlimit(0.f, 1.f, v);
    switch (type) {
        case 1: case 2: case 3: case 4: return k == 0 ? 20.f * std::pow(1000.f, v) : (k == 1 ? v : v * 24.f);
        case 5: return k == 0 ? 20.f * std::pow(1000.f, v) : (k == 1 ? (v * 2.f - 1.f) * 18.f : 0.3f * std::pow(8.f / 0.3f, v));
        case 6: return k == 0 ? v * 36.f : (k == 1 ? 500.f * std::pow(40.f, v) : v);
        case 7: return k == 0 ? 1.f + v * 9.f : (k == 1 ? v * 2.f - 1.f : 500.f * std::pow(40.f, v));
        case 8: return k == 0 ? 16.f - v * 15.f : (k == 1 ? 1.f + v * 31.f : 500.f * std::pow(40.f, v));
        case 9: return k == 0 ? std::pow(2000.f, v) : (k == 1 ? v * 0.95f : 500.f * std::pow(40.f, v));
        case 10: return k == 0 ? 0.05f * std::pow(100.f, v) : (k == 1 ? v : 5.f + v * 25.f);
        case 11: return k == 0 ? 20.f * std::pow(250.f, v) : (k == 1 ? v : v * 180.f);
        case 12: return k == 0 ? 20.f * std::pow(100.f, v) : (k == 1 ? (v * 2.f - 1.f) * 0.95f : v);
        case 13: return k == 0 ? (v * 2.f - 1.f) * 24.f : (k == 1 ? v * 2.f - 1.f : v * 2.f);
        case 14: return v;
        case 15: return k == 0 ? 0.1f * std::pow(200.f, v) : v;
        default: return v;
    }
}

juce::String designerFormat(int type, int k, float v01) {
    const float v = designerValue(type, k, v01);
    auto hz = [](float f) { return f >= 1000.f ? juce::String(f / 1000.f, 2) + " kHz" : juce::String(f, f < 10.f ? 2 : (f < 100.f ? 1 : 0)) + " Hz"; };
    auto pct = [](float f) { return juce::String(juce::roundToInt(f * 100.f)) + "%"; };
    if (type == 0) return "-";
    switch (type) {
        case 1: case 2: case 3: case 4: return k == 0 ? hz(v) : (k == 1 ? pct(v) : juce::String(v, 1) + " dB");
        case 5: return k == 0 ? hz(v) : (k == 1 ? juce::String(v, 1) + " dB" : juce::String(v, 2));
        case 6: return k == 0 ? juce::String(v, 1) + " dB" : (k == 1 ? hz(v) : pct(v));
        case 7: return k == 0 ? juce::String(v, 1) + "x" : (k == 1 ? pct(v) : hz(v));
        case 8: return k == 0 ? juce::String(juce::roundToInt(v)) + " bits" : (k == 1 ? juce::String(juce::roundToInt(v)) + "x" : hz(v));
        case 9: return k == 0 ? (v >= 1000.f ? juce::String(v / 1000.f, 2) + " s" : juce::String(v, v < 10.f ? 1 : 0) + " ms") : (k == 1 ? pct(v) : hz(v));
        case 10: return k == 0 ? hz(v) : (k == 1 ? pct(v) : juce::String(v, 1) + " ms");
        case 11: return k == 0 ? hz(v) : (k == 1 ? pct(v) : juce::String(juce::roundToInt(v)) + " deg");
        case 12: return k == 0 ? hz(v) : pct(v);
        case 13: return k == 0 ? juce::String(v, 1) + " dB" : (k == 1 ? (std::abs(v) < 0.01f ? juce::String("C") : juce::String(juce::roundToInt(std::abs(v) * 100.f)) + (v < 0 ? " L" : " R")) : pct(v));
        case 15: return k == 0 ? hz(v) : pct(v);
        default: return pct(v);
    }
}

// ---- sound design helpers (Randomize / Mutate in the instrument editor)
void randomizeSound(ParamSet& ps, const juce::String& engine, float amount, uint32_t seed) {
    juce::Random rng((juce::int64)seed * 7919 + 17);
    static const juce::StringArray keep { "level", "vel_sens", "bend", "transpose", "voices", "mode", "mono", "legato", "glide",
                                          "sh_fine", "sh_hp", "sh_lp", "sh_drive", "sh_width", "mac1", "mac2", "mac3", "mac4",
                                          "tune", "fine", "loop", "reverse", "keytrack", "start" };
    const bool full = amount >= 0.99f;
    for (int i = 0; i < ps.size(); ++i) {
        const auto& m = ps.meta(i);
        if (keep.contains(m.id)) continue;
        const bool discrete = m.scale == ParamScale::Choice || m.scale == ParamScale::Bool || m.scale == ParamScale::Int;
        if (discrete) {
            if (full || rng.nextFloat() < amount * 0.5f) ps.set(i, (float)rng.nextInt(juce::Range<int>((int)m.min, (int)m.max + 1)));
        } else {
            float n = ps.toNorm(i, ps.getBase(i));
            n = full ? rng.nextFloat() : n + (rng.nextFloat() * 2.f - 1.f) * amount;
            ps.set(i, ps.fromNorm(i, juce::jlimit(0.f, 1.f, n)));
        }
    }
    // keep it playable: audible, not endless, not ear-splitting, not wildly out of tune
    auto clampP = [&ps](const char* id, float lo, float hi) { const int i = ps.indexOf(id); if (i >= 0) ps.set(i, juce::jlimit(lo, hi, ps.getBase(i))); };
    auto setP = [&ps](const char* id, float v) { const int i = ps.indexOf(id); if (i >= 0) ps.set(i, v); };
    clampP("a_a", 0.001f, 0.6f); clampP("a_r", 0.01f, 2.5f); clampP("a_d", 0.1f, 4.f); clampP("a_s", 30.f, 100.f);
    clampP("reso", 0.f, 70.f); clampP("cutoff", 300.f, 20000.f); clampP("noise", 0.f, 40.f); clampP("lfo_pitch", 0.f, 40.f);
    clampP("osc2_semi", -12.f, 12.f);
    if (engine == "studio") {
        setP("o1_on", 1.f); clampP("o1_level", 50.f, 100.f);
        clampP("f1_res", 0.f, 80.f); clampP("f2_res", 0.f, 80.f); clampP("noise_level", 0.f, 35.f); clampP("fm", 0.f, 60.f);
        for (int f = 1; f <= 2; ++f) {
            const juce::String id = "f" + juce::String(f);
            const int ti = ps.indexOf(id + "_type");
            const int type = ti >= 0 ? ps.getInt(ti) : 0;
            const int ci = ps.indexOf(id + "_cut");
            if (ci >= 0) {
                if (type <= 1) ps.set(ci, juce::jlimit(400.f, 20000.f, ps.getBase(ci)));       // low-pass: not muffled to silence
                else if (type <= 3) ps.set(ci, juce::jlimit(20.f, 1500.f, ps.getBase(ci)));    // high-pass: keep some body
            }
        }
        for (int o = 1; o <= 3; ++o) { clampP(("o" + juce::String(o) + "_oct").toRawUTF8(), -2.f, 1.f); clampP(("o" + juce::String(o) + "_uni").toRawUTF8(), 1.f, 5.f); }
        // modulation: big amounts on pitch destinations sound broken, so keep those subtle
        for (int k = 1; k <= 8; ++k) {
            const juce::String id = "m" + juce::String(k);
            const int di = ps.indexOf(id + "_dst"), ai = ps.indexOf(id + "_amt");
            if (di < 0 || ai < 0) continue;
            const int dst = ps.getInt(di);
            const float lim = (dst >= 1 && dst <= 4) ? 3.f : 50.f;
            ps.set(ai, juce::jlimit(-lim, lim, ps.getBase(ai)));
        }
        clampP("e2_r", 0.01f, 3.f); clampP("e3_r", 0.01f, 3.f);
    }
}

ParamSetPtr makeParams(ParamTable t) { return std::make_shared<ParamSet>(t.infos, t.count); }

void applyValues(ParamSet& ps, const juce::String& values) {
    for (auto& kv : juce::StringArray::fromTokens(values, ",", "")) {
        const int i = ps.indexOf(kv.upToFirstOccurrenceOf("=", false, false).trim());
        if (i >= 0) ps.set(i, kv.fromFirstOccurrenceOf("=", false, false).getFloatValue());
    }
}

}  // namespace Catalog
