#include "Samples.h"
#include "Dsp.h"
#include "Core/Paths.h"

using namespace dsp;

void SampleData::computeOverview() {
    const int n = frames();
    const int bins = (n + overviewStep - 1) / overviewStep;
    ovMin.assign((size_t)bins, 0.f);
    ovMax.assign((size_t)bins, 0.f);
    peak = 0.f;
    const int ch = buffer.getNumChannels();
    for (int b = 0; b < bins; ++b) {
        float lo = 0.f, hi = 0.f;
        const int s0 = b * overviewStep, s1 = std::min(n, s0 + overviewStep);
        for (int c = 0; c < ch; ++c) {
            const float* d = buffer.getReadPointer(c);
            for (int i = s0; i < s1; ++i) { lo = std::min(lo, d[i]); hi = std::max(hi, d[i]); }
        }
        ovMin[(size_t)b] = lo; ovMax[(size_t)b] = hi;
        peak = std::max(peak, std::max(-lo, hi));
    }
}

// =================================================================== drum synthesis
namespace DrumSynth {
namespace {

struct Gen {
    double sr;
    std::vector<float> L, R;
    Rng rng { 0xC0FFEEu };
    Gen(double s, double seconds) : sr(s), L((size_t)(s * seconds), 0.f), R((size_t)(s * seconds), 0.f) {}
    int n() const { return (int)L.size(); }
    float t(int i) const { return (float)(i / sr); }
    void addMono(int i, float v) { L[(size_t)i] += v; R[(size_t)i] += v; }
};

// pitched body with exponential pitch drop
void body(Gen& g, float f0, float f1, float pitchTau, float decay, float gain, float sat = 0.f) {
    double ph = 0.0;
    for (int i = 0; i < g.n(); ++i) {
        const float t = g.t(i);
        const float f = f1 + (f0 - f1) * std::exp(-t / pitchTau);
        ph += f / g.sr;
        float v = std::sin((float)(ph * kTwoPi)) * std::exp(-t / decay) * gain;
        if (sat > 0.f) v = fastTanh(v * (1.f + sat * 4.f)) / (1.f + sat);
        g.addMono(i, v);
    }
}
// filtered noise burst
void noise(Gen& g, int type, float hz, float q, float decay, float gain, float attack = 0.f, float start = 0.f) {
    Biquad f; f.set(type, hz, q, 0.f, (float)g.sr);
    Biquad f2 = f;
    for (int i = 0; i < g.n(); ++i) {
        const float t = g.t(i) - start;
        if (t < 0.f) continue;
        const float env = (attack > 0.f ? std::min(1.f, t / attack) : 1.f) * std::exp(-t / decay);
        const float v = f2.tick(f.tick(g.rng.bi())) * env * gain;
        g.L[(size_t)i] += v * (1.f + 0.1f * g.rng.bi());
        g.R[(size_t)i] += v * (1.f + 0.1f * g.rng.bi());
    }
}
// metallic: 6 detuned squares (classic hat/cymbal recipe) through highpass
void metal(Gen& g, float baseMul, float hp, float decay, float gain, float bp = 0.f) {
    static const float fr[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };
    double ph[6] = {};
    Biquad h1, h2, b; h1.set(Biquad::HP, hp, 0.7f, 0, (float)g.sr); h2 = h1;
    if (bp > 0.f) b.set(Biquad::BP, bp, 0.9f, 0, (float)g.sr);
    for (int i = 0; i < g.n(); ++i) {
        float s = 0.f;
        for (int k = 0; k < 6; ++k) { ph[k] += fr[k] * baseMul / g.sr; s += (std::fmod(ph[k], 1.0) < 0.5 ? 1.f : -1.f); }
        s /= 6.f;
        if (bp > 0.f) s = b.tick(s) * 2.f;
        const float v = h2.tick(h1.tick(s)) * std::exp(-g.t(i) / decay) * gain;
        g.addMono(i, v);
    }
}
void tones(Gen& g, std::initializer_list<float> freqs, float decay, float gain) {
    int k = 0;
    for (float f : freqs) {
        const float d = decay * (1.f - 0.12f * (float)k);
        for (int i = 0; i < g.n(); ++i) g.addMono(i, std::sin(kTwoPi * f * g.t(i)) * std::exp(-g.t(i) / d) * gain / (1.f + 0.3f * k));
        ++k;
    }
}
void click(Gen& g, float gain, float hz = 3000.f) { noise(g, Biquad::BP, hz, 0.7f, 0.004f, gain); }

std::shared_ptr<SampleData> finish(Gen& g, const juce::String& name) {
    // fade tail, normalize to -1 dBFS
    const int fade = std::min(g.n(), (int)(g.sr * 0.01));
    for (int i = 0; i < fade; ++i) { const float k = (float)i / (float)fade; g.L[(size_t)(g.n() - 1 - i)] *= k; g.R[(size_t)(g.n() - 1 - i)] *= k; }
    float pk = 1.0e-6f;
    for (int i = 0; i < g.n(); ++i) pk = std::max(pk, std::max(std::fabs(g.L[(size_t)i]), std::fabs(g.R[(size_t)i])));
    const float norm = 0.891f / pk;
    auto s = std::make_shared<SampleData>();
    s->sampleRate = g.sr;
    s->name = name;
    s->buffer.setSize(2, g.n());
    for (int i = 0; i < g.n(); ++i) { s->buffer.setSample(0, i, g.L[(size_t)i] * norm); s->buffer.setSample(1, i, g.R[(size_t)i] * norm); }
    s->computeOverview();
    return s;
}
}  // namespace

std::shared_ptr<SampleData> render(const juce::String& id, double sr) {
    auto make = [&](double secs) { return Gen(sr, secs); };
    if (id == "kick_boom") { auto g = make(0.7); body(g, 160, 48, 0.035f, 0.32f, 1.f, 0.2f); click(g, 0.25f); return finish(g, id); }
    if (id == "kick_punch") { auto g = make(0.45); body(g, 230, 55, 0.02f, 0.18f, 1.f, 0.5f); click(g, 0.45f, 4000); return finish(g, id); }
    if (id == "kick_808") { auto g = make(1.8); body(g, 130, 44, 0.04f, 0.75f, 1.f, 0.35f); click(g, 0.1f); return finish(g, id); }
    if (id == "kick_acoustic") { auto g = make(0.5); body(g, 120, 62, 0.02f, 0.15f, 1.f); noise(g, Biquad::LP, 1500, 0.7f, 0.03f, 0.5f); click(g, 0.3f, 2500); return finish(g, id); }
    if (id == "snare_tight") { auto g = make(0.3); body(g, 260, 190, 0.02f, 0.06f, 0.6f); noise(g, Biquad::HP, 1800, 0.7f, 0.09f, 0.9f); return finish(g, id); }
    if (id == "snare_fat") { auto g = make(0.5); body(g, 210, 170, 0.03f, 0.09f, 0.7f); tones(g, { 330.f }, 0.05f, 0.3f); noise(g, Biquad::BP, 4500, 0.5f, 0.16f, 1.f); return finish(g, id); }
    if (id == "snare_rim") { auto g = make(0.12); tones(g, { 480.f, 1700.f }, 0.02f, 0.8f); noise(g, Biquad::BP, 2500, 2.f, 0.008f, 0.8f); return finish(g, id); }
    if (id == "snare_acoustic") { auto g = make(0.45); body(g, 200, 180, 0.02f, 0.07f, 0.6f); tones(g, { 330.f, 520.f }, 0.04f, 0.25f); noise(g, Biquad::BP, 5500, 0.6f, 0.13f, 0.9f); noise(g, Biquad::LP, 9000, 0.7f, 0.22f, 0.2f); return finish(g, id); }
    if (id == "clap" || id == "clap_big") {
        const bool big = id == "clap_big";
        auto g = make(big ? 0.7 : 0.4);
        const int bursts = big ? 5 : 4;
        for (int b = 0; b < bursts; ++b) noise(g, Biquad::BP, 1200, 1.4f, 0.008f, 1.f, 0.0005f, 0.011f * (float)b * (big ? 1.3f : 1.f));
        noise(g, Biquad::BP, 1100, 1.0f, big ? 0.25f : 0.13f, 0.6f, 0.f, 0.011f * (float)bursts);
        return finish(g, id);
    }
    if (id == "snap") { auto g = make(0.15); noise(g, Biquad::BP, 2600, 3.f, 0.012f, 1.f); click(g, 0.4f, 6000); return finish(g, id); }
    if (id == "hat_closed") { auto g = make(0.12); metal(g, 1.f, 7000, 0.035f, 1.f); noise(g, Biquad::HP, 8000, 0.7f, 0.025f, 0.3f); return finish(g, id); }
    if (id == "hat_closed_tight") { auto g = make(0.07); metal(g, 1.1f, 8000, 0.018f, 1.f); noise(g, Biquad::HP, 9000, 0.7f, 0.012f, 0.3f); return finish(g, id); }
    if (id == "hat_open") { auto g = make(0.8); metal(g, 1.f, 6500, 0.28f, 1.f); noise(g, Biquad::HP, 8000, 0.7f, 0.2f, 0.25f); return finish(g, id); }
    if (id == "hat_pedal") { auto g = make(0.15); metal(g, 0.95f, 5000, 0.05f, 1.f, 9000.f); return finish(g, id); }
    if (id == "tom_low") { auto g = make(0.6); body(g, 120, 82, 0.05f, 0.3f, 1.f); noise(g, Biquad::LP, 2000, 0.7f, 0.02f, 0.2f); return finish(g, id); }
    if (id == "tom_mid") { auto g = make(0.5); body(g, 170, 120, 0.05f, 0.25f, 1.f); noise(g, Biquad::LP, 2500, 0.7f, 0.02f, 0.2f); return finish(g, id); }
    if (id == "tom_high") { auto g = make(0.45); body(g, 240, 175, 0.05f, 0.2f, 1.f); noise(g, Biquad::LP, 3000, 0.7f, 0.02f, 0.2f); return finish(g, id); }
    if (id == "crash") { auto g = make(2.5); metal(g, 1.35f, 4000, 0.9f, 0.7f); noise(g, Biquad::HP, 5000, 0.6f, 0.8f, 0.6f); return finish(g, id); }
    if (id == "ride") { auto g = make(2.0); metal(g, 1.7f, 5000, 0.7f, 0.5f); tones(g, { 2410.f, 3570.f, 5120.f }, 0.9f, 0.15f); noise(g, Biquad::HP, 7000, 0.7f, 0.4f, 0.25f); return finish(g, id); }
    if (id == "ride_bell") { auto g = make(1.4); tones(g, { 1780.f, 2470.f, 3380.f, 4310.f }, 0.6f, 0.6f); metal(g, 1.9f, 6000, 0.2f, 0.2f); return finish(g, id); }
    if (id == "china") {
        auto g = make(1.6); metal(g, 1.2f, 2500, 0.6f, 0.8f, 3200.f); noise(g, Biquad::BP, 3500, 0.8f, 0.5f, 0.5f);
        for (auto& v : g.L) v = fastTanh(v * 3.f); for (auto& v : g.R) v = fastTanh(v * 3.f);
        return finish(g, id);
    }
    if (id == "shaker") { auto g = make(0.2); noise(g, Biquad::HP, 6000, 0.7f, 0.04f, 1.f, 0.012f); noise(g, Biquad::HP, 7000, 0.7f, 0.03f, 0.6f, 0.01f, 0.06f); return finish(g, id); }
    if (id == "cowbell") {
        auto g = make(0.45);
        double p1 = 0, p2 = 0; Biquad b; b.set(Biquad::BP, 800, 1.5f, 0, (float)sr);
        for (int i = 0; i < g.n(); ++i) { p1 += 540.0 / sr; p2 += 800.0 / sr; const float s = ((std::fmod(p1, 1.0) < 0.5) ? 1.f : -1.f) + ((std::fmod(p2, 1.0) < 0.5) ? 1.f : -1.f);
            g.addMono(i, b.tick(s) * std::exp(-g.t(i) / (g.t(i) < 0.01f ? 0.02f : 0.12f))); }
        return finish(g, id);
    }
    if (id == "clave") { auto g = make(0.12); tones(g, { 2500.f }, 0.03f, 1.f); return finish(g, id); }
    if (id == "conga_hi") { auto g = make(0.35); body(g, 420, 360, 0.01f, 0.14f, 1.f); click(g, 0.3f, 2000); return finish(g, id); }
    if (id == "conga_lo") { auto g = make(0.45); body(g, 280, 235, 0.01f, 0.2f, 1.f); click(g, 0.25f, 1500); return finish(g, id); }
    if (id == "tamb") {
        auto g = make(0.45);
        for (int k = 0; k < 3; ++k) metal(g, 4.f + (float)k * 0.37f, 7000, 0.08f + 0.04f * (float)k, 0.4f);
        noise(g, Biquad::HP, 9000, 0.7f, 0.15f, 0.6f, 0.003f);
        return finish(g, id);
    }
    if (id == "woodblock") { auto g = make(0.15); tones(g, { 1150.f, 2900.f }, 0.035f, 1.f); click(g, 0.2f, 1500); return finish(g, id); }
    if (id == "triangle") { auto g = make(2.0); tones(g, { 4500.f, 6870.f, 9280.f }, 1.2f, 0.7f); click(g, 0.1f, 8000); return finish(g, id); }
    if (id == "zap") {
        auto g = make(0.25); double ph = 0;
        for (int i = 0; i < g.n(); ++i) { const float t = g.t(i); ph += (60.0 + 2400.0 * std::exp(-t / 0.03f)) / sr; g.addMono(i, std::sin((float)(ph * kTwoPi)) * std::exp(-t / 0.08f)); }
        return finish(g, id);
    }
    if (id == "noise_sweep") {
        auto g = make(2.0); SVF f;
        for (int i = 0; i < g.n(); ++i) { const float t = g.t(i) / 2.f; f.set(200.f * std::pow(40.f, t), 4.f, (float)sr); f.tick(g.rng.bi()); g.addMono(i, f.bp * t * t); }
        return finish(g, id);
    }
    return nullptr;
}
}  // namespace DrumSynth

