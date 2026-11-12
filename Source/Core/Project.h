// Song data model. Owned and edited on the message thread only; the audio engine
// receives compiled, immutable snapshots (see Engine/Snapshot.h) plus shared ParamSets.
#pragma once
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include "Engine/Params.h"
#include <array>
#include <vector>

constexpr int PPQ = 960;  // ticks per quarter note

struct Note {
    int start = 0, length = PPQ, pitch = 60;
    float velocity = 0.8f;
    bool selected = false;  // UI state, not saved
};

struct AudioClipData {
    juce::String file;           // "audio/xyz.wav" (project-relative) or "lib:<path>" or absolute
    juce::StringArray takes;     // alternative takes (files); `file` is the active one
    double offsetSec = 0.0;      // start position inside the source
    float gainDb = 0.f;
    double fadeInSec = 0.0, fadeOutSec = 0.0;
    bool reverse = false, loop = false, normalize = false;
    float pitchSt = 0.f;         // pitch shift (semitones)
    float stretch = 1.f;         // playback duration factor (2 = twice as long)
    bool keepPitch = true;       // stretch without changing pitch (granular)
};

enum class ClipType { Midi = 0, Audio = 1, Pattern = 2 };

struct Clip {
    juce::String id, name;
    ClipType type = ClipType::Midi;
    int start = 0, length = PPQ * 4;
    int contentLength = PPQ * 4;   // loop length of the content (MIDI); clip repeats it
    int contentOffset = 0;         // trim from the left (ticks into content)
    std::vector<Note> notes;
    juce::String patternId;
    AudioClipData audio;
    bool muted = false;
    bool selected = false;         // UI state
    int end() const { return start + length; }
};

struct DrumStep {
    bool on = false;
    float velocity = 0.8f;
    float probability = 1.f;
    int pitch = 0;      // semitone offset (per-step note editing)
    int ratchet = 1;    // repeats within the step
};

constexpr int kDrumPads = 16;
constexpr int kMaxSteps = 64;

struct Pattern {
    juce::String id, name;
    int steps = 16;
    int stepTicks = PPQ / 4;   // 1/16
    float swing = 0.f;         // 0..0.75 (fraction of a step that off-beats are delayed)
    std::array<std::array<DrumStep, kMaxSteps>, kDrumPads> grid {};
    juce::Colour colour { 0xffff8a3d };
    int lengthTicks() const { return steps * stepTicks; }
};

struct EffectData {
    juce::String id, type;
    juce::String name;             // optional display name ("My Wah"); empty = effect's own name
    ParamSetPtr params;
    std::shared_ptr<std::atomic<bool>> bypass = std::make_shared<std::atomic<bool>>(false);
};

struct DrumPadData {
    juce::String name, sample;   // sample: "builtin:<name>" or file path
    ParamSetPtr params;          // tune, gain, pan, decay, choke, mute
};

struct InstrumentData {
    juce::String engine = "analog";  // analog fm wavetable piano epiano organ string ensemble sampler drums
    juce::String preset = "Init";
    ParamSetPtr params;
    ParamSetPtr arp;                 // arpeggiator / chord-trigger MIDI effect
    juce::String sample;             // sampler engine source
    int sampleRoot = 60;
    std::array<DrumPadData, kDrumPads> pads;
};

struct AutomationPoint {
    int tick = 0;
    float value = 0.5f;    // normalized 0..1
    int curve = 0;         // 0 linear, 1 smooth, 2 hold/step
};

struct AutomationLane {
    juce::String target;   // "ch:vol" | "inst:cutoff" | "fx:<effectId>:<param>"
    std::vector<AutomationPoint> points;
    bool enabled = true;
    bool shown = true;
    float valueAt(int tick) const;
};

enum class TrackType { Instrument = 0, Drum = 1, Audio = 2, Bus = 3, Return = 4, Master = 5 };

