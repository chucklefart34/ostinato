#include "AudioEngine.h"
#include "Core/Paths.h"
#include "Dsp.h"

// Drives the audio callback in real time when no hardware device is available
// (headless CI machines, or a desktop with every output disabled).
class AudioEngine::NullDevice : public juce::Thread {
public:
    explicit NullDevice(AudioEngine& e) : juce::Thread("NullAudio"), engine(e) {}
    ~NullDevice() override { stopThread(2000); }
    void run() override {
        constexpr int n = 512;
        const double sr = 48000.0;
        juce::AudioBuffer<float> out(2, n), in(2, n);
        double ph = 0.0;
        engine.currentRate = sr;
        engine.currentBlock = n;
        engine.renderer->prepare(sr);
        juce::MessageManager::callAsync([e = &engine] { e->rebuild(); });
        const double blockMs = 1000.0 * n / sr;
        double next = juce::Time::getMillisecondCounterHiRes();
        while (!threadShouldExit()) {
            float* ptrs[2] = { out.getWritePointer(0), out.getWritePointer(1) };
            if (engine.simulateInput.load()) {
                for (int i = 0; i < n; ++i) { const float v = 0.5f * (float)std::sin(ph); ph += 2.0 * 3.14159265358979 * 220.0 / sr; in.setSample(0, i, v); in.setSample(1, i, v); }
                const float* ip[2] = { in.getReadPointer(0), in.getReadPointer(1) };
                engine.audioDeviceIOCallbackWithContext(ip, 2, ptrs, 2, n, {});
            } else engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, n, {});
            next += blockMs;
            const double wait = next - juce::Time::getMillisecondCounterHiRes();
            if (wait > 0) wait_ms((int)wait); else next = juce::Time::getMillisecondCounterHiRes();
        }
    }
    void wait_ms(int ms) { wait(ms); }
private:
    AudioEngine& engine;
};

AudioEngine::AudioEngine() : renderer(std::make_unique<Renderer>()) {
    renderer->prepare(currentRate);
    renderer->onTrackInput = [this](uint32_t uid, const float* L, const float* R, int n) {
        for (int k = 0; k < numRec; ++k) {
            auto& slot = rec[(size_t)k];
            if (slot.uid != uid) continue;
            auto* w = slot.live.load(std::memory_order_acquire);
            if (!w) continue;
            const float* ptrs[2] = { L, R };
            if (w->write(ptrs, n)) slot.written.fetch_add(n);
        }
    };
}

AudioEngine::~AudioEngine() { shutdown(); }

void AudioEngine::initialise(const juce::String& savedXml) {
    std::unique_ptr<juce::XmlElement> xml;
    if (savedXml.isNotEmpty()) xml = juce::parseXML(savedXml);
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.bufferSize = project ? project->bufferSize : 256;
    setup.sampleRate = project ? project->sampleRate : 48000;
    const auto err = devices.initialise(0, 2, xml.get(), true, {}, &setup);
    if (err.isNotEmpty()) DBG("Audio init: " << err);
    devices.addAudioCallback(this);
    for (auto& d : juce::MidiInput::getAvailableDevices()) setMidiInputEnabled(d.identifier, true);
    if (!devices.getCurrentAudioDevice()) {
        nullDevice = std::make_unique<NullDevice>(*this);
        nullDevice->startThread(juce::Thread::Priority::high);
    }
    writerThread.startThread();
    startTimerHz(30);
}

void AudioEngine::shutdown() {
    stopTimer();
    if (recording) stopRecording();
    nullDevice.reset();
    devices.removeAudioCallback(this);
    for (auto* m : openInputs) m->stop();
    openInputs.clear();
    devices.closeAudioDevice();
    writerThread.stopThread(2000);
    renderer->setSnapshot(nullptr);
    pending.store(nullptr);
    active.store(nullptr);
    owned.clear();
    registry.clear();
}

juce::String AudioEngine::deviceStateXml() const {
    auto x = devices.createStateXml();
    return x ? x->toString() : juce::String();
}

juce::String AudioEngine::deviceName() const {
    if (auto* d = devices.getCurrentAudioDevice()) return d->getName();
    return nullDevice ? "No audio device (silent clock)" : "None";
}

bool AudioEngine::inputEnabled() const {
    if (simulateInput.load() && nullDevice) return true;
    if (auto* d = devices.getCurrentAudioDevice()) return d->getActiveInputChannels().countNumberOfSetBits() > 0;
    return false;
}