// =================================================================== sample bank
SampleBank& SampleBank::get() { static SampleBank b; return b; }

SampleBank::SampleBank() { fmt.registerBasicFormats(); }

juce::StringArray SampleBank::supportedExtensions() {
    juce::StringArray e { "wav", "aif", "aiff", "flac", "ogg", "mp3" };
   #if JUCE_MAC || JUCE_IOS
    e.addArray({ "m4a", "aac", "caf" });
   #endif
   #if JUCE_WINDOWS
    e.addArray({ "wma", "m4a" });
   #endif
    return e;
}

void SampleBank::setProjectDir(const juce::File& d) { std::lock_guard<std::mutex> g(lock); projDir = d; }
juce::File SampleBank::projectDir() const { std::lock_guard<std::mutex> g(lock); return projDir; }
juce::File SampleBank::libraryDir() const { return Paths::library(); }

juce::File SampleBank::resolve(const juce::String& ref) const {
    if (ref.startsWith("builtin:")) return {};
    if (ref.startsWith("lib:")) return libraryDir().getChildFile(ref.substring(4));
    if (juce::File::isAbsolutePath(ref)) return juce::File(ref);
    return projectDir().getChildFile(ref);
}

std::shared_ptr<SampleData> SampleBank::decodeFile(const juce::File& f, juce::String& error) {
    auto& fm = get().fmt;
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    if (!r) { error = "Unsupported or unreadable audio file: " + f.getFileName(); return nullptr; }
    if (r->lengthInSamples <= 0) { error = "The file contains no audio."; return nullptr; }
    if (r->lengthInSamples > (juce::int64)r->sampleRate * 60 * 20) { error = "Files longer than 20 minutes are not supported on mobile."; return nullptr; }
    auto s = std::make_shared<SampleData>();
    const int ch = (int)std::min<unsigned int>(2u, r->numChannels);
    s->buffer.setSize(ch, (int)r->lengthInSamples);
    r->read(&s->buffer, 0, (int)r->lengthInSamples, 0, true, ch > 1);
    s->sampleRate = r->sampleRate > 0 ? r->sampleRate : 44100.0;
    s->name = f.getFileNameWithoutExtension();
    s->path = f.getFullPathName();
    s->computeOverview();
    return s;
}

