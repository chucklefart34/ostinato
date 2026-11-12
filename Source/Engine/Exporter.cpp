#include "Exporter.h"
#include "Dsp.h"

juce::StringArray ExportSettings::presetNames() {
    juce::StringArray s { "High-quality WAV (48 kHz / 24-bit)", "CD-quality WAV (44.1 kHz / 16-bit)", "Lossless FLAC (48 kHz / 24-bit)",
                          "High-quality compressed (OGG 256 kbps)", "Compressed preview (OGG 96 kbps)" };
    if (formatAvailable(AAC)) s.add("AAC / M4A (256 kbps)");
    return s;
}

ExportSettings ExportSettings::preset(int i) {
    ExportSettings s;
    switch (i) {
        case 1: s.format = WAV; s.sampleRate = 44100; s.bitDepth = 16; break;
        case 2: s.format = FLAC; s.sampleRate = 48000; s.bitDepth = 24; break;
        case 3: s.format = OGG; s.sampleRate = 48000; s.oggQuality = 8; break;
        case 4: s.format = OGG; s.sampleRate = 44100; s.oggQuality = 2; break;
        case 5: s.format = AAC; s.sampleRate = 44100; s.aacKbps = 256; break;
        default: s.format = WAV; s.sampleRate = 48000; s.bitDepth = 24; break;
    }
    return s;
}

bool ExportSettings::formatAvailable(Format f) {
   #if JUCE_MAC || JUCE_IOS
    return true;
   #else
    return f != AAC;
   #endif
}

juce::String ExportSettings::extension() const {
    switch (format) { case FLAC: return ".flac"; case OGG: return ".ogg"; case AAC: return ".m4a"; default: return ".wav"; }
}

Exporter::Exporter(const Project& p, ExportSettings s) : juce::Thread("Export"), projectState(p.toVar()), settings(std::move(s)) {}
Exporter::~Exporter() { signalThreadShouldExit(); stopThread(10000); }

bool Exporter::renderToBuffer(const Project& src, double sr, juce::AudioBuffer<float>& out, double maxTail, juce::String& error,
                              const std::atomic<bool>* cancel, std::atomic<float>* prog, bool loopOnly) {
    // Work on an independent copy so editing during export cannot affect the render.
    Project p;
    if (!p.fromVar(src.toVar(), error)) return false;
    ProcRegistry reg;
    auto snap = Compiler::compile(p, reg, sr, true);
    const int startTick = loopOnly ? p.loopStart : 0;
    const int endTick = loopOnly ? p.loopEnd : snap->songEnd;
    if (endTick <= startTick) { error = "The song is empty. Add some clips before exporting."; return false; }
    snap->loopOn = false;   // render linearly; loop range handled by start/end
    auto r = std::make_unique<Renderer>();
    r->prepare(sr);
    r->setSnapshot(snap.get());
    r->seek(startTick);
    r->play();
    const double bodySec = p.ticksToSeconds(endTick) - p.ticksToSeconds(startTick);
    const juce::int64 body = (juce::int64)std::ceil(bodySec * sr);
    const juce::int64 maxLen = body + (juce::int64)(maxTail * sr);
    out.setSize(2, (int)std::min<juce::int64>(maxLen, std::numeric_limits<int>::max() / 2), false, true, false);
    constexpr int block = 512;
    juce::int64 pos = 0, silentRun = 0;
    const float silence = dsp::db2lin(-90.f);
    while (pos < maxLen) {
        if (cancel && cancel->load()) { error = "Cancelled"; return false; }
        const int n = (int)std::min<juce::int64>(block, maxLen - pos);
        float* L = out.getWritePointer(0, (int)pos);
        float* R = out.getWritePointer(1, (int)pos);
        r->render(L, R, n);
        float pk = 0.f;
        for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
        pos += n;
        if (pos > body) {
            silentRun = pk < silence ? silentRun + n : 0;
            if (silentRun > (juce::int64)(sr * 0.5)) break;   // tail has decayed
        }
        if (prog) prog->store((float)std::min(1.0, (double)pos / (double)std::max<juce::int64>(1, body + (juce::int64)(sr))));
    }
    out.setSize(2, (int)pos, true, true, true);
    // short fade at the very end so a truncated (max-tail) render never clicks
    const int fade = std::min(out.getNumSamples(), (int)(sr * 0.01));
    out.applyGainRamp(out.getNumSamples() - fade, fade, 1.f, 0.f);
    return true;
}