void AudioEngine::enableInput(std::function<void(bool)> done) {
    auto apply = [this, done](bool granted) {
        if (!granted) { if (done) done(false); return; }
        auto setup = devices.getAudioDeviceSetup();
        setup.useDefaultInputChannels = true;
        setup.inputChannels.setRange(0, 2, true);
        const auto err = devices.setAudioDeviceSetup(setup, true);
        if (done) done(err.isEmpty() && inputEnabled());
    };
    if (juce::RuntimePermissions::isRequired(juce::RuntimePermissions::recordAudio)
        && !juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio))
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, apply);
    else
        apply(true);
}

double AudioEngine::monitorLatencyMs() const {
    if (auto* d = devices.getCurrentAudioDevice())
        return 1000.0 * (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples() + d->getCurrentBufferSizeSamples()) / std::max(1.0, d->getCurrentSampleRate());
    return 0.0;
}

bool AudioEngine::setLowLatency(bool on, juce::String& message) {
    auto* d = devices.getCurrentAudioDevice();
    if (!d) { message = "No audio device is open, so there is nothing to speed up."; lowLatency = false; return false; }
    auto setup = devices.getAudioDeviceSetup();
    if (on) {
        auto sizes = d->getAvailableBufferSizes();
        int smallest = 0;
        for (int b : sizes) if (b >= 16 && (smallest == 0 || b < smallest)) smallest = b;
        if (smallest == 0) { message = "This device does not offer a smaller buffer."; return false; }
        if (!lowLatency) normalBuffer = setup.bufferSize > 0 ? setup.bufferSize : d->getCurrentBufferSizeSamples();
        setup.bufferSize = smallest;
    } else setup.bufferSize = normalBuffer > 0 ? normalBuffer : 256;
    const auto err = devices.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty()) { message = err; return false; }
    lowLatency = on;
    message = juce::String(on ? "Low latency on: " : "Normal buffer: ") + juce::String(devices.getCurrentAudioDevice() ? devices.getCurrentAudioDevice()->getCurrentBufferSizeSamples() : 0)
              + " samples, about " + juce::String(monitorLatencyMs(), 1) + " ms from microphone to headphones";
    return true;
}

int AudioEngine::inputChainLatencySamples(const Track& t) const {
    int lat = 0;
    for (auto& e : t.inputEffects) {
        if (e.bypass->load()) continue;
        auto it = registry.effects.find(e.id);
        if (it != registry.effects.end() && it->second && it->second->fx) lat += it->second->fx->latencySamples();
    }
    return lat;
}

std::pair<float, float> AudioEngine::trackInputLevel(const juce::String& trackId) {
    if (auto ch = channelFor(trackId)) {
        std::pair<float, float> r { ch->inPeakL.exchange(0.f), ch->inPeakR.exchange(0.f) };
        return r;
    }
    return { 0.f, 0.f };
}

double AudioEngine::reportedLatencyMs() const {
    if (auto* d = devices.getCurrentAudioDevice())
        return 1000.0 * (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples()) / std::max(1.0, d->getCurrentSampleRate());
    return 0.0;
}

// ------------------------------------------------------------------ snapshots
void AudioEngine::setProject(Project* p) {
    if (p != project) {
        send(Cmd::Stop);
        send(Cmd::AllOff);
    }
    project = p;
    registry.clear();
    rebuild();
}

void AudioEngine::publish(std::unique_ptr<Snapshot> s) {
    Snapshot* raw = s.get();
    owned.push_back(std::move(s));
    if (Snapshot* stale = pending.exchange(raw)) {   // never reached the audio thread
        owned.erase(std::remove_if(owned.begin(), owned.end(), [stale](auto& u) { return u.get() == stale; }), owned.end());
    }
}

void AudioEngine::rebuild() {
    dirty = false;
    if (!project) return;
    auto s = Compiler::compile(*project, registry, currentRate, false);
    s->generation = ++generation;
    publish(std::move(s));
}

void AudioEngine::collectGarbage() {
    Snapshot* r = nullptr;
    while (retired.pop(r))
        owned.erase(std::remove_if(owned.begin(), owned.end(), [r](auto& u) { return u.get() == r; }), owned.end());
}

std::shared_ptr<ChannelProc> AudioEngine::channelFor(const juce::String& id) const {
    auto it = registry.channels.find(id);
    return it != registry.channels.end() ? it->second : nullptr;
}

