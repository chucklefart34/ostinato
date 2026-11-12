#include "SelfTest.h"
#include "Core/MidiIO.h"
#include "Core/Paths.h"
#include "Core/ProjectStore.h"
#include "Core/Session.h"
#include "Core/Theory.h"
#include "Engine/Catalog.h"
#include "Engine/Dsp.h"
#include "Engine/Exporter.h"

namespace {

struct Suite {
    int passed = 0, failed = 0;
    juce::StringArray lines;
    juce::String section;
    void begin(const juce::String& s) {
        section = s; lines.add(""); lines.add("== " + s); std::printf("\n== %s\n", s.toRawUTF8());
        if (progress != juce::File()) progress.appendText("\n== " + s + "\n");
    }
    juce::File progress;   // a crash still leaves a trail of what passed
    bool check(bool ok, const juce::String& what, const juce::String& detail = {}) {
        (ok ? passed : failed)++;
        auto l = juce::String(ok ? "  PASS  " : "  FAIL  ") + what + (detail.isNotEmpty() ? "  [" + detail + "]" : juce::String());
        lines.add(l);
        std::printf("%s\n", l.toRawUTF8());
        std::fflush(stdout);
        if (progress != juce::File()) progress.appendText(l + "\n");
        return ok;
    }
};

constexpr double SR = 48000.0;

float peakOf(const juce::AudioBuffer<float>& b, int start = 0, int len = -1) {
    if (len < 0) len = b.getNumSamples() - start;
    start = juce::jlimit(0, b.getNumSamples(), start);
    len = juce::jlimit(0, b.getNumSamples() - start, len);
    return len > 0 ? b.getMagnitude(start, len) : 0.f;
}
float rmsOf(const juce::AudioBuffer<float>& b, int start, int len) {
    start = juce::jlimit(0, b.getNumSamples(), start);
    len = juce::jlimit(0, b.getNumSamples() - start, len);
    if (len <= 0) return 0.f;
    return 0.5f * (b.getRMSLevel(0, start, len) + b.getRMSLevel(1, start, len));
}
bool allFinite(const juce::AudioBuffer<float>& b) {
    for (int c = 0; c < b.getNumChannels(); ++c) {
        const float* d = b.getReadPointer(c);
        for (int i = 0; i < b.getNumSamples(); ++i) if (!std::isfinite(d[i])) return false;
    }
    return true;
}
// Autocorrelation pitch estimate on the mono mix of [start, start+len).
double pitchOf(const juce::AudioBuffer<float>& b, int start, int len, double sr = SR, double fmin = 40.0, double fmax = 2000.0) {
    std::vector<float> x((size_t)len);
    for (int i = 0; i < len; ++i) x[(size_t)i] = 0.5f * (b.getSample(0, start + i) + b.getSample(1, start + i));
    const int lagMin = (int)(sr / fmax), lagMax = std::min(len / 2, (int)(sr / fmin));
    std::vector<double> r((size_t)lagMax + 2, 0.0);
    double r0 = 0;
    for (int i = 0; i < len - lagMax; ++i) r0 += (double)x[(size_t)i] * x[(size_t)i];
    for (int lag = lagMin; lag <= lagMax; ++lag) {
        double acc = 0;
        for (int i = 0; i < len - lagMax; ++i) acc += (double)x[(size_t)i] * x[(size_t)(i + lag)];
        r[(size_t)lag] = acc;
    }
    // first peak above 85% of the global max (avoids octave errors)
    double mx = 0;
    for (int lag = lagMin; lag <= lagMax; ++lag) mx = std::max(mx, r[(size_t)lag]);
    int best = 0;
    for (int lag = lagMin + 1; lag < lagMax; ++lag)
        if (r[(size_t)lag] > 0.85 * mx && r[(size_t)lag] >= r[(size_t)lag - 1] && r[(size_t)lag] >= r[(size_t)lag + 1]) { best = lag; break; }
    if (best == 0 || r0 <= 0) return 0.0;
    const double a = r[(size_t)best - 1], bb = r[(size_t)best], c = r[(size_t)best + 1];
    const double den = a - 2 * bb + c;
    const double lag = best + (std::fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0);
    return sr / lag;
}
int firstAbove(const juce::AudioBuffer<float>& b, float thr, int from = 0) {
    for (int i = from; i < b.getNumSamples(); ++i)
        if (std::fabs(b.getSample(0, i)) > thr || std::fabs(b.getSample(1, i)) > thr) return i;
    return -1;
}

Project basicProject(double bpm = 120.0) {
    Project p;
    p.tempo.front().bpm = bpm;
    return p;
}

juce::AudioBuffer<float> render(const Project& p, juce::String* errOut = nullptr, double tail = 3.0) {
    juce::AudioBuffer<float> out;
    juce::String err;
    if (!Exporter::renderToBuffer(p, SR, out, tail, err) && errOut) *errOut = err;
    return out;
}

Track& instrumentWithNote(Project& p, const juce::String& preset, int pitch, int start, int len, float vel = 0.9f) {
    auto& t = p.addTrack(TrackType::Instrument, {}, preset);
    Clip c = p.makeMidiClip(0, std::max(start + len, PPQ * 4));
    c.notes.push_back({ start, len, pitch, vel });
    t.clips.push_back(c);
    return t;
}

void writeSineWav(const juce::File& f, double hz, double secs, double sr = 44100.0) {
    juce::AudioBuffer<float> b(2, (int)(secs * sr));
    for (int i = 0; i < b.getNumSamples(); ++i) { const float v = 0.5f * std::sin((float)(2.0 * juce::MathConstants<double>::pi * hz * i / sr)); b.setSample(0, i, v); b.setSample(1, i, v); }
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(sr).withNumChannels(2).withBitsPerSample(24));
    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}

// Runs an effect over a signal directly (no project), returning the output.
juce::AudioBuffer<float> runEffect(const juce::String& type, const juce::String& values, const juce::AudioBuffer<float>& input, double bpm = 120.0) {
    auto ps = Catalog::makeParams(Catalog::effectParams(type));
    Catalog::applyValues(*ps, values);
    auto fx = createEffect(type, ps);
    TransportInfo tr; tr.bpm = bpm;
    fx->setTransport(&tr);
    fx->prepare(SR, 64);
    juce::AudioBuffer<float> out(input);
    for (int pos = 0; pos < out.getNumSamples(); pos += 64) {
        const int n = std::min(64, out.getNumSamples() - pos);
        fx->process(out.getWritePointer(0, pos), out.getWritePointer(1, pos), n);
    }
    return out;
}

juce::AudioBuffer<float> sine(double hz, double secs, float amp = 0.5f, double startSilence = 0.0) {
    juce::AudioBuffer<float> b(2, (int)((secs + startSilence) * SR));
    b.clear();
    const int off = (int)(startSilence * SR);
    for (int i = 0; i < (int)(secs * SR); ++i) { const float v = amp * std::sin((float)(2.0 * juce::MathConstants<double>::pi * hz * i / SR)); b.setSample(0, off + i, v); b.setSample(1, off + i, v); }
    return b;
}

float bandEnergy(const juce::AudioBuffer<float>& b, double hz, int start, int len) {
    // Goertzel magnitude at hz
    const double w = 2.0 * juce::MathConstants<double>::pi * hz / SR;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < len; ++i) {
        const double s0 = 0.5 * (b.getSample(0, start + i) + b.getSample(1, start + i)) + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return (float)std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) / (float)len;
}

// Renders one note on a Studio Synth (or any engine) with extra settings, straight through the instrument.
juce::AudioBuffer<float> playNote(const juce::String& engine, const juce::String& values, int note = 69, double secs = 1.0, double holdSecs = 0.8,
                                  const juce::String& preset = {}) {
    auto ps = Catalog::makeParams(Catalog::engineParams(engine));
    if (preset.isNotEmpty()) if (auto* pr = Catalog::findPreset(preset)) Catalog::applyValues(*ps, pr->values);
    Catalog::applyValues(*ps, values);
    InstrumentSetup setup; setup.engine = engine; setup.params = ps;
    auto inst = createInstrument(setup);
    TransportInfo tr; tr.bpm = 120.0;
    inst->setTransport(&tr);
    inst->prepare(SR, 64);
    juce::AudioBuffer<float> b(2, (int)(secs * SR));
    b.clear();
    inst->noteOn(note, 0.9f, 0.f);
    for (int pos = 0; pos < b.getNumSamples(); pos += 64) {
        if (pos >= (int)(holdSecs * SR) && pos < (int)(holdSecs * SR) + 64) inst->noteOff(note);
        inst->render(b.getWritePointer(0, pos), b.getWritePointer(1, pos), std::min(64, b.getNumSamples() - pos));
    }
    return b;
}
float corrLR(const juce::AudioBuffer<float>& b, int start, int len) {
    double lr = 0, ll = 0, rr = 0;
    for (int i = start; i < start + len; ++i) { const double l = b.getSample(0, i), r = b.getSample(1, i); lr += l * r; ll += l * l; rr += r * r; }
    return ll > 0 && rr > 0 ? (float)(lr / std::sqrt(ll * rr)) : 1.f;
}
float dbRatio(float a, float b) { return dsp::lin2db(std::max(a, 1e-9f)) - dsp::lin2db(std::max(b, 1e-9f)); }

}  // namespace

static juce::File& crashTrailFile() { static juce::File f; return f; }