SamplePtr SampleBank::loadInternal(const juce::String& ref) {
    std::shared_ptr<SampleData> s;
    if (ref.startsWith("builtin:")) s = DrumSynth::render(ref.substring(8), 48000.0);
    else { juce::String err; s = decodeFile(resolve(ref), err); }
    return s;
}

SamplePtr SampleBank::loadNow(const juce::String& ref) {
    if (ref.isEmpty()) return nullptr;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = cache.find(ref);
        if (it != cache.end()) return it->second;
    }
    auto s = loadInternal(ref);
    std::lock_guard<std::mutex> g(lock);
    if (s) cache[ref] = s;
    return s;
}

SamplePtr SampleBank::request(const juce::String& ref) {
    if (ref.isEmpty()) return nullptr;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = cache.find(ref);
        if (it != cache.end()) return it->second;
        if (pending.count(ref)) return nullptr;
        pending[ref] = true;
    }
    pool.addJob([this, ref] {
        auto s = loadInternal(ref);
        {
            std::lock_guard<std::mutex> g(lock);
            pending.erase(ref);
            if (s) cache[ref] = s;
        }
        juce::MessageManager::callAsync([this] { if (onLoaded) onLoaded(); });
    });
    return nullptr;
}

void SampleBank::purgeUnused() {
    std::lock_guard<std::mutex> g(lock);
    for (auto it = cache.begin(); it != cache.end();)
        it = it->second.use_count() == 1 ? cache.erase(it) : std::next(it);
}