// ------------------------------------------------------------------ transport
void AudioEngine::play() {
    if (project) send(Cmd::Seek, playhead.load());
    send(Cmd::Play);
    playingFlag = true;
}
void AudioEngine::stop() {
    send(Cmd::Stop);
    playingFlag = false;
    countingFlag = false;
}
void AudioEngine::seek(double tick) { send(Cmd::Seek, tick); playhead = std::max(0.0, tick); }
void AudioEngine::setMetronome(bool on) { send(on ? Cmd::MetronomeOn : Cmd::MetronomeOff); }

void AudioEngine::previewSample(SamplePtr s) {
    renderer->previewSample.store(nullptr, std::memory_order_release);
    // keep the previous sample alive for one more cycle in case the audio thread is mid-read
    previewHoldOld = previewHold;
    previewHold = s;
    if (s) { renderer->previewPos = 0.0; renderer->previewSample.store(s.get(), std::memory_order_release); }
}

void AudioEngine::liveNoteOn(const juce::String& id, int note, float vel) {
    renderer->liveQueue.push({ LiveEvent::NoteOn, Compiler::uidFor(id), note, vel });
}
void AudioEngine::liveNoteOff(const juce::String& id, int note) {
    renderer->liveQueue.push({ LiveEvent::NoteOff, Compiler::uidFor(id), note, 0.f });
}
void AudioEngine::liveControl(const juce::String& id, LiveEvent::Type type, float value) {
    renderer->liveQueue.push({ type, Compiler::uidFor(id), 0, value });
}

// ------------------------------------------------------------------ audio callback
void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* d) {
    currentRate = d->getCurrentSampleRate();
    currentBlock = d->getCurrentBufferSizeSamples();
    renderer->prepare(currentRate);
    juce::MessageManager::callAsync([this] { rebuild(); });
}
void AudioEngine::audioDeviceStopped() {}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut, int n,
                                                   const juce::AudioIODeviceCallbackContext&) {
    juce::ScopedNoDenormals noDenormals;
    callbacks.fetch_add(1);
    if (Snapshot* p = pending.exchange(nullptr)) {
        Snapshot* old = active.exchange(p);
        renderer->setSnapshot(p);
        if (old) retired.push(old);
    }
    Command c;
    while (commands.pop(c)) {
        switch (c.type) {
            case Cmd::Play: renderer->play(); break;
            case Cmd::Stop: renderer->stop(); renderer->midiRecording = false; break;
            case Cmd::Seek: renderer->seek(c.value); break;
            case Cmd::CountIn: renderer->startCountIn((int)c.value); break;
            case Cmd::MetronomeOn: renderer->metronome = true; break;
            case Cmd::MetronomeOff: renderer->metronome = false; break;
            case Cmd::AllOff: renderer->allNotesOff(true); break;
            case Cmd::RecordArm: renderer->punchInTick = c.value; renderer->midiRecording = true; wrapCount = 0; break;
            case Cmd::RecordDisarm: renderer->midiRecording = false; break;
        }
    }
    for (int ch = 0; ch < std::min(2, numIn); ++ch) {
        if (!in[ch]) continue;
        float pk = 0.f;
        for (int i = 0; i < n; ++i) pk = std::max(pk, std::fabs(in[ch][i]));
        if (pk > inputPeak[ch].load(std::memory_order_relaxed)) inputPeak[ch].store(pk, std::memory_order_relaxed);
    }
    float* L = numOut > 0 ? out[0] : nullptr;
    float* R = numOut > 1 ? out[1] : nullptr;
    float scratchL[4096], scratchR[4096];
    if (!L || n > 4096) { for (int ch = 0; ch < numOut; ++ch) if (out[ch]) juce::FloatVectorOperations::clear(out[ch], n); return; }
    if (!R) R = scratchR;
    (void)scratchL;
    renderer->render(L, R, n, in, numIn);
    if (renderer->loopWrapped && numRec > 0) {
        const int w = wrapCount.load();
        if (w < 64) { wrapAt[(size_t)w].store(rec[0].written.load()); wrapCount.store(w + 1); }
    }
    for (int i = 0; i < n; ++i) {   // last-resort protection for ears and speakers
        if (!std::isfinite(L[i])) L[i] = 0.f;
        if (!std::isfinite(R[i])) R[i] = 0.f;
    }
    for (int ch = 2; ch < numOut; ++ch) if (out[ch]) juce::FloatVectorOperations::clear(out[ch], n);
    if (numOut == 1) for (int i = 0; i < n; ++i) L[i] = 0.5f * (L[i] + R[i]);
    playhead.store(renderer->tick(), std::memory_order_relaxed);
    playingFlag.store(renderer->isPlaying() || renderer->isCountingIn(), std::memory_order_relaxed);
    countingFlag.store(renderer->isCountingIn(), std::memory_order_relaxed);
    voiceCount.store(renderer->activeVoices, std::memory_order_relaxed);
    idleFlag.store(renderer->isIdle(), std::memory_order_relaxed);
}

