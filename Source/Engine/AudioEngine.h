// Real-time engine: audio device, transport, snapshot hand-off, recording, MIDI input.
#pragma once
#include "Renderer.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

class AudioEngine : private juce::AudioIODeviceCallback, private juce::MidiInputCallback, private juce::Timer {
public:
    AudioEngine();
    ~AudioEngine() override;

    void initialise(const juce::String& savedDeviceXml = {});
    void shutdown();
    juce::AudioDeviceManager& deviceManager() { return devices; }
    juce::String deviceStateXml() const;
    bool hasAudioDevice() const { return devices.getCurrentAudioDevice() != nullptr; }
    void enableInput(std::function<void(bool granted)> done);   // asks for mic permission where needed
    bool inputEnabled() const;

    // ---- project hand-off (message thread)
    void setProject(Project* p);
    void rebuild();                     // recompile now
    void markDirty() { dirty = true; }  // recompile on next timer tick (coalesces drags)

    // ---- transport (message thread)
    void play();
    void stop();
    void togglePlay() { isPlaying() ? stop() : play(); }
    void seek(double tick);
    void returnToStart() { seek(project ? (project->loopOn ? project->loopStart : 0) : 0); }
    bool isPlaying() const { return playingFlag.load(); }
    bool isCountingIn() const { return countingFlag.load(); }
    double playheadTick() const { return playhead.load(); }
    void setMetronome(bool on);

    // ---- recording
    bool startRecording(juce::String& error);
    void stopRecording();
    bool isRecording() const { return recording; }
    float inputLevel(int channel) const { return channel >= 0 && channel < 2 ? inputPeak[channel].load() : 0.f; }
    void resetInputLevels() { inputPeak[0] = 0.f; inputPeak[1] = 0.f; }
    double latencyCompensationMs = 0.0;           // user offset (Settings)
    double reportedLatencyMs() const;
    // Monitoring: microphone -> app -> headphones delay, and the smallest-buffer mode that shrinks it.
    double monitorLatencyMs() const;
    bool setLowLatency(bool on, juce::String& message);
    bool isLowLatency() const { return lowLatency; }
    int inputChainLatencySamples(const Track& t) const;
    std::pair<float, float> trackInputLevel(const juce::String& trackId);   // peak since last call
    std::function<void()> onRecordingFinished;    // message thread, after clips were added

    // ---- auditioning
    void previewSample(SamplePtr s);          // nullptr stops
    bool isPreviewingSample() const { return renderer->previewSample.load() != nullptr; }

    // ---- live input (virtual keyboard, pads, MIDI)
    void liveNoteOn(const juce::String& trackId, int note, float velocity);
    void liveNoteOff(const juce::String& trackId, int note);
    void liveControl(const juce::String& trackId, LiveEvent::Type type, float value);
    void setLiveTarget(const juce::String& trackId) { liveTarget.store(Compiler::uidFor(trackId)); liveTargetId = trackId; }
    juce::String getLiveTarget() const { return liveTargetId; }

    // ---- MIDI devices / learn / clock
    juce::StringArray midiInputNames() const;
    void setMidiInputEnabled(const juce::String& identifier, bool on);
    bool isMidiInputEnabled(const juce::String& identifier) const;
    juce::Array<juce::MidiDeviceInfo> midiInputs() const { return juce::MidiInput::getAvailableDevices(); }
    void startMidiLearn(ParamSetPtr params, int index);
    void cancelMidiLearn();
    bool isLearning() const { return learnParams != nullptr; }
    void clearMidiMappings() { const juce::SpinLock::ScopedLockType l(mapLock); mappings.clear(); }
    int numMidiMappings() const { const juce::SpinLock::ScopedLockType l(mapLock); return (int)mappings.size(); }
    bool midiClockSync = false;
    double midiClockBpm() const { return clockBpm.load(); }
    std::function<void()> onMidiLearned;
    std::atomic<int> midiActivity { 0 };