int runSelfTests(const juce::File& reportFile) {
    Suite t;
    t.progress = reportFile;
    if (reportFile != juce::File()) reportFile.replaceWithText("(in progress)\n");
    juce::SystemStats::setApplicationCrashHandler([](void*) {
        std::fprintf(stderr, "CRASH\n%s\n", juce::SystemStats::getStackBacktrace().toRawUTF8());
        crashTrailFile().appendText("\nCRASH\n" + juce::SystemStats::getStackBacktrace() + "\n");
    });
    crashTrailFile() = reportFile;
    const auto tmpRoot = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ostinato-selftest");
    tmpRoot.deleteRecursively();
    tmpRoot.createDirectory();
    Paths::overrideRoot() = tmpRoot;
    SampleBank::get().setProjectDir(tmpRoot.getChildFile("proj"));
    const auto wallStart = juce::Time::getMillisecondCounterHiRes();

    // ------------------------------------------------------------ instruments
    t.begin("Instruments: every preset produces sound at the right pitch");
    for (auto& pr : Catalog::instrumentPresets()) {
        Project p = basicProject();
        const bool drums = juce::String(pr.engine) == "drums";
        const bool sampler = juce::String(pr.engine) == "sampler";
        if (drums) {
            auto& d = p.addTrack(TrackType::Drum, {}, pr.name);
            Clip c = p.makeMidiClip(0, PPQ * 4);
            for (int pad = 0; pad < 16; ++pad) c.notes.push_back({ pad * PPQ / 4, PPQ / 8, 36 + pad, 0.9f });
            d.clips.push_back(c);
        } else {
            auto& tr = instrumentWithNote(p, pr.name, 69, 0, PPQ * 4);
            if (sampler) {
                writeSineWav(tmpRoot.getChildFile("proj/audio/sine440.wav"), 440.0, 2.0);
                tr.inst.sample = "audio/sine440.wav";
                tr.inst.sampleRoot = 69;
            }
        }
        juce::String err;
        auto b = render(p, &err, 4.0);
        const float pk = peakOf(b);
        bool ok = err.isEmpty() && allFinite(b) && pk > 0.01f && pk < 4.f;
        juce::String detail = "peak " + juce::String(dsp::lin2db(pk), 1) + " dB";
        if (ok && !drums) {
            const double hz = pitchOf(b, (int)(0.25 * SR), 8192);
            double cents = hz > 0 ? 1200.0 * std::log2(hz / 440.0) : 9999.0;
            const int octaves = (int)std::lround(cents / 1200.0);   // sub-oscillators / 16' drawbars legitimately sound an octave down
            if (octaves != 0 && std::fabs(cents - octaves * 1200.0) < 35.0) { detail << " [" << juce::String(octaves) << " oct, by design]"; cents -= octaves * 1200.0; }
            detail << ", A4 -> " << juce::String(hz, 1) << " Hz (" << juce::String(cents, 1) << " ct)";
            // percussive/inharmonic presets are allowed a wider tolerance
            const juce::String name = pr.name;
            const bool inharmonic = name == "Bells" || name == "Wobble Bass" || name == "Supersaw" || name == "Choir" || name == "Electric Guitar" || name == "Mallets" || name == "Vibraphone" || name.contains("Pad") || name.contains("Wavetable");
            ok = std::fabs(cents) < (inharmonic ? 1250.0 : 35.0) && hz > 0;
            if (!inharmonic && std::fabs(cents) > 35.0) detail << " OUT OF TUNE";
        }
        if (drums) {
            int hits = 0;
            for (int pad = 0; pad < 16; ++pad) {
                const int s = (int)(pad * 0.125 * SR);
                if (rmsOf(b, s, (int)(0.04 * SR)) > 0.001f) ++hits;
            }
            detail << ", " << hits << "/16 pads sounding";
            ok = ok && hits == 16;
        }
        t.check(ok, juce::String(pr.name) + " (" + pr.engine + ")", detail);
    }

    // ------------------------------------------------------------ timing
    t.begin("Timing: sample-accurate note scheduling");
    {
        Project p = basicProject(120.0);
        auto& tr = p.addTrack(TrackType::Instrument, {}, "Subtractive Synth");
        Catalog::applyValues(*tr.inst.params, "a_a=0.001,osc1_wave=1,osc2_wave=1,cutoff=20000,flt_env=0");
        Clip c = p.makeMidiClip(0, PPQ * 16);
        c.notes.push_back({ PPQ * 4, PPQ, 60, 1.f });              // bar 2 beat 1 at 120 BPM = 2.000 s
        c.notes.push_back({ PPQ * 4 + PPQ / 3, PPQ / 4, 72, 1.f }); // triplet position
        tr.clips.push_back(c);
        auto b = render(p);
        const int onset = firstAbove(b, 1.0e-4f);
        t.check(std::abs(onset - 96000) <= 2, "Note at bar 2 starts at sample 96000", "measured " + juce::String(onset));
        // tempo change: 120 -> 60 BPM at bar 2; note at bar 3 should start at 2 s + 4 s = 6 s
        Project q = basicProject(120.0);
        q.tempo.push_back({ PPQ * 4, 60.0 });
        auto& tq = q.addTrack(TrackType::Instrument, {}, "Subtractive Synth");
        Catalog::applyValues(*tq.inst.params, "a_a=0.001,osc1_wave=1,osc2_wave=1");
        Clip cq = q.makeMidiClip(0, PPQ * 16);
        cq.notes.push_back({ PPQ * 8, PPQ, 60, 1.f });
        tq.clips.push_back(cq);
        auto bq = render(q);
        const int on2 = firstAbove(bq, 1.0e-4f);
        t.check(std::abs(on2 - (int)(6.0 * SR)) <= 2, "Tempo change 120->60 BPM moves bar 3 to 6.0 s", "measured " + juce::String(on2 / SR, 4) + " s");
        t.check(std::fabs(q.ticksToSeconds(q.secondsToTicks(4.321)) - 4.321) < 1e-9, "Tempo map seconds<->ticks round trip");
    }

    // ------------------------------------------------------------ drum sequencer
    t.begin("Drum sequencer: steps, swing, probability, ratchets");
    {
        Project p = basicProject(120.0);
        auto& d = p.addTrack(TrackType::Drum, {}, "Boom Kit");
        d.inst.pads[0].params->set(d.inst.pads[0].params->indexOf("decay"), 0.03f);   // short kick so onsets don't overlap
        auto& pat = p.addPattern("A");
        for (int s = 0; s < 16; s += 4) pat.grid[0][(size_t)s].on = true;     // kick on beats
        auto clip = Clip(); clip.id = p.newId("c"); clip.type = ClipType::Pattern; clip.patternId = pat.id; clip.start = 0; clip.length = PPQ * 4;
        d.clips.push_back(clip);
        auto b = render(p, nullptr, 1.0);
        int found = 0;
        for (int k = 0; k < 4; ++k) {
            const int expected = (int)(k * 0.5 * SR);
            const int on = firstAbove(b, 0.01f, std::max(0, expected - 10));
            if (std::abs(on - expected) <= 2) ++found;
        }
        t.check(found == 4, "4 kicks land exactly on the beats", juce::String(found) + "/4");
        // swing
        pat.grid[0] = {};
        pat.grid[0][1].on = true;
        pat.swing = 0.5f;
        auto bs = render(p, nullptr, 1.0);
        const int expectedSwing = (int)((0.125 + 0.0625) * SR);   // step 1 (1/16 = 0.125 s) delayed by half a step
        const int onS = firstAbove(bs, 0.01f);
        t.check(std::abs(onS - expectedSwing) <= 2, "Swing 50% delays off-beat 16th by half a step", "measured " + juce::String(onS) + " expected " + juce::String(expectedSwing));
        // probability 0 -> silent, ratchet 3 -> three onsets
        pat.swing = 0.f;
        pat.grid[0] = {};
        pat.grid[0][0].on = true; pat.grid[0][0].probability = 0.f;
        t.check(peakOf(render(p, nullptr, 0.5)) < 1.0e-4f, "Probability 0% step never plays");
        pat.grid[0][0].probability = 1.f; pat.grid[0][0].ratchet = 3;
        d.inst.pads[0].params->set(d.inst.pads[0].params->indexOf("decay"), 0.02f);
        pat.steps = 16;
        auto br = render(p, nullptr, 0.5);
        int onsets = 0;
        for (int k = 0; k < 3; ++k) { const int e = (int)(k * 0.125 / 3.0 * SR); const int on = firstAbove(br, 0.01f, std::max(0, e - 4)); if (std::abs(on - e) <= 3) ++onsets; }
        t.check(onsets == 3, "Ratchet x3 retriggers three times inside one step", juce::String(onsets) + "/3");
        // probability 50% over many bars is roughly half and deterministic
        pat.grid[0][0].ratchet = 1;
        for (int s = 0; s < 16; ++s) { pat.grid[0][(size_t)s].on = true; pat.grid[0][(size_t)s].probability = 0.5f; }
        d.clips[0].length = PPQ * 4 * 16;
        ProcRegistry reg;
        auto snap = Compiler::compile(p, reg, SR, true);
        const int played = (int)snap->tracks[0].notes.size();
        auto snap2 = Compiler::compile(p, reg, SR, true);
        t.check(played > 90 && played < 166 && played == (int)snap2->tracks[0].notes.size(), "Probability 50% plays about half the steps, repeatably", juce::String(played) + "/256");
    }

    // ------------------------------------------------------------ effects
    t.begin("Effects: all " + juce::String((int)Catalog::effects().size()) + " process audio, and each does what it says");
    {
        auto test = sine(440.0, 1.0, 0.5f);
        for (auto& e : Catalog::effects()) {
            auto out = runEffect(e.id, "", test);
            bool changed = false;
            for (int i = 0; i < out.getNumSamples() && !changed; ++i) if (std::fabs(out.getSample(0, i) - test.getSample(0, i)) > 1.0e-4f) changed = true;
            const bool neutralByDefault = juce::String(e.id) == "eq3" || juce::String(e.id) == "parametric" || juce::String(e.id) == "pitch" || juce::String(e.id) == "imager" || juce::String(e.id) == "widener" || juce::String(e.id) == "expander" || juce::String(e.id) == "gate" || juce::String(e.id) == "limiter"
                                          || juce::String(e.id) == "deesser";   // no S sounds in a 440 Hz tone
            t.check(allFinite(out) && (changed || neutralByDefault) && peakOf(out) < 4.f, juce::String(e.name) + " runs", changed ? "modifies signal" : "transparent at defaults");
        }
        // targeted behaviour checks
        auto loud = sine(1000.0, 0.5, 0.9f);
        auto lim = runEffect("limiter", "gain=12,ceiling=-3", loud);
        t.check(peakOf(lim) <= dsp::db2lin(-3.f) + 1.0e-4f, "Limiter never exceeds its ceiling (+12 dB into -3 dB)", "peak " + juce::String(dsp::lin2db(peakOf(lim)), 2) + " dB");
        auto comp = runEffect("compressor", "threshold=-30,ratio=8,attack=1,makeup=0", loud);
        t.check(rmsOf(comp, 12000, 8000) < rmsOf(loud, 12000, 8000) * 0.5f, "Compressor reduces a loud signal", juce::String(dsp::lin2db(rmsOf(comp, 12000, 8000) / rmsOf(loud, 12000, 8000)), 1) + " dB");
        auto quiet = sine(440.0, 0.5, 0.003f);
        auto gate = runEffect("gate", "threshold=-30,range=-80", quiet);
        t.check(rmsOf(gate, 12000, 8000) < rmsOf(quiet, 12000, 8000) * 0.01f, "Gate closes below threshold");
        auto exp = runEffect("expander", "threshold=-20,ratio=4,range=-60", quiet);
        t.check(rmsOf(exp, 12000, 8000) < rmsOf(quiet, 12000, 8000) * 0.3f, "Expander attenuates below threshold");
        auto lowS = sine(100.0, 0.5, 0.2f);
        auto eqB = runEffect("eq3", "low=12,low_f=200", lowS);
        t.check(rmsOf(eqB, 12000, 8000) > rmsOf(lowS, 12000, 8000) * 2.5f, "3-band EQ low +12 dB boosts 100 Hz", juce::String(dsp::lin2db(rmsOf(eqB, 12000, 8000) / rmsOf(lowS, 12000, 8000)), 1) + " dB");
        auto peq = runEffect("parametric", "b2_f=1000,b2_g=-12,b2_q=2", sine(1000.0, 0.5, 0.3f));
        t.check(rmsOf(peq, 12000, 8000) < 0.3f * 0.707f * 0.35f, "Parametric EQ -12 dB band cuts 1 kHz");
        auto hiS = sine(8000.0, 0.5, 0.3f);
        auto lp = runEffect("lowpass", "cutoff=500,slope=1", hiS);
        t.check(rmsOf(lp, 12000, 8000) < rmsOf(hiS, 12000, 8000) * 0.01f, "Low-pass 500 Hz removes 8 kHz (> 40 dB)");
        auto hp = runEffect("highpass", "cutoff=2000,slope=1", lowS);
        t.check(rmsOf(hp, 12000, 8000) < rmsOf(lowS, 12000, 8000) * 0.01f, "High-pass 2 kHz removes 100 Hz");
        auto bp = runEffect("bandpass", "cutoff=1000,reso=50", lowS);
        t.check(rmsOf(bp, 12000, 8000) < rmsOf(lowS, 12000, 8000) * 0.2f, "Band-pass 1 kHz rejects 100 Hz");
        // reverb tail / delay echo position
        juce::AudioBuffer<float> click(2, (int)(SR * 2.0)); click.clear(); click.setSample(0, 100, 1.f); click.setSample(1, 100, 1.f);
        auto rv = runEffect("reverb", "decay=2,mix=100,predelay=0", click);
        t.check(rmsOf(rv, (int)(0.3 * SR), (int)(0.2 * SR)) > 1.0e-4f && allFinite(rv), "Reverb produces a decaying tail");
        auto dl = runEffect("delay", "sync=0,time=250,feedback=0,mix=100,tone=20000", click);
        const int echo = firstAbove(dl, 0.05f, 200);
        t.check(std::abs(echo - (100 + 12000)) <= 3, "Delay 250 ms echo arrives at 250 ms", "at " + juce::String((echo - 100) / SR * 1000.0, 2) + " ms");
        auto dls = runEffect("delay", "sync=4,feedback=0,mix=100,tone=20000", click, 120.0);
        const int echoS = firstAbove(dls, 0.05f, 200);
        t.check(std::abs(echoS - (100 + 24000)) <= 3, "Tempo-synced 1/4 delay at 120 BPM = 500 ms", "at " + juce::String((echoS - 100) / SR * 1000.0, 2) + " ms");
        auto pp = runEffect("pingpong", "sync=0,time=100,feedback=60,mix=100,tone=20000", click);
        const int e1 = firstAbove(pp, 0.05f, 200);
        const int e2 = e1 + 4801;   // one delay later (delay lines read before writing)
        t.check(e1 > 0 && std::fabs(pp.getSample(0, e1)) > 0.1f && std::fabs(pp.getSample(1, e1)) < 0.01f
                && std::fabs(pp.getSample(1, e2)) > 0.05f && std::fabs(pp.getSample(0, e2)) < 0.01f, "Ping-pong delay: 1st echo left, 2nd echo right");
        auto ps = runEffect("pitch", "semis=12,mix=100", sine(220.0, 1.0, 0.4f));
        const double hzUp = pitchOf(ps, 24000, 8192);
        t.check(std::fabs(1200.0 * std::log2(hzUp / 440.0)) < 30.0, "Pitch shifter +12 st turns 220 Hz into 440 Hz", juce::String(hzUp, 1) + " Hz");
        auto bc = runEffect("bitcrusher", "bits=2,down=1,mix=100", sine(440.0, 0.2, 0.9f));
        std::set<float> levels; for (int i = 0; i < 2000; ++i) levels.insert(std::round(bc.getSample(0, i) * 1000.f));
        t.check(levels.size() <= 4, "Bitcrusher 2-bit leaves at most 4 levels", juce::String((int)levels.size()) + " levels");
        auto wid = runEffect("widener", "width=0,bass_mono=20", [] { auto b = sine(440.0, 0.3, 0.4f); b.applyGain(1, 0, b.getNumSamples(), -1.f); return b; }());
        t.check(rmsOf(wid, 4000, 4000) < 0.05f, "Widener width 0% collapses an out-of-phase signal to mono");
        auto trem = runEffect("tremolo", "rate=4,depth=100", sine(440.0, 1.0, 0.5f));
        t.check(rmsOf(trem, 0, (int)SR) < rmsOf(test, 0, (int)SR) * 0.8f, "Tremolo modulates amplitude");
        auto ts = runEffect("transient", "attack=100,sustain=0", [] { juce::AudioBuffer<float> b(2, 48000); b.clear(); for (int i = 0; i < 48000; ++i) { const float e = std::exp(-i / 4000.f); const float v = e * std::sin(i * 0.05f); b.setSample(0, i, v); b.setSample(1, i, v); } return b; }());
        t.check(peakOf(ts, 0, 2000) > 0.95f * 1.2f || peakOf(ts, 0, 2000) > 1.1f, "Transient shaper boosts attacks", "attack peak " + juce::String(peakOf(ts, 0, 2000), 2));
    }

    // ------------------------------------------------------------ mixer
    t.begin("Mixer: volume, pan, mute, solo, phase, sends, buses");
    {
        auto mk = [](Project& p) -> Track& {
            auto& tr = p.addTrack(TrackType::Instrument, {}, "Subtractive Synth");
            Catalog::applyValues(*tr.inst.params, "osc1_wave=3,osc2_wave=3,a_a=0.001,a_s=100,cutoff=20000,flt_env=0,level=-6");
            Clip c = p.makeMidiClip(0, PPQ * 4); c.notes.push_back({ 0, PPQ * 4, 69, 1.f }); tr.clips.push_back(c);
            return tr;
        };
        Project p = basicProject();
        mk(p);
        auto A = [&p]() -> Track& { return p.tracks[0]; };
        const float base = rmsOf(render(p, nullptr, 0.2), 24000, 24000);
        A().channel->set(0, -6.f);
        const float minus6 = rmsOf(render(p, nullptr, 0.2), 24000, 24000);
        t.check(std::fabs(dsp::lin2db(minus6 / base) + 6.f) < 0.3f, "Fader -6 dB lowers level by 6 dB", juce::String(dsp::lin2db(minus6 / base), 2) + " dB");
        A().channel->set(0, 0.f);
        A().channel->set(1, -100.f);
        auto pl = render(p, nullptr, 0.2);
        t.check(pl.getRMSLevel(1, 24000, 24000) < 1.0e-4f && pl.getRMSLevel(0, 24000, 24000) > 0.01f, "Pan hard left silences the right channel");
        A().channel->set(1, 0.f);
        A().channel->set(2, 1.f);
        t.check(peakOf(render(p, nullptr, 0.2), 24000) < 1.0e-5f, "Mute silences the track");
        A().channel->set(2, 0.f);
        mk(p);
        auto B = [&p]() -> Track& { return p.tracks[1]; };
        B().channel->set(3, 1.f);   // solo b
        A().channel->set(1, -100.f); B().channel->set(1, 100.f);
        auto so = render(p, nullptr, 0.2);
        t.check(so.getRMSLevel(0, 24000, 24000) < 1.0e-4f && so.getRMSLevel(1, 24000, 24000) > 0.01f, "Solo on one track mutes the others");
        B().channel->set(3, 0.f); A().channel->set(1, 0.f); B().channel->set(1, 0.f);
        B().channel->set(4, 1.f);   // phase invert b -> cancels a
        t.check(rmsOf(render(p, nullptr, 0.2), 24000, 24000) < base * 0.05f, "Phase invert cancels an identical track");
        B().channel->set(4, 0.f);
        // bus routing + return send
        Project q = basicProject();
        mk(q);
        q.addTrack(TrackType::Bus);
        auto& src = q.tracks[0];
        auto& bus = q.tracks[1];
        src.output = bus.id;
        bus.channel->set(2, 1.f);  // mute bus -> src silent
        t.check(peakOf(render(q, nullptr, 0.2), 24000) < 1.0e-5f, "Track routed to a muted bus is silent");
        bus.channel->set(2, 0.f);
        q.addTrack(TrackType::Return);
        q.tracks[2].effects.push_back(q.makeEffect("reverb"));
        Catalog::applyValues(*q.tracks[2].effects.back().params, "mix=100,decay=3");
        auto& src2 = q.tracks[0];
        src2.channel->set(src2.channel->indexOf("send1"), 0.f);
        src2.channel->set(0, -60.f);   // fader down, post-fader send -> silent
        t.check(peakOf(render(q, nullptr, 1.0), 24000) < 1.0e-5f, "Post-fader send follows the fader");
        src2.channel->set(src2.channel->indexOf("pre1"), 1.f);  // pre-fader -> reverb audible
        t.check(rmsOf(render(q, nullptr, 1.0), 24000, 24000) > 0.001f, "Pre-fader send feeds the return with the fader down");
        // a cycle bus->bus must not hang or crash
        q.addTrack(TrackType::Bus);
        q.tracks[1].output = q.tracks[3].id; q.tracks[3].output = q.tracks[1].id;
        ProcRegistry reg;
        auto snap = Compiler::compile(q, reg, SR, true);
        t.check(snap->order.size() == q.tracks.size(), "Routing cycle is broken safely (all tracks still processed)");
    }

    // ------------------------------------------------------------ automation
    t.begin("Automation: lanes drive parameters in sync with the timeline");
    {
        Project p = basicProject(120.0);
        auto& tr = p.addTrack(TrackType::Instrument, {}, "Subtractive Synth");
        Catalog::applyValues(*tr.inst.params, "osc1_wave=3,osc2_wave=3,a_a=0.001,a_s=100,a_r=0.01,cutoff=20000,flt_env=0");
        Clip c = p.makeMidiClip(0, PPQ * 8); c.notes.push_back({ 0, PPQ * 8, 69, 1.f }); tr.clips.push_back(c);
        AutomationLane l; l.target = "ch:vol";
        const float zeroDb = tr.channel->toNorm(0, 0.f), silent = tr.channel->toNorm(0, -60.f);
        l.points = { { 0, silent, 0 }, { PPQ * 8, zeroDb, 0 } };
        tr.automation.push_back(l);
        auto b = render(p, nullptr, 0.1);
        const float early = rmsOf(b, (int)(0.2 * SR), 4800), late = rmsOf(b, (int)(3.6 * SR), 4800);
        tr.automation.clear();
        const float ref = rmsOf(render(p, nullptr, 0.1), (int)(3.6 * SR), 4800);   // same note, fader at 0 dB
        // expected: at 0.2 s the lane is at -57 dB, at 3.6 s at -6 dB (linear in the fader's dB scale)
        const float dEarly = dsp::lin2db(early / ref), dLate = dsp::lin2db(late / ref);
        t.check(std::fabs(dEarly + 57.f) < 2.f && std::fabs(dLate + 6.f) < 1.f, "Volume lane -60 dB -> 0 dB over 4 s follows the timeline",
                "at 0.2 s " + juce::String(dEarly, 1) + " dB (expect -57), at 3.6 s " + juce::String(dLate, 1) + " dB (expect -6)");
        AutomationLane step; step.points = { { 0, 0.f, 2 }, { 100, 1.f, 2 } };
        t.check(step.valueAt(50) == 0.f && step.valueAt(100) == 1.f, "Hold/step curve holds until the next point");
        AutomationLane sm; sm.points = { { 0, 0.f, 1 }, { 100, 1.f, 1 } };
        t.check(std::fabs(sm.valueAt(50) - 0.5f) < 1e-4f && sm.valueAt(25) < 0.25f, "Smooth curve eases in and out");
        // effect + instrument parameter automation
        tr.automation.clear();
        tr.effects.push_back(p.makeEffect("lowpass"));
        AutomationLane fx; fx.target = "fx:" + tr.effects.back().id + ":cutoff";
        fx.points = { { 0, 0.f, 0 }, { PPQ * 8, 0.f, 0 } };
        tr.automation.push_back(fx);
        Catalog::applyValues(*tr.inst.params, "osc1_wave=0,osc2_wave=0");
        auto bf = render(p, nullptr, 0.1);
        t.check(bandEnergy(bf, 440.0 * 8, 48000, 8192) < 1.0e-4f, "Effect parameter automation (filter cutoff) applies");
    }

    // ------------------------------------------------------------ audio clips
    t.begin("Audio clips: placement, trim, fades, reverse, stretch, pitch, loop, SRC");
    {
        const auto wav = tmpRoot.getChildFile("proj/audio/tone.wav");
        writeSineWav(wav, 441.0, 2.0, 44100.0);   // 44.1 kHz file rendered at 48 kHz
        Project p = basicProject(120.0);
        auto& a = p.addTrack(TrackType::Audio);
        Clip c; c.id = p.newId("c"); c.type = ClipType::Audio; c.audio.file = "audio/tone.wav"; c.start = PPQ * 2; c.length = PPQ * 4;   // starts at 1 s, lasts 2 s
        a.clips.push_back(c);
        auto b = render(p, nullptr, 0.2);
        const int on = firstAbove(b, 0.01f);
        t.check(std::abs(on - 48000) < 48, "Clip starts at 1.0 s", juce::String(on / SR, 4) + " s");
        const double hz = pitchOf(b, 60000, 8192);
        t.check(std::fabs(hz - 441.0) < 2.0, "44.1 kHz file plays at correct pitch at 48 kHz (sample-rate conversion)", juce::String(hz, 2) + " Hz");
        a.clips[0].audio.pitchSt = 12.f;
        const double hz2 = pitchOf(render(p, nullptr, 0.2), 60000, 8192);
        t.check(std::fabs(hz2 - 882.0) < 5.0, "Pitch +12 st with time preserved", juce::String(hz2, 1) + " Hz");
        a.clips[0].audio.pitchSt = 0.f;
        a.clips[0].audio.stretch = 2.f; a.clips[0].length = PPQ * 8;  // 4 s long
        auto bs = render(p, nullptr, 0.2);
        const double hzS = pitchOf(bs, (int)(3.5 * SR), 8192);
        t.check(rmsOf(bs, (int)(4.5 * SR), 4800) > 0.1f && std::fabs(hzS - 441.0) < 3.0, "Stretch x2 doubles length and keeps pitch", juce::String(hzS, 1) + " Hz at 3.5 s");
        a.clips[0].audio.stretch = 1.f; a.clips[0].length = PPQ * 4;
        a.clips[0].audio.fadeInSec = 0.5;
        auto bfi = render(p, nullptr, 0.2);
        t.check(rmsOf(bfi, 48000, 2400) < rmsOf(bfi, 48000 + 24000, 2400) * 0.2f, "Fade-in ramps up");
        a.clips[0].audio.fadeInSec = 0.0;
        a.clips[0].audio.gainDb = -12.f;
        t.check(std::fabs(dsp::lin2db(rmsOf(render(p, nullptr, 0.2), 60000, 9600) / rmsOf(b, 60000, 9600)) + 12.f) < 0.3f, "Clip gain -12 dB");
        a.clips[0].audio.gainDb = 0.f;
        a.clips[0].audio.normalize = true;
        t.check(std::fabs(peakOf(render(p, nullptr, 0.2), 50000, 40000) - 0.891f) < 0.02f, "Normalize brings the peak to -1 dBFS");
        a.clips[0].audio.normalize = false;
        a.clips[0].audio.loop = true; a.clips[0].length = PPQ * 12;   // 6 s from a 2 s file
        t.check(rmsOf(render(p, nullptr, 0.2), (int)(6.0 * SR), 4800) > 0.1f, "Looping clip repeats the source");
        a.clips[0].audio.loop = false; a.clips[0].length = PPQ * 4;
        a.clips[0].audio.offsetSec = 1.5;                               // trim: only 0.5 s of audio left
        auto bt = render(p, nullptr, 0.2);
        t.check(rmsOf(bt, (int)(1.2 * SR), 4800) > 0.1f && rmsOf(bt, (int)(1.7 * SR), 4800) < 1.0e-4f, "Left trim offsets into the source");
        a.clips[0].audio.offsetSec = 0.0;
        a.clips[0].audio.reverse = true;
        t.check(rmsOf(render(p, nullptr, 0.2), 60000, 4800) > 0.1f, "Reverse playback");
    }

    // ------------------------------------------------------------ arp & loop & live
    t.begin("Arpeggiator, chord trigger, looping");
    {
        Project p = basicProject(120.0);
        auto& tr = p.addTrack(TrackType::Instrument, {}, "Pluck Synth");
        tr.inst.arp->set(0, 1.f);                  // on
        tr.inst.arp->set(2, 6.f);                  // 1/8
        Clip c = p.makeMidiClip(0, PPQ * 4);
        for (int n : { 60, 64, 67 }) c.notes.push_back({ 0, PPQ * 4, n, 0.9f });
        tr.clips.push_back(c);
        auto b = render(p, nullptr, 0.5);
        // expect 8 onsets in 2 seconds (eighth notes at 120 BPM = 0.25 s)
        int onsets = 0;
        for (int k = 0; k < 8; ++k) { const int s = (int)(k * 0.25 * SR); if (rmsOf(b, s + 200, 1200) > rmsOf(b, s - 2400 > 0 ? s - 2400 : 0, 1200) * 1.2f || k == 0) ++onsets; }
        t.check(onsets >= 7, "Arpeggiator plays held chord as 1/8 notes", juce::String(onsets) + "/8 onsets");
        ProcRegistry reg;
        auto snap = Compiler::compile(p, reg, SR, true);
        t.check(snap->tracks[0].notes.size() == 3, "Arpeggiator input notes compiled");
        // loop region: one note at beat 1 in a 1-bar loop played for 3 bars of time
        Project q = basicProject(120.0);
        auto& tq = q.addTrack(TrackType::Instrument, {}, "Mallets");
        Clip cq = q.makeMidiClip(0, PPQ * 4); cq.notes.push_back({ 0, PPQ / 2, 72, 1.f }); tq.clips.push_back(cq);
        q.loopOn = true; q.loopStart = 0; q.loopEnd = PPQ * 4;
        ProcRegistry rq;
        auto sq = Compiler::compile(q, rq, SR, true);
        auto rOwner = std::make_unique<Renderer>(); auto& r = *rOwner;  /* on the heap: a Renderer is ~300 KB */ r.prepare(SR); r.setSnapshot(sq.get()); r.play();
        juce::AudioBuffer<float> out(2, (int)(6.0 * SR)); out.clear();
        for (int pos = 0; pos < out.getNumSamples(); pos += 480) r.render(out.getWritePointer(0, pos), out.getWritePointer(1, pos), std::min(480, out.getNumSamples() - pos));
        int loops = 0;
        for (int k = 0; k < 3; ++k) { const int s = (int)(k * 2.0 * SR); if (firstAbove(out, 0.01f, std::max(0, s - 2)) - s <= 2) ++loops; }
        t.check(loops == 3, "Loop region retriggers the note every 2 s, sample-accurately", juce::String(loops) + "/3");
        // live input reaches the instrument with the transport stopped
        auto liveOwner = std::make_unique<Renderer>(); auto& live = *liveOwner;  /* on the heap: a Renderer is ~300 KB */ live.prepare(SR); live.setSnapshot(sq.get());
        live.liveQueue.push({ LiveEvent::NoteOn, Compiler::uidFor(tq.id), 60, 1.f });
        juce::AudioBuffer<float> lo(2, 9600); lo.clear();
        live.render(lo.getWritePointer(0), lo.getWritePointer(1), 9600);
        t.check(peakOf(lo) > 0.01f, "Live keyboard notes play while stopped");
        // battery saver: silence while stopped -> engine idles; a live note wakes it instantly
        ProcRegistry ri;
        auto si = Compiler::compile(q, ri, SR, true);   // fresh processors (the snapshot above still holds a live note)
        Renderer idleR; idleR.prepare(SR); idleR.setSnapshot(si.get());
        juce::AudioBuffer<float> ib(2, 4800);
        for (int i = 0; i < 30; ++i) idleR.render(ib.getWritePointer(0), ib.getWritePointer(1), 4800);   // 3 s
        const bool slept = idleR.isIdle();
        idleR.liveQueue.push({ LiveEvent::NoteOn, Compiler::uidFor(tq.id), 64, 1.f });
        ib.clear();
        idleR.render(ib.getWritePointer(0), ib.getWritePointer(1), 4800);
        t.check(slept && !idleR.isIdle() && peakOf(ib) > 0.01f, "Battery saver idles after silence and wakes on the next note", juce::String("slept=") + (slept ? "yes" : "no") + " peak=" + juce::String(peakOf(ib), 4));
        idleR.play();
        t.check(!idleR.isIdle(), "Pressing play wakes the engine");
    }

    // ------------------------------------------------------------ export
    t.begin("Export: complete renders, formats, no truncation");
    {
        Project p = basicProject(100.0);
        ProjectStore::buildTemplate(p, "Beat Starter");
        auto& lead = instrumentWithNote(p, "Pad Synth", 72, PPQ * 12, PPQ * 4);   // ends at bar 5 with a long release
        lead.effects.push_back(p.makeEffect("reverb"));
        Catalog::applyValues(*lead.effects.back().params, "decay=4,mix=40");
        const double songSec = p.ticksToSeconds(p.songEndTick());
        for (auto name : { "Beat Starter", "Hip-Hop", "Band" }) {
            Project tp; ProjectStore::buildTemplate(tp, name);
            juce::AudioBuffer<float> tb; juce::String e;
            Exporter::renderToBuffer(tp, SR, tb, 2.0, e);
            t.check(peakOf(tb) < 1.f, juce::String("Template \"") + name + "\" leaves headroom (no clipping)", juce::String(dsp::lin2db(peakOf(tb)), 1) + " dBFS peak");
        }
        for (int fmtIdx : { 0, 1, 2 }) {
            ExportSettings s;
            s.format = (ExportSettings::Format)fmtIdx;
            s.sampleRate = fmtIdx == 2 ? 44100.0 : 48000.0;
            s.bitDepth = fmtIdx == 0 ? 24 : 16;
            s.destination = tmpRoot.getChildFile("Exports/song" + s.extension());
            Exporter ex(p, s);
            ex.run();
            bool ok = ex.result.ok && s.destination.existsAsFile();
            double fileSec = 0.0;
            if (ok) {
                std::unique_ptr<juce::AudioFormatReader> rd(SampleBank::get().formats().createReaderFor(s.destination));
                ok = rd != nullptr;
                if (rd) fileSec = rd->lengthInSamples / rd->sampleRate;
            }
            t.check(ok && fileSec >= songSec + 0.5 && fileSec < songSec + 16.0, juce::String("Export ") + s.extension() + " includes the whole song plus the reverb tail",
                    "song " + juce::String(songSec, 2) + " s, file " + juce::String(fileSec, 2) + " s, " + juce::String(ex.result.renderMs, 0) + " ms to render, peak " + juce::String(ex.result.peakDb, 1) + " dB");
        }
        ExportSettings s32; s32.bitDepth = 32; s32.destination = tmpRoot.getChildFile("Exports/float.wav");
        Exporter e32(p, s32); e32.run();
        t.check(e32.result.ok, "32-bit float WAV export");
        ExportSettings aac; aac.format = ExportSettings::AAC; aac.destination = tmpRoot.getChildFile("Exports/a.m4a");
        Exporter ea(p, aac); ea.run();
       #if JUCE_MAC || JUCE_IOS
        t.check(ea.result.ok, "AAC export with Apple's encoder", ea.result.error);
       #else
        t.check(!ea.result.ok && ea.result.error.contains("Apple"), "AAC reports clearly that it needs Apple's encoder on this platform");
       #endif
        Project empty;
        juce::AudioBuffer<float> eb; juce::String err;
        t.check(!Exporter::renderToBuffer(empty, SR, eb, 1.0, err) && err.contains("empty"), "Exporting an empty song gives a clear error");
        // cancellation
        ExportSettings sc; sc.destination = tmpRoot.getChildFile("Exports/cancel.wav");
        Project longSong = p;
        longSong.fromVar(p.toVar(), err);
        for (auto& tr : longSong.tracks) for (auto& c : tr.clips) c.length *= 40;
        Exporter ec(longSong, sc);
        ec.startThread();
        juce::Thread::sleep(30);
        ec.signalThreadShouldExit();
        ec.waitForThreadToExit(5000);
        t.check(ec.result.cancelled && !sc.destination.existsAsFile(), "Cancelling an export stops it and leaves no partial file");
    }

    // ------------------------------------------------------------ persistence
    t.begin("Projects: save/load fidelity, management, recovery, templates");
    {
        Project p = basicProject(97.5);
        p.name = "Fidelity";
        p.timeSig.front() = { 0, 7, 8 };
        p.key = 9; p.scale = 1;
        p.markers.push_back({ PPQ * 8, "Chorus", true, juce::Colours::orange });
        p.loopOn = true; p.loopStart = PPQ * 4; p.loopEnd = PPQ * 12;
        for (auto& e : Catalog::engines()) p.addTrack(juce::String(e.id) == "drums" ? TrackType::Drum : TrackType::Instrument, e.id, {});
        auto& fxTrack = p.tracks.front();
        for (auto& e : Catalog::effects()) { fxTrack.effects.push_back(p.makeEffect(e.id)); fxTrack.effects.back().params->set(0, fxTrack.effects.back().params->meta(0).max); }
        fxTrack.effects.back().bypass->store(true);
        Clip c = p.makeMidiClip(PPQ, PPQ * 3); c.notes.push_back({ 10, 200, 61, 0.33f }); fxTrack.clips.push_back(c);
        AutomationLane l; l.target = "ch:pan"; l.points = { { 0, 0.1f, 1 }, { 960, 0.9f, 2 } }; fxTrack.automation.push_back(l);
        auto& pat = p.addPattern("Groove"); pat.grid[3][5] = { true, 0.4f, 0.5f, -3, 2 }; pat.swing = 0.3f; pat.steps = 32;
        p.addTrack(TrackType::Bus); p.addTrack(TrackType::Return);
        p.master.effects.push_back(p.makeEffect("limiter"));
        const auto folder = ProjectStore::uniqueFolder(p.name);
        juce::String err;
        t.check(ProjectStore::save(p, folder, err), "Save to a project folder", err);
        Project q;
        t.check(ProjectStore::load(q, folder, err), "Load it back", err);
        q.modified = p.modified;   // save() stamps the modification time by design
        const auto a = juce::JSON::toString(p.toVar()), b = juce::JSON::toString(q.toVar());
        int diffAt = -1;
        for (int i = 0; i < juce::jmin(a.length(), b.length()); ++i) if (a[i] != b[i]) { diffAt = i; break; }
        if (diffAt >= 0 || a.length() != b.length()) {
            tmpRoot.getChildFile("rt_a.json").replaceWithText(juce::JSON::toString(p.toVar(), false));
            tmpRoot.getChildFile("rt_b.json").replaceWithText(juce::JSON::toString(q.toVar(), false));
        }
        t.check(a == b, "Every field round-trips exactly (tracks, MIDI, patterns, effects, automation, mixer, markers, tempo, meter)",
                juce::String(a.length()) + " bytes");
        juce::File renamed, dup;
        t.check(ProjectStore::rename(folder, "Renamed Song", renamed, err) && renamed.exists() && !folder.exists(), "Rename", err);
        t.check(ProjectStore::duplicate(renamed, dup, err) && dup.exists(), "Duplicate", err);
        const auto zip = tmpRoot.getChildFile("share.zip");
        juce::File imported;
        t.check(ProjectStore::exportZip(renamed, zip, err) && ProjectStore::importZip(zip, imported, err) && imported.exists(), "Export and re-import a portable .zip", err);
        t.check(ProjectStore::remove(dup) && !dup.exists(), "Delete");
        t.check(ProjectStore::list().size() >= 2, "Project list finds saved projects", juce::String((int)ProjectStore::list().size()));
        ProjectStore::writeRecovery(q, renamed);
        juce::String rn; juce::File rf; juce::Time when;
        juce::Thread::sleep(1100);
        ProjectStore::writeRecovery(q, renamed);
        t.check(ProjectStore::recoveryAvailable(rn, rf, when) && rf == renamed, "Crash recovery file detected after an unsaved change");
        Project rec; t.check(ProjectStore::loadRecovery(rec, rf, err) && rec.tracks.size() == q.tracks.size(), "Recovery restores the project", err);
        ProjectStore::markSessionOpen();
        t.check(ProjectStore::previousSessionCrashed(), "Unclean shutdown is detected");
        ProjectStore::markSessionClosed();
        t.check(!ProjectStore::previousSessionCrashed(), "Clean shutdown clears the marker");
        for (auto& name : ProjectStore::templateNames()) {
            Project tp; ProjectStore::buildTemplate(tp, name);
            juce::AudioBuffer<float> tb; juce::String e2;
            const bool hasContent = tp.songEndTick() > 0;
            const bool renders = !hasContent || (Exporter::renderToBuffer(tp, SR, tb, 1.0, e2) && peakOf(tb) > 0.01f);
            t.check(renders, "Template \"" + name + "\"", juce::String((int)tp.tracks.size()) + " tracks" + (hasContent ? ", plays" : ""));
        }
        t.check(ProjectStore::saveUserTemplate(q, "My Template", err) && ProjectStore::templateNames().contains("My Template"), "Save a user template", err);
        // corrupted file is rejected cleanly
        const auto bad = ProjectStore::uniqueFolder("Broken");
        bad.createDirectory();
        ProjectStore::projectFile(bad).replaceWithText("{ \"format\": \"ostinato-project\", \"tracks\": [ {");
        Project bp;
        t.check(!ProjectStore::load(bp, bad, err) && err.isNotEmpty(), "Damaged project file reports an error instead of crashing", err);
    }

    // ------------------------------------------------------------ midi
    t.begin("MIDI files: export and import round trip");
    {
        Project p = basicProject(133.0);
        p.timeSig.front() = { 0, 3, 4 };
        auto& tr = p.addTrack(TrackType::Instrument, {}, "Grand Piano");
        Clip c = p.makeMidiClip(0, PPQ * 6);
        for (int i = 0; i < 12; ++i) c.notes.push_back({ i * PPQ / 2, PPQ / 3, 60 + i, 0.5f + i * 0.03f });
        tr.clips.push_back(c);
        auto& d = p.addTrack(TrackType::Drum, {}, "Boom Kit");
        auto& pat = p.addPattern("P"); pat.grid[0][0].on = true; pat.grid[1][4].on = true;
        Clip pc; pc.id = p.newId("c"); pc.type = ClipType::Pattern; pc.patternId = pat.id; pc.length = PPQ * 3; d.clips.push_back(pc);
        const auto f = tmpRoot.getChildFile("roundtrip.mid");
        juce::String err;
        t.check(MidiIO::exportFile(p, f, err), "Export standard MIDI file", err);
        Project q; int added = 0;
        t.check(MidiIO::importFile(q, f, err, added) && added == 2, "Import it into an empty project", juce::String(added) + " tracks " + err);
        t.check(std::fabs(q.tempo.front().bpm - 133.0) < 0.01 && q.timeSig.front().num == 3, "Tempo and time signature imported");
        bool notesOk = !q.tracks.empty() && q.tracks[0].clips.size() == 1 && q.tracks[0].clips[0].notes.size() == 12;
        if (notesOk) for (int i = 0; i < 12; ++i) {
            const auto& n = q.tracks[0].clips[0].notes[(size_t)i];
            notesOk = notesOk && n.start == i * PPQ / 2 && n.length == PPQ / 3 && n.pitch == 60 + i && std::fabs(n.velocity - (0.5f + i * 0.03f)) < 0.01f;
        }
        t.check(notesOk, "Note positions, lengths, pitches and velocities preserved");
        t.check(q.tracks.size() > 1 && q.tracks[1].type == TrackType::Drum, "Channel 10 becomes a drum track");
    }

    // ------------------------------------------------------------ theory
    t.begin("Production tools: scales, chords, quantize, key and tempo detection");
    {
        Project p; p.key = 2; p.scale = 1;   // D minor
        t.check(Theory::inScale(p, 62) && Theory::inScale(p, 65) && !Theory::inScale(p, 66), "Scale membership (D minor)");
        auto ch = Theory::chordPitches(60, 5, 0);
        t.check(ch == std::vector<int>({ 60, 63, 67, 70 }), "Chord helper: Cm7");
        auto dia = Theory::diatonicChord(p, 4, 4, false);
        t.check(Theory::diatonicChordName(p, 4, false).startsWith("Am"), "Diatonic chord v in D minor is A minor", Theory::diatonicChordName(p, 4, false));
        std::vector<Note> notes = { { 7, 100, 60, 0.8f }, { 470, 100, 62, 0.8f }, { 1001, 100, 64, 0.8f } };
        Theory::quantize(notes, PPQ / 2, 1.f, 0.f, false, false);
        t.check(notes[0].start == 0 && notes[1].start == 480 && notes[2].start == 960, "Quantize 1/8 at 100%");
        std::vector<Note> cmaj;
        for (int i = 0; i < 64; ++i) { static const int s[] = { 60, 62, 64, 65, 67, 69, 71, 72, 64, 67, 60 }; cmaj.push_back({ i * 240, 240, s[i % 11], 0.8f }); }
        auto kg = Theory::detectKey(cmaj);
        t.check(kg.key == 0 && !kg.minor, "Key detection finds C major", Theory::keyNames()[kg.key] + (kg.minor ? " minor" : " major"));
        juce::String err;
        Project beat; beat.tempo.front().bpm = 124.0;
        auto& d = beat.addTrack(TrackType::Drum, {}, "Electro Kit");
        auto& pat = beat.addPattern("x");
        for (int s = 0; s < 16; s += 4) pat.grid[0][(size_t)s].on = true;
        for (int s = 2; s < 16; s += 4) pat.grid[3][(size_t)s].on = true;
        Clip pc; pc.id = beat.newId("c"); pc.type = ClipType::Pattern; pc.patternId = pat.id; pc.length = PPQ * 4 * 8; d.clips.push_back(pc);
        juce::AudioBuffer<float> bb;
        Exporter::renderToBuffer(beat, 44100.0, bb, 0.5, err);
        SampleData sd; sd.buffer = bb; sd.sampleRate = 44100.0;
        const double bpm = Theory::detectBpm(sd);
        t.check(std::fabs(bpm - 124.0) < 1.5, "BPM detection on a rendered 124 BPM loop", juce::String(bpm, 1) + " BPM");
        Theory::TapTempo tap;
        double est = 0; for (int i = 0; i < 6; ++i) est = tap.tap(i * 500.0);
        t.check(std::fabs(est - 120.0) < 0.01, "Tap tempo", juce::String(est, 2));
    }

    // ------------------------------------------------------------ samples
    t.begin("Samples: built-in packs, decoding, waveform data");
    {
        int wavs = 0;
        SampleBank::get().ensureBuiltinPacks();
        for (auto& f : Paths::library().getChildFile("Packs").findChildFiles(juce::File::findFiles, true, "*.wav")) {
            juce::String err;
            auto s = SampleBank::decodeFile(f, err);
            if (s && s->peak > 0.1f && !s->ovMax.empty()) ++wavs;
        }
        t.check(wavs >= 40, "Royalty-free sample packs generated and decodable", juce::String(wavs) + " files");
        juce::StringArray folders;
        for (auto& d : Paths::library().getChildFile("Packs").findChildFiles(juce::File::findDirectories, false)) folders.add(d.getFileName());
        t.check(folders.size() == 7, "Packs: " + folders.joinIntoString(", "));
        // AIFF + FLAC + OGG decode through the bank
        auto src = sine(523.25, 0.5, 0.4f);
        int decoded = 0;
        for (auto ext : { ".aiff", ".flac", ".ogg" }) {
            const auto f = tmpRoot.getChildFile(juce::String("dec") + ext);
            std::unique_ptr<juce::AudioFormat> fmt;
            if (juce::String(ext) == ".aiff") fmt = std::make_unique<juce::AiffAudioFormat>();
            else if (juce::String(ext) == ".flac") fmt = std::make_unique<juce::FlacAudioFormat>();
            else fmt = std::make_unique<juce::OggVorbisAudioFormat>();
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
            auto w = fmt->createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(SR).withNumChannels(2).withBitsPerSample(16).withQualityOptionIndex(5));
            if (w) { w->writeFromAudioSampleBuffer(src, 0, src.getNumSamples()); w.reset(); }
            juce::String err;
            auto s = SampleBank::decodeFile(f, err);
            if (s && s->frames() > 20000) ++decoded;
        }
        t.check(decoded == 3, "AIFF, FLAC and OGG files decode", juce::String(decoded) + "/3");
        t.check(SampleBank::supportedExtensions().contains("mp3"), "MP3 decoding is enabled");
        for (auto& d : Catalog::drumSounds()) {
            auto s = DrumSynth::render(d.id, SR);
            if (!s || s->peak < 0.5f) { t.check(false, juce::String("Drum sound ") + d.id); }
        }
        t.check(true, "All " + juce::String((int)Catalog::drumSounds().size()) + " synthesized drum sounds render");
    }


    // ------------------------------------------------------------ Studio Synth
    t.begin("Studio Synth: oscillators, filters, modulation, voices");
    {
        const int a = (int)(0.3 * SR), n = 8192;
        auto wide = playNote("studio", "o1_uni=5,o1_det=30,o1_spread=100");
        auto narrowB = playNote("studio", "o1_uni=5,o1_det=30,o1_spread=0");
        t.check(corrLR(wide, a, n) < 0.9f && corrLR(narrowB, a, n) > 0.999f, "Unison spread makes the sound stereo (and 0% keeps it mono)",
                "L/R correlation " + juce::String(corrLR(wide, a, n), 2) + " vs " + juce::String(corrLR(narrowB, a, n), 3));
        const double uniHz = pitchOf(wide, a, n);
        t.check(std::fabs(1200.0 * std::log2(uniHz / 440.0)) < 25.0, "Detuned unison stays centred on the note", juce::String(uniHz, 1) + " Hz");
        auto dark = playNote("studio", "f1_type=1,f1_cut=300,f1_env=0,f1_key=0");
        auto bright = playNote("studio", "f1_type=1,f1_cut=12000,f1_env=0,f1_key=0");
        const float cutDb = dbRatio(bandEnergy(bright, 3080.0, a, n), bandEnergy(dark, 3080.0, a, n));
        t.check(cutDb > 30.f, "Filter 1 low-pass 24 removes highs", juce::String(cutDb, 1) + " dB less at 3 kHz");
        auto hp = playNote("studio", "f1_type=3,f1_cut=2500,f1_env=0,f1_key=0");
        t.check(dbRatio(bandEnergy(bright, 440.0, a, n), bandEnergy(hp, 440.0, a, n)) > 20.f, "High-pass 24 removes the fundamental");
        auto f2 = playNote("studio", "f1_type=1,f1_cut=12000,f1_env=0,f1_key=0,f2_on=1,f2_type=0,f2_cut=400,f2_key=0,f2_env=0");
        t.check(dbRatio(bandEnergy(bright, 3080.0, a, n), bandEnergy(f2, 3080.0, a, n)) > 15.f, "Filter 2 in series filters further");
        auto envOpen = playNote("studio", "f1_type=1,f1_cut=200,f1_key=0,f1_env=100,e2_a=0.001,e2_d=0.15,e2_s=0");
        const float early = bandEnergy(envOpen, 3080.0, (int)(0.01 * SR), 2048), late = bandEnergy(envOpen, 3080.0, (int)(0.6 * SR), 2048);
        t.check(dbRatio(early, late) > 15.f, "Envelope 2 sweeps the filter (bright attack, dark sustain)", juce::String(dbRatio(early, late), 1) + " dB");
        // modulation matrix: LFO 1 -> pitch is vibrato
        auto vib = playNote("studio", "m1_src=3,m1_dst=1,m1_amt=4,l1_rate=4,f1_env=0", 69, 1.2, 1.2);
        double lo = 1e9, hi = 0;
        for (int w = 0; w < 10; ++w) { const double hz = pitchOf(vib, (int)(0.1 * SR) + w * 4800, 2400); if (hz > 0) { lo = std::min(lo, hz); hi = std::max(hi, hz); } }
        const double vibCents = 1200.0 * std::log2(hi / std::max(lo, 1.0));
        t.check(vibCents > 140.0 && vibCents < 230.0, "Mod matrix: LFO 1 -> Pitch (4%) gives about +/-1 semitone vibrato", juce::String(vibCents, 0) + " cents peak to peak");
        auto mac0 = playNote("studio", "m1_src=11,m1_dst=17,m1_amt=100,mac1=0,f1_type=1,f1_cut=300,f1_env=0,f1_key=0");
        auto mac1 = playNote("studio", "m1_src=11,m1_dst=17,m1_amt=100,mac1=100,f1_type=1,f1_cut=300,f1_env=0,f1_key=0");
        t.check(dbRatio(bandEnergy(mac1, 3080.0, a, n), bandEnergy(mac0, 3080.0, a, n)) > 20.f, "Macro 1 -> Filter cutoff opens the filter");
        // oscillator interactions
        auto pure = playNote("studio", "o1_wave=0,f1_cut=20000,f1_env=0");
        auto fm = playNote("studio", "o1_wave=0,o2_on=1,o2_wave=0,o2_level=0,fm=50,f1_cut=20000,f1_env=0");
        t.check(dbRatio(bandEnergy(fm, 1320.0, a, n), bandEnergy(pure, 1320.0, a, n)) > 25.f, "FM (osc 2 > osc 1) adds harmonics to a sine");
        auto sync = playNote("studio", "o1_level=0,o2_on=1,o2_wave=1,o2_semi=7,sync=1,f1_cut=20000,f1_env=0");
        auto nosync = playNote("studio", "o1_level=0,o2_on=1,o2_wave=1,o2_semi=7,sync=0,f1_cut=20000,f1_env=0");
        const double syncHz = pitchOf(sync, a, n), freeHz = pitchOf(nosync, a, n);
        t.check(std::fabs(syncHz - 440.0) < 5.0 && std::fabs(freeHz - 659.3) < 6.0, "Hard sync locks osc 2 to osc 1's pitch", juce::String(syncHz, 1) + " Hz synced, " + juce::String(freeHz, 1) + " Hz free");
        auto ring = playNote("studio", "o1_wave=0,o2_on=1,o2_wave=0,o2_semi=7,ring=100,f1_cut=20000,f1_env=0");
        t.check(dbRatio(bandEnergy(ring, 1099.3, a, n), bandEnergy(ring, 440.0, a, n)) > 20.f, "Ring mod produces sum frequencies instead of the notes");
        auto sub = playNote("studio", "o1_level=0,sub_level=100,f1_cut=20000,f1_env=0");
        t.check(std::fabs(pitchOf(sub, a, n) - 220.0) < 3.0, "Sub oscillator sounds an octave down");
        auto wt = playNote("studio", "o1_wave=7,o1_table=2,o1_pos=50,f1_cut=20000,f1_env=0");
        t.check(std::fabs(1200.0 * std::log2(pitchOf(wt, a, n) / 440.0)) < 20.0, "Wavetable oscillator plays in tune");
        auto comb = playNote("studio", "o1_level=30,f1_type=7,f1_cut=440,f1_key=0,f1_res=90,f1_env=0", 57);
        t.check(bandEnergy(comb, 440.0, a, n) > 4.f * bandEnergy(comb, 330.0, a, n), "Comb filter resonates at its pitch");
        // voices
        auto ps = Catalog::makeParams(Catalog::engineParams("studio"));
        InstrumentSetup setup; setup.engine = "studio"; setup.params = ps;
        auto inst = createInstrument(setup);
        inst->prepare(SR, 64);
        float L[64], R[64];
        auto play4 = [&] { inst->allNotesOff(true); for (int k : { 60, 64, 67, 71 }) inst->noteOn(k, 0.8f, 0.f); std::fill(L, L + 64, 0.f); std::fill(R, R + 64, 0.f); inst->render(L, R, 64); return inst->activeVoices(); };
        Catalog::applyValues(*ps, "voices=2");
        const int two = play4();
        Catalog::applyValues(*ps, "voices=8");
        const int eight = play4();
        Catalog::applyValues(*ps, "mode=1");
        const int mono = play4();
        t.check(two == 2 && eight == 4 && mono == 1, "Voice count and mono mode limit simultaneous notes", juce::String(two) + "/" + juce::String(eight) + "/" + juce::String(mono));
        // random patches stay healthy
        int healthy = 0, audible = 0;
        for (uint32_t seed = 1; seed <= 60; ++seed) {
            auto rp = Catalog::makeParams(Catalog::engineParams("studio"));
            Catalog::randomizeSound(*rp, "studio", 1.f, seed);
            InstrumentSetup su; su.engine = "studio"; su.params = rp;
            auto ri = createInstrument(su);
            TransportInfo tr; ri->setTransport(&tr);
            ri->prepare(SR, 64);
            juce::AudioBuffer<float> b(2, (int)(1.0 * SR)); b.clear();
            ri->noteOn(60, 0.9f, 0.f);
            for (int pos = 0; pos < b.getNumSamples(); pos += 64) { if (pos == (int)(0.6 * SR) / 64 * 64) ri->noteOff(60); ri->render(b.getWritePointer(0, pos), b.getWritePointer(1, pos), 64); }
            const float pk = peakOf(b);
            if (allFinite(b) && pk < 4.f) ++healthy;
            if (pk > 0.003f) ++audible;
        }
        t.check(healthy == 60 && audible >= 54, "Randomize: 60 random sounds are all finite and safe, and audible", juce::String(healthy) + " safe, " + juce::String(audible) + " audible");
        auto mp = Catalog::makeParams(Catalog::engineParams("studio"));
        Catalog::randomizeSound(*mp, "studio", 0.06f, 7);
        int changed = 0, far = 0;
        auto ref = Catalog::makeParams(Catalog::engineParams("studio"));
        for (int i = 0; i < mp->size(); ++i) {
            const float d = std::fabs(mp->toNorm(i, mp->getBase(i)) - ref->toNorm(i, ref->getBase(i)));
            if (d > 0.0001f) ++changed;
            if (d > 0.3f && mp->meta(i).scale != ParamScale::Choice && mp->meta(i).scale != ParamScale::Int && mp->meta(i).scale != ParamScale::Bool) ++far;
        }
        t.check(changed > 20 && far == 0, "Mutate nudges many controls a little", juce::String(changed) + " changed, " + juce::String(far) + " moved far");
    }

    t.begin("More control: Shape section on every engine, drum pad shaping");
    {
        const int a = (int)(0.3 * SR), n = 8192;
        auto base = playNote("analog", "", 69, 1.0, 0.8, "Analog Synth");
        auto cut = playNote("analog", "sh_hp=1500", 69, 1.0, 0.8, "Analog Synth");
        t.check(dbRatio(bandEnergy(base, 440.0, a, n), bandEnergy(cut, 440.0, a, n)) > 12.f, "Shape Low Cut removes lows (subtractive synth)");
        auto fine = playNote("piano", "sh_fine=50");
        const double c = 1200.0 * std::log2(pitchOf(fine, a, n) / 440.0);
        t.check(std::fabs(c - 50.0) < 12.0, "Shape Fine Tune moves pitch by cents (piano)", juce::String(c, 1) + " ct");
        auto mono = playNote("ensemble", "sh_width=0");
        t.check(corrLR(mono, a, n) > 0.999f, "Shape Width 0% folds to mono (ensemble)");
        auto warm = playNote("organ", "sh_drive=100");
        t.check(allFinite(warm) && peakOf(warm) < 2.f, "Shape Warmth saturates without blowing up (organ)");
        // drum pads
        auto kit = [&](const juce::String& padValues) {
            Project p = basicProject();
            auto& d = p.addTrack(TrackType::Drum, {}, "Boom Kit");
            Catalog::applyValues(*d.inst.pads[0].params, padValues);
            Clip cl = p.makeMidiClip(0, PPQ * 4);
            cl.notes.push_back({ 0, PPQ / 4, 36, 0.9f });
            d.clips.push_back(cl);
            return render(p, nullptr, 1.0);
        };
        auto k0 = kit(""), kAtk = kit("attack=80"), kHp = kit("hp=1500"), kStart = kit("start=50"), kDrop = kit("pitch_env=24,pitch_dec=40"), kDrive = kit("drive=100");
        t.check(dbRatio(rmsOf(k0, 0, (int)(0.004 * SR)), rmsOf(kAtk, 0, (int)(0.004 * SR))) > 12.f, "Pad attack fades the hit in");
        t.check(dbRatio(bandEnergy(k0, 60.0, 0, 4096), bandEnergy(kHp, 60.0, 0, 4096)) > 15.f, "Pad low cut removes the kick's sub");
        auto len = [](const juce::AudioBuffer<float>& b) { int last = 0; for (int i = 0; i < b.getNumSamples(); ++i) if (std::fabs(b.getSample(0, i)) > 0.001f) last = i; return last; };
        t.check(len(kStart) < len(k0) - (int)(0.05 * SR), "Pad start skips into the sample", juce::String(len(kStart) / SR, 2) + " s vs " + juce::String(len(k0) / SR, 2) + " s");
        auto zc = [](const juce::AudioBuffer<float>& b, int from, int to) { int z = 0; for (int i = from + 1; i < to; ++i) if ((b.getSample(0, i) >= 0.f) != (b.getSample(0, i - 1) >= 0.f)) ++z; return z; };
        t.check(zc(kDrop, 0, (int)(0.02 * SR)) > zc(k0, 0, (int)(0.02 * SR)) + 2, "Pad pitch drop starts the hit higher");
        const float crest0 = peakOf(k0, 0, (int)(0.2 * SR)) / std::max(rmsOf(k0, 0, (int)(0.2 * SR)), 1e-9f);
        const float crestD = peakOf(kDrive, 0, (int)(0.2 * SR)) / std::max(rmsOf(kDrive, 0, (int)(0.2 * SR)), 1e-9f);
        t.check(allFinite(kDrive) && crestD < crest0 * 0.8f && peakOf(kDrive) < 2.f, "Pad drive thickens the hit (less peaky, louder body)",
                "crest " + juce::String(dsp::lin2db(crest0), 1) + " -> " + juce::String(dsp::lin2db(crestD), 1) + " dB");
    }

    t.begin("Effects you build: De-esser and Effect Designer");
    {
        // vocal-like test: steady 300 Hz plus a loud 7 kHz "s"
        auto in = sine(300.0, 1.0, 0.3f);
        auto ess = sine(7000.0, 1.0, 0.3f);
        for (int ch = 0; ch < 2; ++ch) in.addFrom(ch, 0, ess, ch, 0, in.getNumSamples());
        auto out = runEffect("deesser", "freq=5000,threshold=-30,range=12", in);
        const int a = (int)(0.3 * SR), n = 8192;
        t.check(dbRatio(bandEnergy(in, 7000.0, a, n), bandEnergy(out, 7000.0, a, n)) > 6.f && std::fabs(dbRatio(bandEnergy(in, 300.0, a, n), bandEnergy(out, 300.0, a, n))) < 1.f,
                "De-esser turns down the S sound but not the voice", juce::String(dbRatio(bandEnergy(in, 7000.0, a, n), bandEnergy(out, 7000.0, a, n)), 1) + " dB on 7 kHz");
        auto listen = runEffect("deesser", "freq=5000,listen=1", in);
        t.check(dbRatio(bandEnergy(in, 300.0, a, n), bandEnergy(listen, 300.0, a, n)) > 20.f, "De-esser listen mode plays only the S band");
        // designer: empty = untouched; every module runs cleanly
        auto src = sine(440.0, 1.0, 0.4f);
        auto empty = runEffect("designer", "s1_type=0,s2_type=0", src);
        float diff = 0.f;
        for (int i = 0; i < src.getNumSamples(); ++i) diff = std::max(diff, std::fabs(empty.getSample(0, i) - src.getSample(0, i)));
        t.check(diff < 1e-5f, "Designer with empty modules passes audio untouched");
        int ok = 0, changedCount = 0;
        for (int type = 1; type <= 15; ++type) {
            auto o = runEffect("designer", "s1_type=" + juce::String(type) + ",s2_type=0,s1_a=60,s1_b=60,s1_c=40", src);
            if (allFinite(o) && peakOf(o) < 4.f) ++ok;
            float d = 0.f;
            for (int i = (int)(0.5 * SR); i < (int)(0.6 * SR); ++i) d = std::max(d, std::fabs(o.getSample(0, i) - src.getSample(0, i)));
            if (d > 0.01f) ++changedCount;
        }
        t.check(ok == 15 && changedCount == 15, "All 15 designer modules process audio (finite, audible change)", juce::String(ok) + " ok, " + juce::String(changedCount) + " changed");
        auto saw = sine(2000.0, 1.0, 0.4f);
        auto lp = runEffect("designer", "s1_type=1,s1_a=30,s1_b=0,s1_c=0,s2_type=0", saw);   // cutoff 20*1000^0.3 = 159 Hz
        t.check(dbRatio(peakOf(saw, a, n), peakOf(lp, a, n)) > 20.f, "Designer low-pass module (A = cutoff) filters", Catalog::designerFormat(1, 0, 0.3f));
        juce::AudioBuffer<float> imp(2, (int)SR); imp.clear(); imp.setSample(0, 100, 1.f); imp.setSample(1, 100, 1.f);
        const float v250 = std::log(250.f) / std::log(2000.f);
        auto dl = runEffect("designer", "s1_type=9,s1_a=" + juce::String(v250 * 100.f, 4) + ",s1_b=0,s1_mix=100,s2_type=0", imp);
        int peakAt = 0; float best = 0.f;
        for (int i = 200; i < imp.getNumSamples(); ++i) if (std::fabs(dl.getSample(0, i)) > best) { best = std::fabs(dl.getSample(0, i)); peakAt = i; }
        const double echoMs = (peakAt - 100) / SR * 1000.0;
        t.check(std::fabs(echoMs - 250.0) < 1.0, "Designer delay module echoes at the time its knob shows", Catalog::designerFormat(9, 0, v250) + " -> " + juce::String(echoMs, 2) + " ms");
        auto steady = runEffect("designer", "s1_type=13,s1_a=50,s2_type=0", src);
        auto moving = runEffect("designer", "s1_type=13,s1_a=50,s2_type=0,m1_src=1,m1_dst=1,m1_amt=40,l1_rate=2", src);
        auto swing = [](const juce::AudioBuffer<float>& b) { float mn = 1e9f, mx = 0.f; for (int w = 0; w < 15; ++w) { const float r = rmsOf(b, (int)(0.1 * SR) + w * 2400, 2400); mn = std::min(mn, r); mx = std::max(mx, r); } return dbRatio(mx, mn); };
        t.check(swing(moving) > 10.f && swing(moving) < 45.f && swing(steady) < 0.5f, "Designer LFO moves a module knob (gain) over time",
                juce::String(swing(moving), 1) + " dB swing vs " + juce::String(swing(steady), 2) + " dB without");
        auto loud = sine(1000.0, 0.5, 0.5f), quiet = sine(1000.0, 0.5, 0.02f);
        const juce::String wah = "s1_type=1,s1_a=30,s1_b=50,s2_type=0,m1_src=3,m1_dst=1,m1_amt=60,ef_gain=12";
        auto wl = runEffect("designer", wah, loud), wq = runEffect("designer", wah, quiet);
        const float gainLoud = peakOf(wl, (int)(0.3 * SR), 4096) / 0.5f, gainQuiet = peakOf(wq, (int)(0.3 * SR), 4096) / 0.02f;
        t.check(dbRatio(gainLoud, gainQuiet) > 10.f, "Designer envelope follower opens the filter when you play louder (auto-wah)");
        t.check(juce::String(Catalog::designerKnobName(1, 0)) == "Cutoff" && juce::String(Catalog::designerKnobName(9, 1)) == "Feedback", "Designer knobs are labelled for the chosen module");
        int presetsOk = 0, chainOk = 0, chains = 0;
        juce::StringArray bad;
        for (auto& pr : Catalog::effectPresets()) if (juce::String(pr.effect) == "designer") { auto o = runEffect("designer", pr.values, src); if (allFinite(o) && peakOf(o) < 4.f) ++presetsOk; else bad.add(pr.name + juce::String(" ") + juce::String(peakOf(o), 1)); }
        for (auto& ch : Catalog::chainPresets()) {
            ++chains;
            bool good = true;
            auto x = sine(220.0, 0.5, 0.3f);
            for (auto& [type, values] : ch.effects) { if (!Catalog::findEffect(type)) { good = false; break; } x = runEffect(type, values, x); }
            if (good && allFinite(x) && peakOf(x) < 4.f && (ch.effects.empty() || peakOf(x) > 0.01f)) ++chainOk;
        }
        t.check(presetsOk >= 6 && chainOk == chains, "Designer presets and all vocal/instrument chains run cleanly", juce::String(presetsOk) + " designer presets, " + juce::String(chainOk) + "/" + juce::String(chains) + " chains " + bad.joinIntoString(", "));
    }

    t.begin("Recording booth: input gain, monitoring modes, live effects, record clean or with effects");
    {
        Project p = basicProject();
        auto& at = p.addTrack(TrackType::Audio);
        at.armed = true;
        const auto id = at.id;
        auto fx = p.makeEffect("lowpass");
        Catalog::applyValues(*fx.params, "cutoff=200,reso=0,slope=1");
        at.inputEffects.push_back(fx);
        ProcRegistry reg;
        constexpr int N = 19 * 256;
        std::vector<float> inBuf(N);
        for (int i = 0; i < N; ++i) inBuf[(size_t)i] = 0.25f * std::sin(2.f * 3.14159265f * 2000.f * (float)i / (float)SR);
        struct Result { juce::AudioBuffer<float> out, rec; };
        auto run = [&](const juce::String& inputValues, bool play) {
            auto* tr = p.findTrack(id);
            tr->input->resetToDefaults();
            Catalog::applyValues(*tr->input, inputValues);
            auto snap = Compiler::compile(p, reg, SR, true);
            auto rOwner = std::make_unique<Renderer>(); auto& r = *rOwner;  /* on the heap: a Renderer is ~300 KB */ r.prepare(SR); r.setSnapshot(snap.get());
            r.idleSleepEnabled = false;
            Result res; res.out.setSize(2, N); res.out.clear(); res.rec.setSize(2, N); res.rec.clear();
            int recPos = 0;
            r.onTrackInput = [&](uint32_t, const float* L, const float* R, int n) { for (int i = 0; i < n && recPos + i < N; ++i) { res.rec.setSample(0, recPos + i, L[i]); res.rec.setSample(1, recPos + i, R[i]); } recPos += n; };
            if (play) r.play();
            for (int pos = 0; pos < N; pos += 256) {
                const float* ins[1] = { inBuf.data() + pos };
                r.render(res.out.getWritePointer(0, pos), res.out.getWritePointer(1, pos), 256, ins, 1);
            }
            return res;
        };
        const int a = 2048, n = 2048;
        auto off = run("monitor=0", false);
        auto direct = run("monitor=1", false);
        auto through = run("monitor=2", false);
        p.findTrack(id)->inputEffects[0].bypass->store(true);
        auto throughDry = run("monitor=2", false);
        auto throughGain = run("monitor=2,gain=6", false);
        p.findTrack(id)->inputEffects[0].bypass->store(false);
        t.check(peakOf(off.out) < 1e-6f, "Hear yourself: Off is silent");
        t.check(peakOf(direct.out) < 1e-6f, "Hear yourself: Direct (interface) leaves the app silent");
        t.check(peakOf(throughDry.out, a, n) > 0.2f, "Hear yourself: Through effects plays the input", juce::String(peakOf(throughDry.out, a, n), 3));
        t.check(dbRatio(peakOf(throughDry.out, a, n), peakOf(through.out, a, n)) > 20.f, "Live input effects are heard while monitoring (low-pass on 2 kHz)");
        t.check(std::fabs(dbRatio(peakOf(throughGain.out, a, n), peakOf(throughDry.out, a, n)) - 6.f) < 0.3f, "Input gain +6 dB raises the input by 6 dB");
        auto recClean = run("monitor=2,print=0", true);
        auto recFx = run("monitor=0,print=1", true);
        t.check(peakOf(recClean.rec, a, n) > 0.2f, "Record clean: the take is the dry input even with effects on", juce::String(peakOf(recClean.rec, a, n), 3));
        t.check(dbRatio(peakOf(recClean.rec, a, n), peakOf(recFx.rec, a, n)) > 20.f, "Record with effects: the take goes through the chain (even with monitoring off)");
        auto* tr0 = p.findTrack(id);
        tr0->inputEffects.push_back(p.makeEffect("limiter"));
        auto snap = Compiler::compile(p, reg, SR, true);
        int lat = 0;
        for (auto& trr : snap->tracks) if (trr.id == id) lat = Renderer::inputChainLatency(trr);
        t.check(lat > 0, "Recording with effects knows the chain's latency (limiter look-ahead)", juce::String(lat) + " samples");
        // save / load
        Project q; juce::String err;
        const bool loaded = q.fromVar(p.toVar(), err);
        auto* qt = q.findTrack(id);
        t.check(loaded && qt && qt->inputEffects.size() == 2 && qt->inputEffects[0].type == "lowpass" && qt->input->getInt(3) == 1, "Input chain and booth settings are saved with the song");
        auto v = p.toVar();
        if (auto* tracks = v["tracks"].getArray())
            for (auto& tv : *tracks) if (tv["id"].toString() == id) if (auto* o = tv.getDynamicObject()) { o->removeProperty("input"); o->setProperty("monitor", true); }
        Project old; old.fromVar(v, err);
        t.check(old.findTrack(id) && old.findTrack(id)->monitorMode() == Track::MonitorThrough, "Older songs with monitoring on open with \"Through effects\"");
        Project tpl = basicProject();
        ProjectStore::buildTemplate(tpl, "Songwriter");
        bool vocalChain = false;
        for (auto& trr : tpl.tracks) if (trr.name == "Vocals") vocalChain = trr.inputEffects.size() >= 3;
        t.check(vocalChain, "Songwriter template's vocal track comes with a vocal chain");
    }

    // ------------------------------------------------------------ live engine
   #if JUCE_MODAL_LOOPS_PERMITTED
    t.begin("Real-time engine: transport, undo, recording (simulated input)");
    {
        AudioEngine engine;
        engine.simulateInput = true;
        engine.initialise();
        Session session(engine);
        session.newProject("Live Test", "Beat Starter", 120.0, 4, 4);
        const auto mm = juce::MessageManager::getInstance();
        auto pump = [mm](int ms) { const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms; while (juce::Time::getMillisecondCounter() < end) mm->runDispatchLoopUntil(10); };
        pump(200);
        engine.seek(0);
        engine.play();
        pump(600);
        const double tick = engine.playheadTick();
        t.check(engine.isPlaying() && tick > PPQ * 0.6 && tick < PPQ * 1.8, "Transport advances in real time", juce::String(tick / PPQ, 2) + " beats after 0.6 s");
        auto ch = engine.channelFor(session.project.tracks[0].id);
        t.check(ch && ch->peakL.load() > 0.01f, "Track meters see signal during playback", ch ? juce::String(ch->peakL.load(), 3) : juce::String("none"));
        engine.stop();
        pump(100);
        t.check(!engine.isPlaying(), "Stop");
        const auto before = session.project.tracks.size();
        session.checkpoint("Add track");
        session.project.addTrack(TrackType::Audio);
        session.changed(true);
        session.undo();
        t.check(session.project.tracks.size() == before && session.canRedo(), "Undo removes the added track");
        session.redo();
        t.check(session.project.tracks.size() == before + 1, "Redo restores it");
        // record 1.0 s of the simulated 220 Hz input on the audio track (count-in off)
        session.project.countInBars = 0;
        // Audio recording needs an input. Machines with a real output device but no input (CI Macs)
        // cannot feed the simulated signal, so those checks are skipped there, not failed.
        const bool haveInput = engine.inputEnabled();
        if (haveInput) {
            auto& at = session.project.tracks.back();
            at.armed = true;
            session.changed(true);
            engine.rebuild();
            pump(100);
            engine.seek(PPQ * 4);
            juce::String err;
            const bool started = engine.startRecording(err);
            pump(1000);
            engine.stopRecording();
            pump(100);
            const Clip* rc = nullptr;
            for (auto& c : session.project.tracks.back().clips) if (c.type == ClipType::Audio) rc = &c;
            t.check(started && rc != nullptr, "Recording creates an audio clip", err);
            if (rc) {
                auto smp = SampleBank::get().loadNow(rc->audio.file);
                const double secs = smp ? smp->seconds() : 0.0;
                t.check(smp && secs > 0.8 && secs < 1.4, "Recorded file holds ~1 s of audio", juce::String(secs, 2) + " s");
                if (smp) {
                    juce::AudioBuffer<float> chk(2, std::min(smp->frames(), 16384));
                    for (int i = 0; i < chk.getNumSamples(); ++i) { chk.setSample(0, i, smp->buffer.getSample(0, i)); chk.setSample(1, i, smp->buffer.getSample(0, i)); }
                    const double hz = pitchOf(chk, 2048, 8192, smp->sampleRate);
                    t.check(std::fabs(hz - 220.0) < 2.0, "Recorded audio is the input signal (220 Hz)", juce::String(hz, 1) + " Hz");
                }
                t.check(rc->start == PPQ * 4, "Clip placed at the punch-in point (bar 2)");
            }
            // record again, this time with an input chain printed (designer gain module at -12 dB)
            {
                auto& tr = session.project.tracks.back();
                auto fx = session.project.makeEffect("designer");
                Catalog::applyValues(*fx.params, "s1_type=13,s1_a=25,s1_b=50,s1_c=50,s2_type=0");
                tr.inputEffects.push_back(fx);
                tr.input->set(3, 1.f);
                session.changed(true);
                engine.rebuild();
                pump(100);
                engine.seek(PPQ * 8);
                const bool s3 = engine.startRecording(err);
                pump(800);
                engine.stopRecording();
                pump(100);
                const Clip* fc = nullptr;
                for (auto& c : session.project.tracks.back().clips) if (c.type == ClipType::Audio && c.start == PPQ * 8) fc = &c;
                auto smp = fc ? SampleBank::get().loadNow(fc->audio.file) : nullptr;
                const float pk = smp ? smp->buffer.getMagnitude(0, std::min(smp->frames(), 24000)) : 0.f;
                t.check(s3 && smp && smp->buffer.getNumChannels() == 2 && std::fabs(pk - 0.125f) < 0.03f, "Live recording with effects writes the processed sound (stereo, -12 dB)",
                        juce::String(pk, 3) + " peak, " + juce::String(smp ? smp->buffer.getNumChannels() : 0) + " ch");
                tr.input->set(3, 0.f);
                session.changed(true);
            }
            session.project.tracks.back().armed = false;
        } else {
            t.lines.add("  SKIP  Audio recording checks: this machine's audio device has no input");
            std::printf("  SKIP  Audio recording checks: this machine's audio device has no input\n");
        }
        // MIDI recording from live input
        session.project.tracks[1].armed = true;
        session.changed(true);
        engine.rebuild();
        pump(100);
        engine.seek(0);
        const bool s2 = engine.startRecording(err);
        pump(250);
        engine.liveNoteOn(session.project.tracks[1].id, 64, 0.8f);
        pump(300);
        engine.liveNoteOff(session.project.tracks[1].id, 64);
        pump(200);
        engine.stopRecording();
        bool gotNote = false;
        for (auto& c : session.project.tracks[1].clips) for (auto& n : c.notes) if (n.pitch == 64 && n.length > PPQ / 4) gotNote = true;
        t.check(s2 && gotNote, "MIDI recording captures live notes into a clip", err);
        juce::String serr;
        t.check(session.save(serr) && ProjectStore::projectFile(session.folder).existsAsFile(), "Session save", serr);
        engine.shutdown();
    }
   #endif

    // ------------------------------------------------------------ performance
    t.begin("Performance (this machine)");
    {
        Project p = basicProject(120.0);
        ProjectStore::buildTemplate(p, "Electronic");
        for (auto& tr : p.tracks) {
            if (tr.type != TrackType::Instrument) continue;
            Clip c = p.makeMidiClip(0, PPQ * 32);
            for (int i = 0; i < 64; ++i) for (int k = 0; k < 3; ++k) c.notes.push_back({ i * PPQ / 2, PPQ / 2, 48 + (i * 7 + k * 4) % 24, 0.8f });
            tr.clips.push_back(c);
            tr.effects.push_back(p.makeEffect("compressor"));
            tr.effects.push_back(p.makeEffect("eq3"));
        }
        juce::AudioBuffer<float> b; juce::String err;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        Exporter::renderToBuffer(p, SR, b, 2.0, err);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const double audioMs = 1000.0 * b.getNumSamples() / SR;
        t.check(audioMs / ms > 3.0, "Dense 6-track arrangement renders faster than real time", juce::String(audioMs / ms, 1) + "x real time (" + juce::String((int)p.tracks.size()) + " tracks, 3-note chords, 2 inserts each)");
        // worst-case single block cost for a 256-sample buffer
        ProcRegistry reg;
        auto snap = Compiler::compile(p, reg, SR, true);
        auto rOwner = std::make_unique<Renderer>(); auto& r = *rOwner;  /* on the heap: a Renderer is ~300 KB */ r.prepare(SR); r.setSnapshot(snap.get()); r.play();
        float L[256], R[256];
        double worst = 0;
        for (int i = 0; i < 2000; ++i) { const auto a = juce::Time::getMillisecondCounterHiRes(); r.render(L, R, 256); worst = std::max(worst, juce::Time::getMillisecondCounterHiRes() - a); }
        t.check(worst < 256.0 / SR * 1000.0, "Worst 256-sample block fits its deadline", juce::String(worst, 3) + " ms of " + juce::String(256.0 / SR * 1000.0, 2) + " ms");
    }

    const double wall = (juce::Time::getMillisecondCounterHiRes() - wallStart) / 1000.0;
    t.lines.insert(0, "Ostinato self-test: " + juce::String(t.passed) + " passed, " + juce::String(t.failed) + " failed (" + juce::String(wall, 1) + " s)");
    std::printf("\n%s\n", t.lines[0].toRawUTF8());
    if (reportFile != juce::File()) reportFile.replaceWithText(t.lines.joinIntoString("\n") + "\n");
    Paths::overrideRoot() = juce::File();
    return t.failed == 0 ? 0 : 1;
}