// ------------------------------------------------------------------ recording
bool AudioEngine::startRecording(juce::String& error) {
    if (recording || !project) return recording;
    std::vector<Track*> audioArmed, midiArmed;
    for (auto& t : project->tracks) {
        if (!t.armed) continue;
        if (t.type == TrackType::Audio) audioArmed.push_back(&t);
        else if (t.hasInstrument()) midiArmed.push_back(&t);
    }
    if (audioArmed.empty() && midiArmed.empty()) { error = "Arm a track first: tap the record dot on an audio or instrument track."; return false; }
    if (!audioArmed.empty() && !inputEnabled()) { error = "Audio input is off. Allow microphone access, then try again."; return false; }
    const auto audioDir = SampleBank::get().projectDir().getChildFile("audio");
    audioDir.createDirectory();
    numRec = 0;
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    for (auto* t : audioArmed) {
        if (numRec >= (int)rec.size()) break;
        auto& slot = rec[(size_t)numRec];
        const auto f = audioDir.getNonexistentChildFile("Rec " + juce::File::createLegalFileName(t->name) + " " + stamp, ".wav");
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
        if (!os) { error = "Cannot create a recording file in the project folder."; return false; }
        juce::WavAudioFormat wav;
        const int ch = t->stereoInput || t->recordsWithEffects() ? 2 : 1;
        auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(currentRate).withNumChannels(ch).withBitsPerSample(24));
        if (!w) { error = "Cannot open the recording file for writing."; return false; }
        slot.trackId = t->id;
        slot.file = "audio/" + f.getFileName();
        slot.inputCh = t->inputChannel;
        slot.stereo = ch == 2;
        slot.uid = Compiler::uidFor(t->id);
        slot.extraLatency = t->recordsWithEffects() ? inputChainLatencySamples(*t) / currentRate : 0.0;
        slot.written = 0;
        slot.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(w.release(), writerThread, (int)(currentRate * 8));
        ++numRec;
    }
    recPunchTick = playhead.load();
    if (project->loopOn && recPunchTick >= project->loopEnd) recPunchTick = project->loopStart;
    recMidiClip.clear();
    openNotes.clear();
    for (int k = 0; k < numRec; ++k) rec[(size_t)k].live.store(rec[(size_t)k].writer.get(), std::memory_order_release);
    send(Cmd::RecordArm, recPunchTick);
    if (!isPlaying()) {
        send(Cmd::Seek, recPunchTick);
        if (project->countInBars > 0) {
            const int bar = project->tickToBar((int)recPunchTick);
            const auto ts = project->timeSigAtBar(bar);
            send(Cmd::CountIn, ts.num * project->countInBars);
        } else send(Cmd::Play);
        playingFlag = true;
    }
    recording = true;
    return true;
}

void AudioEngine::stopRecording() {
    if (!recording) return;
    for (int k = 0; k < numRec; ++k) rec[(size_t)k].live.store(nullptr, std::memory_order_release);
    send(Cmd::RecordDisarm);
    send(Cmd::Stop);
    // wait until the audio thread has finished any block that may still use a writer
    const int start = callbacks.load();
    const auto t0 = juce::Time::getMillisecondCounter();
    while (callbacks.load() < start + 2 && juce::Time::getMillisecondCounter() - t0 < 500) juce::Thread::sleep(2);
    playingFlag = false;
    drainRecordedMidi();
    finishRecordingClips();
    recording = false;
    rebuild();
    if (onRecordingFinished) onRecordingFinished();
}

