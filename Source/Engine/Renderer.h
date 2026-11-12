// The render core shared by real-time playback and offline export.
//
// Threading model:
//  * The message thread owns the Project. Whenever it changes, Compiler builds an immutable
//    Snapshot (sorted note events, audio clip descriptors, automation, routing order) and
//    references long-lived processors (instruments, effects, channel state) from a registry.
//  * The audio thread picks up new snapshots through a lock-free pointer swap and never
//    allocates, locks or frees; retired snapshots are deleted back on the message thread.
//  * Knob values travel through the shared ParamSets (atomics), so they do not need a rebuild.
#pragma once
#include "Core/Project.h"
#include "Effects.h"
#include "Instruments.h"
#include "Samples.h"
#include "Dsp.h"
#include <atomic>
#include <map>
#include <set>

constexpr int kMaxTracks = 256;

// ---------------------------------------------------------------- compiled data
struct CompiledNote {
    int start, end;
    int pitch;
    float velocity, detune;
};

constexpr double kGrainSec = 0.07, kGrainHop = kGrainSec * 0.5;   // time-stretch grains

struct CompiledAudioClip {
    SamplePtr sample;
    std::shared_ptr<const std::vector<float>> grainShift;   // WSOLA alignment per grain (seconds)
    int startTick, endTick;
    double startSec, endSec;
    double offsetSec;
    float gain;
    double fadeIn, fadeOut;
    bool reverse, loop, keepPitch;
    float pitchRatio, stretch;
};

struct CompiledLane {
    ParamSetPtr params;
    int index = -1;
    AutomationLane lane;
};

// Long-lived per-track processors (reused across snapshots while compatible).
struct InstrumentProc {
    std::unique_ptr<Instrument> inst;
    ParamSetPtr arp;
    juce::String signature;
    // runtime note bookkeeping (audio thread only)
    struct Active { int pitch; double endTick; };
    std::array<Active, 256> active {};
    int numActive = 0;
    std::array<int, 64> held {};      // arpeggiator held notes (as played order)
    int numHeld = 0;
    std::array<double, 64> heldEnd {};
    double arpClock = 0.0;
    int arpStep = 0, arpNote = -1;
    double arpOffTick = 0.0;
    uint32_t arpRng = 12345;
};

struct FxSlot {
    std::unique_ptr<Effect> fx;
    std::shared_ptr<std::atomic<bool>> bypass;
    juce::String id, type;
    const ParamSet* paramsPtr = nullptr;
    float wet = 1.f;    // bypass crossfade state
};

struct ChannelProc {
    dsp::Smoothed gl, gr;
    std::atomic<float> peakL { 0.f }, peakR { 0.f };
    std::atomic<bool> clipped { false };
    std::atomic<int> voices { 0 };
    float busL[dsp::kQuantum] {}, busR[dsp::kQuantum] {};   // inputs from children / sends
    float monoState = 0.f;
    // Live input of an armed audio track (after input gain, before its input effects).
    std::atomic<float> inPeakL { 0.f }, inPeakR { 0.f };
    static constexpr int kRing = 4096;
    std::array<std::atomic<float>, kRing> inRing {};        // mono input history for the tuner
    std::atomic<int> inRingPos { 0 };
    // Instrument output history for the synth scope (pre-fader, post-instrument).
    std::array<std::atomic<float>, 2048> scope {};
    std::atomic<int> scopePos { 0 };
};

struct TrackRender {
    juce::String id;
    uint32_t uid = 0;
    TrackType type = TrackType::Instrument;
    std::shared_ptr<InstrumentProc> inst;
    std::vector<std::shared_ptr<FxSlot>> fx;
    std::vector<std::shared_ptr<FxSlot>> inFx;  // recording input chain (audio tracks)
    ParamSetPtr input;                          // input gain / monitor mode / record mode
    std::shared_ptr<ChannelProc> ch;
    ParamSetPtr channel;
    std::vector<CompiledNote> notes;            // sorted by start
    std::vector<CompiledAudioClip> clips;       // sorted by start
    std::vector<CompiledLane> lanes;
    int out = -1;                               // index of bus track, -1 = master
    int sends[4] { -1, -1, -1, -1 };            // index of return tracks
    bool armed = false, stereoIn = false;
    int inputCh = 0;
    std::vector<int> soloLinks;                 // indices whose solo keeps this track audible
};

struct Snapshot {
    std::vector<TrackRender> tracks;
    std::vector<int> order;                     // processing order (children before buses)
    TrackRender master;
    std::vector<TempoPoint> tempo;
    std::vector<TimeSigPoint> timeSig;
    bool loopOn = false;
    int loopStart = 0, loopEnd = PPQ * 16;
    int songEnd = 0;
    double sampleRate = 48000.0;
    uint64_t generation = 0;

    double bpmAt(double tick) const;
    double ticksToSeconds(double tick) const;
    double secondsToTicks(double sec) const;
    int barToTick(int bar) const;
    int tickToBar(int tick) const;
    int ticksPerBarAt(int tick) const;
    int beatTicksAt(int tick) const;
};

// Registry of reusable processors, owned by whoever owns the renderer (engine or exporter).
class ProcRegistry {
public:
    std::map<juce::String, std::shared_ptr<InstrumentProc>> instruments;
    std::map<juce::String, std::shared_ptr<FxSlot>> effects;
    std::map<juce::String, std::shared_ptr<ChannelProc>> channels;
    double sampleRate = 0.0;
    void clear() { instruments.clear(); effects.clear(); channels.clear(); }
};