void SampleBank::clear() { std::lock_guard<std::mutex> g(lock); cache.clear(); }

size_t SampleBank::memoryBytes() const {
    std::lock_guard<std::mutex> g(lock);
    size_t b = 0;
    for (auto& [k, s] : cache) if (s) b += (size_t)s->buffer.getNumChannels() * (size_t)s->frames() * sizeof(float);
    return b;
}

// =================================================================== built-in packs
namespace {
bool writeWav(const juce::File& f, const juce::AudioBuffer<float>& b, double sr) {
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::OutputStream>(f.createOutputStream().release());
    if (!stream) return false;
    auto opts = juce::AudioFormatWriterOptions{}.withSampleRate(sr).withNumChannels(b.getNumChannels()).withBitsPerSample(24);
    auto w = wav.createWriterFor(stream, opts);
    if (!w) return false;
    return w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}


// Sequences one-shots into a drum loop: pattern string per sound, 16 steps per bar.
juce::AudioBuffer<float> drumLoop(double sr, double bpm, int bars, const std::vector<std::pair<juce::String, juce::String>>& lanes, float swing = 0.f) {
    const double step = 60.0 / bpm / 4.0;
    const int len = (int)(step * 16 * bars * sr);
    juce::AudioBuffer<float> out(2, len);
    out.clear();
    for (auto& [snd, pat] : lanes) {
        auto s = DrumSynth::render(snd, sr);
        if (!s) continue;
        for (int b = 0; b < bars; ++b)
            for (int i = 0; i < 16; ++i) {
                const juce::juce_wchar c = pat[i % pat.length()];
                if (c == '.' || c == '-') continue;
                const float vel = c == 'x' ? 0.6f : (c == 'X' ? 1.f : 0.4f);
                double t = (b * 16 + i) * step + ((i & 1) ? swing * step : 0.0);
                const int start = (int)(t * sr);
                for (int k = 0; k < s->frames() && start + k < len; ++k)
                    for (int ch = 0; ch < 2; ++ch) out.addSample(ch, start + k, s->buffer.getSample(ch, k) * vel * 0.7f);
            }
    }
    float pk = out.getMagnitude(0, len);
    if (pk > 0.f) out.applyGain(0.891f / pk);
    return out;
}

juce::AudioBuffer<float> synthTone(double sr, double secs, std::function<void(float*, float*, int)> fill) {
    juce::AudioBuffer<float> b(2, (int)(sr * secs));
    b.clear();
    fill(b.getWritePointer(0), b.getWritePointer(1), b.getNumSamples());
    const int fade = std::min(b.getNumSamples(), (int)(sr * 0.02));
    b.applyGainRamp(b.getNumSamples() - fade, fade, 1.f, 0.f);
    const float pk = b.getMagnitude(0, b.getNumSamples());
    if (pk > 0.f) b.applyGain(0.891f / pk);
    return b;
}
}  // namespace