struct Track {
    juce::String id, name;
    TrackType type = TrackType::Instrument;
    juce::Colour colour { 0xff4f8cff };
    std::vector<Clip> clips;
    InstrumentData inst;
    std::vector<EffectData> effects;
    ParamSetPtr channel;           // volume, pan, mute, solo, phase, mono, send1..4, sendPre1..4
    juce::String output = "master";// "master" or a Bus track id
    std::vector<AutomationLane> automation;
    bool armed = false, collapsed = false;
    int inputChannel = 0;          // first input channel for recording (stereo pair or mono)
    bool stereoInput = false;
    // Recording input stage (audio tracks): gain, monitoring mode, record clean or with effects,
    // and its own effect chain that runs on the live input before it is heard / recorded.
    ParamSetPtr input;
    std::vector<EffectData> inputEffects;
    enum Monitor { MonitorOff = 0, MonitorDirect = 1, MonitorThrough = 2 };
    int monitorMode() const { return input ? input->getInt(1) : MonitorOff; }
    bool recordsWithEffects() const { return input && input->getInt(3) == 1; }
    bool hasInstrument() const { return type == TrackType::Instrument || type == TrackType::Drum; }
    bool hasClips() const { return type == TrackType::Instrument || type == TrackType::Drum || type == TrackType::Audio; }
    AutomationLane* findLane(const juce::String& target);
};

struct TempoPoint { int tick = 0; double bpm = 120.0; };
struct TimeSigPoint { int bar = 0; int num = 4, den = 4; };
struct Marker { int tick = 0; juce::String name; bool section = false; juce::Colour colour { 0xff9aa0a6 }; };

class Project {
public:
    Project();

    // --- metadata / settings
    juce::String name = "Untitled";
    juce::String created, modified;
    int sampleRate = 48000;        // preferred device rate
    int bufferSize = 256;          // preferred device block size
    int lengthBars = 32;
    int key = 0;                   // 0 = C
    int scale = 0;                 // index into Theory scales
    juce::String customScale = "101011010101";
    int snap = 4;                  // index into snap table
    bool metronome = false;
    int countInBars = 1;
    bool loopOn = false;
    int loopStart = 0, loopEnd = PPQ * 16;

    std::vector<TempoPoint> tempo { TempoPoint {} };
    std::vector<TimeSigPoint> timeSig { TimeSigPoint {} };
    std::vector<Marker> markers;
    std::vector<Track> tracks;
    Track master;
    std::vector<Pattern> patterns;

    // --- transient (not saved): isolated preview of one pattern or clip, looped
    juce::String previewTrack, previewPattern, previewClip;

    // --- identity
    juce::String newId(const char* prefix);

    // --- lookup
    Track* findTrack(const juce::String& id);
    const Track* findTrack(const juce::String& id) const;
    int trackIndex(const juce::String& id) const;
    Pattern* findPattern(const juce::String& id);
    const Pattern* findPattern(const juce::String& id) const;
    std::vector<const Track*> returnTracks() const;

    // --- construction helpers
    Track& addTrack(TrackType type, const juce::String& engine = {}, const juce::String& preset = {}, int insertAt = -1);
    Pattern& addPattern(const juce::String& name = {});
    Clip makeMidiClip(int start, int length);
    void initInstrument(InstrumentData& inst, const juce::String& engine);
    EffectData makeEffect(const juce::String& type);

    // --- time
    double bpmAt(int tick) const;
    double ticksToSeconds(double tick) const;
    double secondsToTicks(double sec) const;
    TimeSigPoint timeSigAtBar(int bar) const;
    int barToTick(int bar) const;
    int tickToBar(int tick) const;           // bar index containing tick
    int ticksPerBarAt(int tick) const;
    juce::String formatPosition(double tick) const;  // "bar.beat.sixteenth"
    int songEndTick() const;                 // last clip end (0 if empty)

    // --- serialization (portable JSON)
    juce::var toVar() const;
    bool fromVar(const juce::var& v, juce::String& error);

    static juce::Colour trackColour(int i);

private:
    int idCounter = 1;
};

// Snap resolutions shared by arrangement / piano roll / automation.
struct SnapInfo { const char* name; int ticks; };
const std::vector<SnapInfo>& snapTable();   // index 0 = Off (ticks = 1)
int snapTicks(const Project& p, int snapIndex, int atTick);
int snapTick(const Project& p, int tick, int snapIndex, bool round = true);