// Waveform-similarity (WSOLA) alignment for pitch-preserving stretch, cached per clip setting.
std::shared_ptr<const std::vector<float>> grainShifts(const SamplePtr& s, double offsetSec, double speed, double pitch,
                                                      bool reverse, bool loop, double clipDurSec);

namespace Compiler {
// blockingSamples: load samples synchronously (export); otherwise request asynchronously.
std::unique_ptr<Snapshot> compile(const Project& p, ProcRegistry& reg, double sampleRate, bool blockingSamples);
uint32_t uidFor(const juce::String& id);
}

// ---------------------------------------------------------------- live events
struct LiveEvent {
    enum Type : uint8_t { NoteOn, NoteOff, PitchBend, ModWheel, Sustain, AllOff } type;
    uint32_t trackUid;
    int note;
    float value;
};

// Recorded MIDI from the audio thread (message thread turns these into notes).
struct RecordedNote {
    uint32_t trackUid;
    int pitch;
    float velocity;
    double tick;
    bool on;
};

// Single-producer single-consumer queue (lock-free, fixed capacity).
template <typename T, int N>
class SpscQueue {
public:
    bool push(const T& v) {
        int s1, z1, s2, z2;
        fifo.prepareToWrite(1, s1, z1, s2, z2);
        if (z1 + z2 == 0) return false;
        data[(size_t)(z1 ? s1 : s2)] = v;
        fifo.finishedWrite(1);
        return true;
    }
    int size() const { return fifo.getNumReady(); }
    bool pop(T& v) {
        int s1, z1, s2, z2;
        fifo.prepareToRead(1, s1, z1, s2, z2);
        if (z1 + z2 == 0) return false;
        v = data[(size_t)(z1 ? s1 : s2)];
        fifo.finishedRead(1);
        return true;
    }
private:
    juce::AbstractFifo fifo { N };
    std::array<T, N> data {};
};

// ---------------------------------------------------------------- renderer
class Renderer {
public:
    Renderer();
    void prepare(double sampleRate);
    void setSnapshot(Snapshot* s);              // audio thread (or offline thread)
    Snapshot* snapshot() const { return snap; }

    // Transport (call from the rendering thread)
    void play();
    void stop();
    void seek(double tick);
    bool isPlaying() const { return playing; }
    double tick() const { return curTick; }
    void allNotesOff(bool hard);

    SpscQueue<LiveEvent, 1024> liveQueue;       // producer: UI thread
    SpscQueue<LiveEvent, 1024> midiQueue;       // producer: MIDI input thread
    SpscQueue<RecordedNote, 4096> recordedNotes;               // filled while midiRecording

    // Renders n frames. `in` may be null. Returns true if anything audible was produced.
    void render(float* outL, float* outR, int n, const float* const* in = nullptr, int numIn = 0);

    // Recording (set by engine)
    bool midiRecording = false;
    double punchInTick = 0.0;
    bool loopWrapped = false;                   // set when the loop jumped during this block
    // Called for every armed audio track while playing at/after the punch-in point, with the audio to
    // record: clean (after input gain) or with the track's input effects, depending on its setting.
    std::function<void(uint32_t trackUid, const float* L, const float* R, int n)> onTrackInput;
    static int inputChainLatency(const TrackRender& t);   // samples of latency the input effects add

    TransportInfo transport;
    int activeVoices = 0;
    bool metronome = false;                     // real-time only (never exported)
    // Battery saver: after 2 s of silence while stopped, skip all processing until something happens.
    std::atomic<bool> idleSleepEnabled { true };
    bool isIdle() const { return idle; }
    float metronomeGain = 0.5f;
    // Sample audition (browser): played on top of the mix, never exported.
    std::atomic<const SampleData*> previewSample { nullptr };
    double previewPos = 0.0;
    // Count-in: clicks for `beats` beats while the song waits, then playback starts.
    void startCountIn(int beats);
    bool isCountingIn() const { return countInLeft > 0; }

private:
    void renderSub(int offset, int n, const float* const* in, int numIn, float* outL, float* outR);
    void processInstrumentTrack(TrackRender& t, double t0, double t1, double tps, int n);
    void renderAudioClips(TrackRender& t, double sec0, int n, float* L, float* R);
    void applyAutomation(TrackRender& t, double tick);
    void channelStrip(TrackRender& t, float* L, float* R, int n, bool audible);
    void runInserts(std::vector<std::shared_ptr<FxSlot>>& chain, float* L, float* R, int n);
    void processInput(TrackRender& t, const float* const* in, int numIn, int offset, int n, float* L, float* R, double t0);
    float inL[dsp::kQuantum] {}, inR[dsp::kQuantum] {};
    void releaseSequenced(TrackRender& t, int offset);
    bool trackAudible(const TrackRender& t, bool anySolo) const;

    Snapshot* snap = nullptr;
    double sr = 48000.0;
    bool playing = false;
    double curTick = 0.0, curSec = 0.0;
    bool needsRelease = false;
    float bufL[kMaxTracks][dsp::kQuantum] {}, bufR[kMaxTracks][dsp::kQuantum] {};
    float mixL[dsp::kQuantum] {}, mixR[dsp::kQuantum] {};
    std::vector<float> clickHi, clickLo;
    const std::vector<float>* clickCur = nullptr;
    int clickPos = 0;
    void handleLive(const LiveEvent& e);
    int countInLeft = 0, countInBeatLen = 1, countInPos = 0, countInBeatsPerBar = 4;
    bool idle = false;
    double silentSeconds = 0.0;
};