    // ---- meters / diagnostics (message thread)
    std::shared_ptr<ChannelProc> channelFor(const juce::String& trackId) const;
    Effect* effectFor(const juce::String& effectId) const {
        auto it = registry.effects.find(effectId);
        return it != registry.effects.end() && it->second ? it->second->fx.get() : nullptr;
    }
    double cpuUsage() const { return devices.getCpuUsage(); }
    int xruns() const { return devices.getXRunCount(); }
    double sampleRate() const { return currentRate; }
    int blockSize() const { return currentBlock; }
    int voices() const { return voiceCount.load(); }
    bool isIdle() const { return idleFlag.load(); }
    void setBatterySaver(bool on) { renderer->idleSleepEnabled = on; }
    int snapshotsBuilt() const { return (int)generation; }
    juce::String deviceName() const;
    std::function<void()> onTransportChanged;

    Renderer& rendererForTests() { return *renderer; }
    std::atomic<bool> simulateInput { false };    // tests: the silent clock feeds a 220 Hz sine as input

private:
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut, int n,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
    void timerCallback() override;
    void collectGarbage();
    void finishRecordingClips();
    void publish(std::unique_ptr<Snapshot> s);

    enum class Cmd : uint8_t { Play, Stop, Seek, CountIn, MetronomeOn, MetronomeOff, AllOff, RecordArm, RecordDisarm };
    struct Command { Cmd type; double value; };
    SpscQueue<Command, 256> commands;
    void send(Cmd c, double v = 0.0) { commands.push({ c, v }); }

    juce::AudioDeviceManager devices;
    std::unique_ptr<Renderer> renderer;
    ProcRegistry registry;
    Project* project = nullptr;
    bool dirty = false;
    double currentRate = 48000.0;
    int currentBlock = 256;
    bool lowLatency = false;
    int normalBuffer = 0;
    uint64_t generation = 0;

    std::atomic<Snapshot*> pending { nullptr };
    std::atomic<Snapshot*> active { nullptr };
    SpscQueue<Snapshot*, 64> retired;              // audio -> message thread
    std::vector<std::unique_ptr<Snapshot>> owned;   // all live snapshots (message thread)

    std::atomic<double> playhead { 0.0 };
    std::atomic<bool> playingFlag { false }, countingFlag { false };
    std::atomic<uint32_t> liveTarget { 0 };
    juce::String liveTargetId;
    std::atomic<float> inputPeak[2] {};
    std::atomic<int> callbacks { 0 };

    // recording
    struct RecSlot {
        juce::String trackId, file;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        std::atomic<juce::AudioFormatWriter::ThreadedWriter*> live { nullptr };
        int inputCh = 0;
        bool stereo = false;
        uint32_t uid = 0;
        double extraLatency = 0.0;     // seconds added by the input effects when recording with them
        std::atomic<juce::int64> written { 0 };
    };
    std::array<RecSlot, 8> rec;
    int numRec = 0;
    bool recording = false;
    double recPunchTick = 0.0;
    std::atomic<int> wrapCount { 0 };
    std::array<std::atomic<juce::int64>, 64> wrapAt {};   // samples written at each loop wrap
    std::atomic<bool> recArmed { false };
    juce::String recMidiClipTrack;
    std::map<juce::String, juce::String> recMidiClip;      // trackId -> clip id being recorded
    std::map<std::pair<uint32_t, int>, std::pair<double, float>> openNotes;
    juce::TimeSliceThread writerThread { "Recorder" };
    void drainRecordedMidi();

    // MIDI
    juce::OwnedArray<juce::MidiInput> openInputs;
    juce::StringArray enabledInputs;
    struct Mapping { int channel, cc; std::weak_ptr<ParamSet> params; int index; };
    std::vector<Mapping> mappings;
    mutable juce::SpinLock mapLock;
    ParamSetPtr learnParams;
    int learnIndex = -1;
    std::atomic<double> clockBpm { 0.0 };
    double lastClockTime = 0.0, clockAccum = 0.0;
    int clockCount = 0;
    std::atomic<int> voiceCount { 0 };
    std::atomic<bool> idleFlag { false };
    SamplePtr previewHold, previewHoldOld;

    // used when no audio device exists (CI, headless): drives the callback in real time
    class NullDevice;
    std::unique_ptr<NullDevice> nullDevice;
};