void AudioEngine::finishRecordingClips() {
    if (!project) return;
    const double baseLatency = reportedLatencyMs() / 1000.0 + latencyCompensationMs / 1000.0;
    for (int k = 0; k < numRec; ++k) {
        auto& slot = rec[(size_t)k];
        const double latency = baseLatency + slot.extraLatency;
        const juce::int64 written = slot.written.load();
        slot.writer.reset();   // flushes and closes the file
        Track* t = project->findTrack(slot.trackId);
        if (!t || written < (juce::int64)(currentRate * 0.05)) {
            SampleBank::get().resolve(slot.file).deleteFile();
            continue;
        }
        const double durSec = (double)written / currentRate;
        Clip c;
        c.id = project->newId("c");
        c.type = ClipType::Audio;
        c.name = t->name + " take";
        c.audio.file = slot.file;
        const int wraps = wrapCount.load();
        if (wraps > 0 && project->loopOn) {
            c.start = project->loopStart;
            c.length = project->loopEnd - project->loopStart;
            const double loopStartSec = project->ticksToSeconds(project->loopStart);
            const double punchSec = project->ticksToSeconds(recPunchTick);
            juce::StringArray takes;
            takes.add(slot.file + "|" + juce::String(latency - (punchSec - loopStartSec), 6));
            for (int w = 0; w < wraps; ++w) takes.add(slot.file + "|" + juce::String((double)wrapAt[(size_t)w].load() / currentRate + latency, 6));
            // choose the last complete take
            const double loopDur = project->ticksToSeconds(project->loopEnd) - loopStartSec;
            int activeTake = takes.size() - 1;
            const double lastLen = durSec - (double)wrapAt[(size_t)wraps - 1].load() / currentRate;
            if (lastLen < loopDur * 0.5 && activeTake > 0) --activeTake;
            c.audio.takes = takes;
            c.audio.offsetSec = takes[activeTake].fromLastOccurrenceOf("|", false, false).getDoubleValue();
            c.name = t->name + " take " + juce::String(activeTake + 1) + "/" + juce::String(takes.size());
        } else {
            c.start = (int)recPunchTick;
            const double endSec = project->ticksToSeconds(recPunchTick) + durSec - latency;
            c.length = std::max(1, (int)project->secondsToTicks(endSec) - c.start);
            c.audio.offsetSec = latency;
            c.audio.takes.add(slot.file + "|" + juce::String(latency, 6));
        }
        t->clips.push_back(c);
    }
    numRec = 0;
    // close hanging MIDI notes at the playhead
    for (auto& [key, on] : openNotes) {
        for (auto& t : project->tracks) {
            if (Compiler::uidFor(t.id) != key.first) continue;
            auto it = recMidiClip.find(t.id);
            if (it == recMidiClip.end()) continue;
            for (auto& c : t.clips)
                if (c.id == it->second) {
                    const int start = (int)on.first - c.start;
                    c.notes.push_back({ start, std::max(PPQ / 16, (int)playhead.load() - (int)on.first), key.second, on.second });
                }
        }
    }
    openNotes.clear();
    recMidiClip.clear();
}

void AudioEngine::drainRecordedMidi() {
    if (!project) return;
    RecordedNote rn;
    bool changed = false;
    while (renderer->recordedNotes.pop(rn)) {
        Track* track = nullptr;
        for (auto& t : project->tracks) if (Compiler::uidFor(t.id) == rn.trackUid) { track = &t; break; }
        if (!track) continue;
        auto key = std::make_pair(rn.trackUid, rn.pitch);
        if (rn.on) { openNotes[key] = { rn.tick, rn.velocity }; continue; }
        auto it = openNotes.find(key);
        if (it == openNotes.end()) continue;
        const double onTick = it->second.first;
        const float vel = it->second.second;
        openNotes.erase(it);
        Clip* clip = nullptr;
        if (recMidiClip.count(track->id))
            for (auto& c : track->clips) if (c.id == recMidiClip[track->id]) clip = &c;
        if (!clip) {
            const int start = project->loopOn ? project->loopStart : project->barToTick(project->tickToBar((int)recPunchTick));
            track->clips.push_back(project->makeMidiClip(start, project->ticksPerBarAt(start)));
            clip = &track->clips.back();
            clip->name = "Recorded";
            recMidiClip[track->id] = clip->id;
        }
        const int rel = std::max(0, (int)onTick - clip->start);
        const int len = std::max(PPQ / 32, (int)(rn.tick - onTick));
        clip->notes.push_back({ rel, len, rn.pitch, vel });
        const int needed = rel + len;
        if (needed > clip->length) {
            const int bar = project->ticksPerBarAt(clip->start);
            clip->length = ((needed + bar - 1) / bar) * bar;
            clip->contentLength = clip->length;
        }
        changed = true;
    }
    if (changed) markDirty();
}