void SampleBank::ensureBuiltinPacks(std::function<void(float)> progress) {
    const auto root = libraryDir().getChildFile("Packs");
    const auto marker = root.getChildFile(".generated-v1");
    if (marker.existsAsFile()) return;
    const double sr = 44100.0;
    struct Item { juce::String path; std::function<juce::AudioBuffer<float>()> make; };
    std::vector<Item> items;
    auto shot = [&](const juce::String& folder, const juce::String& file, const juce::String& id) {
        items.push_back({ folder + "/" + file + ".wav", [id, sr] { auto s = DrumSynth::render(id, sr); return s ? s->buffer : juce::AudioBuffer<float>(); } });
    };
    // Hip-hop drums
    for (auto [f, id] : std::vector<std::pair<const char*, const char*>> { { "HH Kick", "kick_boom" }, { "HH Snare", "snare_fat" }, { "HH Clap", "clap" },
                                                                          { "HH Hat Closed", "hat_closed" }, { "HH Hat Open", "hat_open" }, { "HH Rim", "snare_rim" } })
        shot("Hip-Hop Drums", f, id);
    items.push_back({ "Hip-Hop Drums/HH Loop 90 BPM.wav", [sr] { return drumLoop(sr, 90, 2, { { "kick_boom", "X.....x...X....." }, { "snare_fat", "....X.......X..." }, { "hat_closed", "x.x.x.x.x.x.x.xo" } }, 0.18f); } });
    items.push_back({ "Hip-Hop Drums/HH Loop 84 BPM.wav", [sr] { return drumLoop(sr, 84, 2, { { "kick_acoustic", "X..x......X..x.." }, { "snare_acoustic", "....X.......X..." }, { "hat_closed", "xxxxxxxxxxxxxxxx" } }, 0.12f); } });
    // Trap drums
    for (auto [f, id] : std::vector<std::pair<const char*, const char*>> { { "Trap 808 Kick", "kick_808" }, { "Trap Snare", "snare_tight" }, { "Trap Clap", "clap_big" },
                                                                          { "Trap Hat", "hat_closed_tight" }, { "Trap Open Hat", "hat_open" }, { "Trap Snap", "snap" } })
        shot("Trap Drums", f, id);
    items.push_back({ "Trap Drums/Trap Loop 140 BPM.wav", [sr] { return drumLoop(sr, 140, 2, { { "kick_808", "X......X..X....." }, { "clap_big", "........X......." }, { "hat_closed_tight", "xxxxxxxxxoxoxxxx" } }); } });
    // Electronic drums
    for (auto [f, id] : std::vector<std::pair<const char*, const char*>> { { "EL Kick", "kick_punch" }, { "EL Snare", "snare_tight" }, { "EL Clap", "clap" },
                                                                          { "EL Hat", "hat_closed" }, { "EL Open Hat", "hat_open" }, { "EL Crash", "crash" }, { "EL Ride", "ride" } })
        shot("Electronic Drums", f, id);
    items.push_back({ "Electronic Drums/House Loop 124 BPM.wav", [sr] { return drumLoop(sr, 124, 2, { { "kick_punch", "X...X...X...X..." }, { "clap", "....X.......X..." }, { "hat_open", "..x...x...x...x." }, { "hat_closed", "x.x.x.x.x.x.x.x." } }); } });
    items.push_back({ "Electronic Drums/Breakbeat 170 BPM.wav", [sr] { return drumLoop(sr, 170, 2, { { "kick_punch", "X.........X....." }, { "snare_tight", "....X..x.x..X..." }, { "ride", "x.x.x.x.x.x.x.x." } }); } });
    // Percussion
    for (auto [f, id] : std::vector<std::pair<const char*, const char*>> { { "Shaker", "shaker" }, { "Cowbell", "cowbell" }, { "Clave", "clave" }, { "Conga High", "conga_hi" },
                                                                          { "Conga Low", "conga_lo" }, { "Tambourine", "tamb" }, { "Woodblock", "woodblock" }, { "Triangle", "triangle" } })
        shot("Percussion", f, id);
    items.push_back({ "Percussion/Perc Loop 110 BPM.wav", [sr] { return drumLoop(sr, 110, 2, { { "shaker", "xxxxxxxxxxxxxxxx" }, { "conga_hi", "..X...X..X....X." }, { "conga_lo", "X.....X.....X..." }, { "clave", "X..X..X...X.X..." } }); } });
    // Ambient textures
    items.push_back({ "Ambient Textures/Warm Drone C.wav", [sr] { return synthTone(sr, 8.0, [sr](float* L, float* R, int n) {
        double p[6] = {}; const double f[6] = { 65.41, 65.6, 130.81, 196.0, 261.6, 392.0 };
        for (int i = 0; i < n; ++i) { const float t = (float)(i / sr), env = std::min(1.f, t / 2.f);
            float l = 0, r = 0; for (int k = 0; k < 6; ++k) { p[k] += f[k] / sr; const float s = std::sin((float)(p[k] * kTwoPi)) / (1.f + k * 0.6f) * (1.f + 0.3f * std::sin(t * (0.3f + k * 0.17f)));
                if (k & 1) l += s; else r += s; l += s * 0.3f; r += s * 0.3f; }
            L[i] = l * env; R[i] = r * env; } }); } });
    items.push_back({ "Ambient Textures/Air Texture.wav", [sr] { return synthTone(sr, 8.0, [sr](float* L, float* R, int n) {
        Rng rng(7); SVF a, b;
        for (int i = 0; i < n; ++i) { const float t = (float)(i / sr);
            a.set(800.f + 600.f * std::sin(t * 0.7f), 6.f, (float)sr); b.set(1300.f + 700.f * std::sin(t * 0.43f + 1.f), 6.f, (float)sr);
            a.tick(rng.bi()); b.tick(rng.bi()); const float env = std::min(1.f, t / 1.5f); L[i] = a.bp * env; R[i] = b.bp * env; } }); } });
    items.push_back({ "Ambient Textures/Glass Shimmer.wav", [sr] { return synthTone(sr, 6.0, [sr](float* L, float* R, int n) {
        const double f[5] = { 1046.5, 1318.5, 1568.0, 2093.0, 2637.0 };
        for (int i = 0; i < n; ++i) { const float t = (float)(i / sr); float l = 0, r = 0;
            for (int k = 0; k < 5; ++k) { const float tt = t - 0.6f * (float)k; if (tt < 0) continue; const float s = std::sin((float)(kTwoPi * f[k] * tt)) * std::exp(-tt / 2.5f) * 0.4f; if (k & 1) l += s; else r += s; }
            L[i] = l; R[i] = r; } }); } });
    // Bass sounds
    items.push_back({ "Bass Sounds/Sub Bass C1.wav", [sr] { return synthTone(sr, 2.0, [sr](float* L, float* R, int n) {
        for (int i = 0; i < n; ++i) { const float t = (float)(i / sr); const float v = std::sin(kTwoPi * 32.7f * t) * std::min(1.f, t / 0.005f) * std::exp(-t / 1.5f); L[i] = R[i] = v; } }); } });
    items.push_back({ "Bass Sounds/808 Bass A1.wav", [sr] { auto s = DrumSynth::render("kick_808", sr); return s ? s->buffer : juce::AudioBuffer<float>(); } });
    items.push_back({ "Bass Sounds/Reese Bass E1.wav", [sr] { return synthTone(sr, 2.5, [sr](float* L, float* R, int n) {
        Osc a, b, c; SVF fl, fr2;
        for (int i = 0; i < n; ++i) { const float t = (float)(i / sr); fl.set(400.f + 300.f * std::sin(t * 2.f), 1.2f, (float)sr); fr2 = fl;
            const float x = a.tick(kSaw, 41.2f / (float)sr) + b.tick(kSaw, 41.6f / (float)sr) + c.tick(kSaw, 40.8f / (float)sr);
            fl.tick(x); L[i] = R[i] = fl.lp * std::min(1.f, t / 0.01f); } }); } });
    items.push_back({ "Bass Sounds/Pluck Bass G1.wav", [sr] { return synthTone(sr, 1.2, [sr](float* L, float* R, int n) {
        Osc a; SVF f;
        for (int i = 0; i < n; ++i) { const float t = (float)(i / sr); f.set(150.f + 3000.f * std::exp(-t / 0.08f), 2.f, (float)sr); f.tick(a.tick(kSaw, 49.0f / (float)sr));
            L[i] = R[i] = f.lp * std::exp(-t / 0.5f); } }); } });
    // Sound effects
    shot("Sound Effects", "Laser Zap", "zap");
    shot("Sound Effects", "Noise Riser", "noise_sweep");
    items.push_back({ "Sound Effects/Downlifter.wav", [sr] { return synthTone(sr, 2.0, [sr](float* L, float* R, int n) {
        Rng rng(3); SVF f; for (int i = 0; i < n; ++i) { const float t = (float)(i / sr) / 2.f; f.set(8000.f * std::pow(0.02f, t), 3.f, (float)sr); f.tick(rng.bi()); L[i] = R[i] = f.bp * (1.f - t); } }); } });
    items.push_back({ "Sound Effects/Impact Boom.wav", [sr] { return synthTone(sr, 3.0, [sr](float* L, float* R, int n) {
        Rng rng(11); OnePole lp; lp.setHz(300.f, (float)sr); double ph = 0;
        for (int i = 0; i < n; ++i) { const float t = (float)(i / sr); ph += (35.0 + 90.0 * std::exp(-t / 0.1f)) / sr;
            const float v = std::sin((float)(ph * kTwoPi)) * std::exp(-t / 1.2f) + lp.lp(rng.bi()) * std::exp(-t / 0.6f) * 2.f; L[i] = R[i] = fastTanh(v * 1.5f); } }); } });
    items.push_back({ "Sound Effects/Vinyl Crackle Loop.wav", [sr] { return synthTone(sr, 4.0, [sr](float* L, float* R, int n) {
        Rng rng(5); OnePole hp; hp.setHz(1000.f, (float)sr);
        for (int i = 0; i < n; ++i) { float v = hp.hp(rng.bi()) * 0.03f; if (rng.uni() < 0.0007f) v += rng.bi() * 0.8f; L[i] = v; R[i] = v * 0.9f + rng.bi() * 0.01f; } }); } });

    int done = 0;
    for (auto& it : items) {
        const auto f = root.getChildFile(it.path);
        if (!f.existsAsFile()) {
            auto b = it.make();
            if (b.getNumSamples() > 0) writeWav(f, b, sr);
        }
        if (progress) progress((float)++done / (float)items.size());
    }
    marker.replaceWithText("generated by Ostinato; royalty-free, synthesized at install time\n");
}