void Exporter::run() {
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    Project p;
    juce::String err;
    juce::AudioBuffer<float> audio;
    auto finish = [this, t0] {
        result.renderMs = juce::Time::getMillisecondCounterHiRes() - t0;
        finished = true;
        if (onDone) juce::MessageManager::callAsync([this] { if (onDone) onDone(result); });
    };
    if (!p.fromVar(projectState, err)) { result.error = err; finish(); return; }
    std::atomic<bool> cancelFlag { false };
    struct Watch : juce::Thread::Listener { std::atomic<bool>& f; explicit Watch(std::atomic<bool>& x) : f(x) {} void exitSignalSent() override { f = true; } } watch(cancelFlag);
    addListener(&watch);
    const bool ok = renderToBuffer(p, settings.sampleRate, audio, settings.maxTailSeconds, err, &cancelFlag, &progress, settings.loopRangeOnly);
    removeListener(&watch);
    if (!ok) { result.cancelled = cancelFlag.load(); result.error = err; finish(); return; }
    result.seconds = audio.getNumSamples() / settings.sampleRate;
    result.peakDb = dsp::lin2db(audio.getMagnitude(0, audio.getNumSamples()));

    auto dest = settings.destination;
    if (dest == juce::File()) { result.error = "No destination file."; finish(); return; }
    dest.getParentDirectory().createDirectory();
    const auto tmp = dest.getSiblingFile("." + dest.getFileName() + ".part");
    tmp.deleteFile();

    if (settings.format == ExportSettings::AAC) {
        if (!PlatformAudio::encodeAAC(audio, settings.sampleRate, settings.aacKbps, tmp, err)) { result.error = err; tmp.deleteFile(); finish(); return; }
    } else {
        if (settings.format != ExportSettings::OGG && settings.bitDepth == 16 && settings.dither) {
            juce::Random rnd(1234);
            const float lsb = 1.f / 32768.f;
            for (int c = 0; c < 2; ++c) {
                float* d = audio.getWritePointer(c);
                for (int i = 0; i < audio.getNumSamples(); ++i) d[i] += (rnd.nextFloat() - rnd.nextFloat()) * lsb;
            }
        }
        std::unique_ptr<juce::AudioFormat> fmt;
        if (settings.format == ExportSettings::FLAC) fmt = std::make_unique<juce::FlacAudioFormat>();
        else if (settings.format == ExportSettings::OGG) fmt = std::make_unique<juce::OggVorbisAudioFormat>();
        else fmt = std::make_unique<juce::WavAudioFormat>();
        std::unique_ptr<juce::OutputStream> os(tmp.createOutputStream().release());
        if (!os) { result.error = "Cannot write to " + dest.getParentDirectory().getFullPathName(); finish(); return; }
        auto opts = juce::AudioFormatWriterOptions{}.withSampleRate(settings.sampleRate).withNumChannels(2);
        if (settings.format == ExportSettings::OGG) opts = opts.withQualityOptionIndex(settings.oggQuality).withBitsPerSample(16);
        else if (settings.format == ExportSettings::FLAC) opts = opts.withBitsPerSample(settings.bitDepth == 16 ? 16 : 24);
        else {
            opts = opts.withBitsPerSample(settings.bitDepth);
            if (settings.bitDepth == 32) opts = opts.withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        }
        auto w = fmt->createWriterFor(os, opts);
        if (!w) { result.error = "This format does not support the chosen sample rate / bit depth."; tmp.deleteFile(); finish(); return; }
        const int chunk = 65536;
        for (int pos = 0; pos < audio.getNumSamples(); pos += chunk) {
            const int n = std::min(chunk, audio.getNumSamples() - pos);
            if (!w->writeFromAudioSampleBuffer(audio, pos, n)) { result.error = "Disk write failed (storage full?)."; w.reset(); tmp.deleteFile(); finish(); return; }
        }
        w.reset();
    }
    dest.deleteFile();
    if (!tmp.moveFileTo(dest)) { result.error = "Could not move the finished file into place."; finish(); return; }
    result.ok = true;
    result.file = dest;
    progress = 1.f;
    finish();
}

#if ! (JUCE_MAC || JUCE_IOS)
namespace PlatformAudio {
bool encodeAAC(const juce::AudioBuffer<float>&, double, int, const juce::File&, juce::String& error) {
    error = "AAC export uses the Apple encoder and is available on iPhone, iPad and Mac. Choose WAV, FLAC or OGG here.";
    return false;
}
#if ! JUCE_ANDROID
void haptic(int) {}
bool hapticsAvailable() { return false; }
#else
void haptic(int) {}
bool hapticsAvailable() { return false; }
#endif
}
#endif