// ------------------------------------------------------------------ MIDI
void AudioEngine::setMidiInputEnabled(const juce::String& id, bool on) {
    for (int i = openInputs.size(); --i >= 0;)
        if (openInputs[i]->getIdentifier() == id) { openInputs[i]->stop(); openInputs.remove(i); }
    enabledInputs.removeString(id);
    if (!on) return;
    if (auto in = juce::MidiInput::openDevice(id, this)) {
        in->start();
        openInputs.add(in.release());
        enabledInputs.add(id);
    }
}
bool AudioEngine::isMidiInputEnabled(const juce::String& id) const { return enabledInputs.contains(id); }
juce::StringArray AudioEngine::midiInputNames() const {
    juce::StringArray s;
    for (auto& d : juce::MidiInput::getAvailableDevices()) s.add(d.name);
    return s;
}
void AudioEngine::startMidiLearn(ParamSetPtr p, int index) { const juce::SpinLock::ScopedLockType l(mapLock); learnParams = std::move(p); learnIndex = index; }
void AudioEngine::cancelMidiLearn() { const juce::SpinLock::ScopedLockType l(mapLock); learnParams.reset(); learnIndex = -1; }

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) {
    midiActivity.fetch_add(1);
    static juce::SpinLock pushLock;   // several devices may call back on different threads
    auto push = [this](LiveEvent e) { const juce::SpinLock::ScopedLockType l(pushLock); renderer->midiQueue.push(e); };
    const uint32_t target = liveTarget.load();
    if (m.isMidiClock()) {
        const double now = m.getTimeStamp() > 0.0 ? m.getTimeStamp() : juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (lastClockTime > 0.0) {
            clockAccum += now - lastClockTime;
            if (++clockCount >= 24) {
                const double bpm = 60.0 / clockAccum;
                if (bpm > 20.0 && bpm < 300.0) clockBpm.store(clockBpm.load() <= 0.0 ? bpm : clockBpm.load() * 0.7 + bpm * 0.3);
                clockAccum = 0.0; clockCount = 0;
            }
        }
        lastClockTime = now;
        return;
    }
    if (m.isMidiStart() || m.isMidiContinue() || m.isMidiStop()) {
        if (!midiClockSync) return;
        const bool startMsg = m.isMidiStart(), stopMsg = m.isMidiStop();
        juce::MessageManager::callAsync([this, startMsg, stopMsg] {
            if (stopMsg) stop();
            else { if (startMsg) seek(0); play(); }
            if (onTransportChanged) onTransportChanged();
        });
        return;
    }
    if (m.isController()) {
        {
            const juce::SpinLock::ScopedLockType l(mapLock);
            if (learnParams) {
                mappings.erase(std::remove_if(mappings.begin(), mappings.end(), [&](const Mapping& mp) {
                    auto sp = mp.params.lock(); return (sp == learnParams && mp.index == learnIndex) || (mp.channel == m.getChannel() && mp.cc == m.getControllerNumber());
                }), mappings.end());
                mappings.push_back({ m.getChannel(), m.getControllerNumber(), learnParams, learnIndex });
                learnParams.reset();
                juce::MessageManager::callAsync([this] { if (onMidiLearned) onMidiLearned(); });
                return;
            }
            bool mapped = false;
            for (auto& mp : mappings)
                if (mp.channel == m.getChannel() && mp.cc == m.getControllerNumber())
                    if (auto sp = mp.params.lock()) { sp->set(mp.index, sp->fromNorm(mp.index, m.getControllerValue() / 127.f)); mapped = true; }
            if (mapped) return;
        }
        if (m.getControllerNumber() == 1) push({ LiveEvent::ModWheel, target, 0, m.getControllerValue() / 127.f });
        else if (m.getControllerNumber() == 64) push({ LiveEvent::Sustain, target, 0, m.getControllerValue() >= 64 ? 1.f : 0.f });
        else if (m.getControllerNumber() == 123) push({ LiveEvent::AllOff, target, 0, 0.f });
        return;
    }
    if (m.isNoteOn()) push({ LiveEvent::NoteOn, target, m.getNoteNumber(), m.getFloatVelocity() });
    else if (m.isNoteOff()) push({ LiveEvent::NoteOff, target, m.getNoteNumber(), 0.f });
    else if (m.isPitchWheel()) push({ LiveEvent::PitchBend, target, 0, (m.getPitchWheelValue() - 8192) / 8192.f });
}

// ------------------------------------------------------------------ housekeeping
void AudioEngine::timerCallback() {
    collectGarbage();
    if (recording) drainRecordedMidi();
    if (dirty) rebuild();
}
