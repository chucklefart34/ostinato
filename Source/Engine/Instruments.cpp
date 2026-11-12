#include "Instruments.h"
#include "Dsp.h"
#include <juce_dsp/juce_dsp.h>
#include <mutex>

using namespace dsp;

namespace {

// ---------------------------------------------------------------- fast sine
struct SineTable {
    static constexpr int N = 4096;
    float t[N + 1];
    SineTable() { for (int i = 0; i <= N; ++i) t[i] = std::sin(kTwoPi * (float)i / N); }
    inline float operator()(float phase) const {  // phase in cycles, any value
        phase -= std::floor(phase);
        const float x = phase * N;
        const int i = (int)x;
        return t[i] + (t[i + 1] - t[i]) * (x - (float)i);
    }
};
const SineTable& sine() { static const SineTable s; return s; }

// Reads a ParamSet by id with indices resolved once.
struct P {
    const ParamSet* ps = nullptr;
    std::vector<int> idx;
    void bind(const ParamSet* p, std::initializer_list<const char*> ids) {
        ps = p;
        idx.clear();
        for (auto* id : ids) idx.push_back(p->indexOf(id));
    }
    float operator[](int k) const { const int i = idx[(size_t)k]; return i >= 0 ? ps->get(i) : 0.f; }
};

// Common per-block values for melodic engines.
struct Common {
    float ampA = 0.005f, ampD = 0.3f, ampS = 0.8f, ampR = 0.25f;
    float glideMs = 0.f;
    bool mono = false, legato = false;
    float bendRange = 2.f, transpose = 0.f, velSens = 0.7f, level = 0.5f;
    int lfoShape = 0;
    float lfoRate = 5.f, lfoPitchCt = 0.f, lfoCutoff = 0.f, lfoAmp = 0.f;
    int lfoSync = 0;
    float modWheelVib = 50.f;   // cents of LFO vibrato at full mod wheel (engines with a mod matrix set 0)
};

// ---------------------------------------------------------------- voice-managing engine
// V must provide: start(note, vel, detune, legato, const S&), release(), kill(),
// isActive(), render(L, R, n, const S&, float pitchOffsetSt, float ampMod), note, glideTo(note)
template <class V, class S, int MaxVoices = 16>
class PolyEngine : public Instrument {
public:
    explicit PolyEngine(ParamSetPtr p) : params(std::move(p)) {}

    void prepare(double sampleRate, int) override {
        sr = (float)sampleRate;
        for (auto& v : voices) v.prepare(sr);
        static const char* shapeIds[] = { "sh_fine", "sh_hp", "sh_lp", "sh_drive", "sh_width" };
        for (int k = 0; k < 5; ++k) shIdx[k] = params ? params->indexOf(shapeIds[k]) : -1;
        for (int c = 0; c < 2; ++c) { shHp[c].reset(); shLp[c].reset(); }
        onPrepare();
    }
    // Engines that set pitch only when a note starts (modal piano) take the Shape fine tune there.
    virtual bool followsPitchOffset() const { return true; }
    float fineTune() const { return shIdx[0] >= 0 ? params->get(shIdx[0]) * 0.01f : 0.f; }
    // Maximum simultaneous notes (engines with a "voices" setting override this).
    virtual int voiceLimit() const { return MaxVoices; }
    virtual void onPrepare() {}
    virtual void updateShared(S& s) = 0;     // read params into s (once per render call)
    virtual void postProcess(float*, float*, int) {}

    void noteOn(int note, float vel, float detune) override {
        updateShared(shared);
        if (!followsPitchOffset()) detune += fineTune();
        note = juce::jlimit(0, 127, note + (int)shared.c.transpose);
        held[(size_t)note] = true;
        heldOrder.removeFirstMatchingValue(note);
        heldOrder.add(note);
        const float velGain = 1.f - shared.c.velSens + shared.c.velSens * vel;
        if (shared.c.mono) {
            auto& v = voices[0];
            const bool legatoNow = shared.c.legato && v.isActive() && !v.released;
            if (v.isActive() && (shared.c.glideMs > 0.f || legatoNow)) v.glideTo((float)note + detune, shared.c.glideMs, sr);
            else v.snapPitch((float)note + detune);
            if (!legatoNow) v.start(note, vel, velGain, detune, shared);
            else v.note = note;
            v.age = ++ageCounter;
            for (int i = 1; i < MaxVoices; ++i) voices[(size_t)i].kill();
            return;
        }
        V* target = nullptr;
        const int lim = juce::jlimit(1, MaxVoices, voiceLimit());
        for (int k = 0; k < lim && !target; ++k) if (voices[(size_t)k].isActive() && voices[(size_t)k].note == note) target = &voices[(size_t)k];   // retrigger same note
        for (int k = 0; k < lim && !target; ++k) if (!voices[(size_t)k].isActive()) target = &voices[(size_t)k];
        if (!target) {  // steal: oldest released voice first, else oldest
            uint32_t best = UINT32_MAX;
            for (int k = 0; k < lim; ++k) { auto& v = voices[(size_t)k]; if (v.released && v.age < best) { best = v.age; target = &v; } }
            if (!target) for (int k = 0; k < lim; ++k) { auto& v = voices[(size_t)k]; if (v.age < best) { best = v.age; target = &v; } }
        }
        if (shared.c.glideMs > 0.f && lastNote >= 0) { target->snapPitch((float)lastNote); target->glideTo((float)note + detune, shared.c.glideMs, sr); }
        else target->snapPitch((float)note + detune);
        target->start(note, vel, velGain, detune, shared);
        target->age = ++ageCounter;
        lastNote = note;
    }

    void noteOff(int note) override {
        note = juce::jlimit(0, 127, note + (int)shared.c.transpose);
        held[(size_t)note] = false;
        heldOrder.removeFirstMatchingValue(note);
        if (shared.c.mono) {
            auto& v = voices[0];
            if (v.note != note) return;
            if (!heldOrder.isEmpty()) {           // fall back to previous held note (legato)
                const int prev = heldOrder.getLast();
                v.glideTo((float)prev, std::max(shared.c.glideMs, 1.f), sr);
                v.note = prev;
                return;
            }
            if (sustain) { sustained[(size_t)note] = true; return; }
            v.release();
            return;
        }
        for (auto& v : voices)
            if (v.isActive() && v.note == note && !v.released) {
                if (sustain) sustained[(size_t)note] = true;
                else v.release();
            }
    }

    void setSustain(bool on) override {
        Instrument::setSustain(on);
        if (!on)
            for (int n = 0; n < 128; ++n)
                if (sustained[(size_t)n]) {
                    sustained[(size_t)n] = false;
                    if (!held[(size_t)n]) for (auto& v : voices) if (v.note == n && v.isActive()) v.release();
                }
    }

    void allNotesOff(bool hard) override {
        for (auto& v : voices) { if (hard) v.kill(); else if (v.isActive()) v.release(); }
        held.fill(false); sustained.fill(false); heldOrder.clearQuick();
    }

    int activeVoices() const override {
        int n = 0;
        for (auto& v : voices) n += v.isActive() ? 1 : 0;
        return n;
    }

    void render(float* L, float* R, int n) override {
        updateShared(shared);
        // global LFO (control rate)
        float rate = shared.c.lfoRate;
        if (shared.c.lfoSync > 0 && transport) rate = (float)(transport->bpm / 60.0) / syncBeats(shared.c.lfoSync - 1);
        const float lfo = lfoState.advance(rate, sr, n, shared.c.lfoShape);
        shared.lfo = lfo;
        const float fine = shIdx[0] >= 0 ? params->get(shIdx[0]) * 0.01f : 0.f;
        const float pitchOff = bend * shared.c.bendRange + (shared.c.lfoPitchCt + modWheel * shared.c.modWheelVib) * lfo / 100.f + fine;
        const float ampMod = 1.f - shared.c.lfoAmp * 0.5f * (1.f + lfo);
        std::fill(tmpL, tmpL + n, 0.f);
        std::fill(tmpR, tmpR + n, 0.f);
        const int lim = juce::jlimit(1, MaxVoices, voiceLimit());
        for (int k = 0; k < MaxVoices; ++k) {
            auto& v = voices[(size_t)k];
            if (!v.isActive()) continue;
            if (k >= lim) { v.kill(); continue; }   // voice count was lowered
            v.render(tmpL, tmpR, n, shared, pitchOff, ampMod);
        }
        postProcess(tmpL, tmpR, n);
        applyShape(tmpL, tmpR, n);
        for (int i = 0; i < n; ++i) { L[i] += tmpL[i] * shared.c.level; R[i] += tmpR[i] * shared.c.level; }
    }

    // Shape section: low cut, high cut, warmth (soft saturation) and stereo width on the engine output.
    void applyShape(float* l, float* r, int n) {
        if (shIdx[1] < 0) return;
        const float hp = params->get(shIdx[1]), lp = params->get(shIdx[2]);
        const float drive = params->get(shIdx[3]) * 0.01f, width = params->get(shIdx[4]) * 0.01f;
        if (hp > 20.5f) {
            for (auto& f : shHp) f.set(hp, 0.707f, sr);
            for (int i = 0; i < n; ++i) { shHp[0].tick(l[i]); shHp[1].tick(r[i]); l[i] = shHp[0].hp; r[i] = shHp[1].hp; }
        } else for (auto& f : shHp) f.reset();
        if (lp < 19900.f) {
            for (auto& f : shLp) f.set(lp, 0.707f, sr);
            for (int i = 0; i < n; ++i) { shLp[0].tick(l[i]); shLp[1].tick(r[i]); l[i] = shLp[0].lp; r[i] = shLp[1].lp; }
        } else for (auto& f : shLp) f.reset();
        if (drive > 0.001f) {
            const float g = 1.f + drive * 6.f, comp = (1.f + drive) / g;
            for (int i = 0; i < n; ++i) { l[i] = fastTanh(l[i] * g) * comp; r[i] = fastTanh(r[i] * g) * comp; }
        }
        if (std::fabs(width - 1.f) > 0.001f)
            for (int i = 0; i < n; ++i) {
                const float m = 0.5f * (l[i] + r[i]), sd = 0.5f * (l[i] - r[i]) * width;
                l[i] = m + sd; r[i] = m - sd;
            }
    }

protected:
    void readCommon(const P& p, int base, bool hasEnv, bool hasLfo, Common& c) {
        // base = index in P of first common param in the order:
        // [a_a a_d a_s a_r] [lfo_shape lfo_rate lfo_sync lfo_pitch lfo_cutoff lfo_amp] glide mono legato bend transpose vel_sens level
        int k = base;
        if (hasEnv) { c.ampA = p[k++]; c.ampD = p[k++]; c.ampS = p[k++] * 0.01f; c.ampR = p[k++]; }
        if (hasLfo) {
            c.lfoShape = (int)p[k++]; c.lfoRate = p[k++]; c.lfoSync = (int)p[k++];
            c.lfoPitchCt = p[k++]; c.lfoCutoff = p[k++] * 0.01f; c.lfoAmp = p[k++] * 0.01f;
        }
        c.glideMs = p[k++]; c.mono = p[k++] >= 0.5f; c.legato = p[k++] >= 0.5f;
        c.bendRange = p[k++]; c.transpose = p[k++]; c.velSens = p[k++] * 0.01f; c.level = db2lin(p[k++]);
    }

    ParamSetPtr params;
    float sr = 48000.f;
    std::array<V, MaxVoices> voices;
    S shared;
    LFO lfoState;
    std::array<bool, 128> held {}, sustained {};
    juce::Array<int> heldOrder;
    uint32_t ageCounter = 0;
    int lastNote = -1;
    float tmpL[8192] {}, tmpR[8192] {};
    int shIdx[5] { -1, -1, -1, -1, -1 };
    SVF shHp[2], shLp[2];
};

// Voice base: pitch glide, age, note bookkeeping.
struct VoiceBase {
    int note = -1;
    uint32_t age = 0;
    bool released = false;
    float sr = 48000.f;
    float pitch = 60.f, pitchTarget = 60.f, glideCoef = 1.f;
    float vel = 1.f, velGain = 1.f, detune = 0.f;
    void prepare(float s) { sr = s; }
    void snapPitch(float p) { pitch = pitchTarget = p; glideCoef = 1.f; }
    void glideTo(float p, float ms, float s) { pitchTarget = p; glideCoef = ms <= 0.f ? 1.f : onePoleCoef(ms, s / (float)kQuantum) ; }
    inline float stepPitch() { pitch += (pitchTarget - pitch) * glideCoef; return pitch; }
};

// ================================================================ ANALOG
struct AnalogShared {
    Common c;
    float lfo = 0.f;
    int w1 = 0, w2 = 0, fltType = 0, unison = 1;
    float pw = 0.5f, semi2 = 0.f, fine2 = 0.f, mix = 0.5f, sub = 0.f, noise = 0.f, detune = 0.2f, width = 0.6f;
    float cutoff = 6000.f, reso = 0.15f, fltEnv = 0.3f, keyTrack = 0.5f, drive = 0.f;
    float fA = 0.005f, fD = 0.4f, fS = 0.3f, fR = 0.3f;
};

struct AnalogVoice : VoiceBase {
    static constexpr int MaxUni = 7;
    Osc o1[MaxUni], o2[MaxUni], subOsc;
    float uniDetune[MaxUni] {}, uniPan[MaxUni] {};
    SVF fl[2], fl2[2];
    ADSR amp, fenv;
    Rng rng { 777u };
    int counter = 0;

    bool isActive() const { return amp.active(); }
    void kill() { amp.kill(); fenv.kill(); released = false; }
    void release() { amp.noteOff(); fenv.noteOff(); released = true; }
    void start(int n, float v, float vg, float det, const AnalogShared& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false;
        const bool fresh = !amp.active();
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        fenv.set(s.fA, s.fD, s.fS, s.fR, sr);
        amp.noteOn(); fenv.noteOn();
        if (fresh) {
            for (int u = 0; u < MaxUni; ++u) { o1[u].phase = rng.uni(); o2[u].phase = rng.uni(); }
            for (auto& f : fl) f.reset();
            for (auto& f : fl2) f.reset();
        }
        counter = 0;
    }
    void render(float* L, float* R, int n, const AnalogShared& s, float pitchOff, float ampMod) {
        const int uni = juce::jlimit(1, MaxUni, s.unison);
        for (int u = 0; u < uni; ++u) {
            const float t = uni == 1 ? 0.f : (float)u / (float)(uni - 1) * 2.f - 1.f;
            uniDetune[u] = t * s.detune * 0.5f;                  // semitones
            uniPan[u] = t * s.width;
        }
        const float uniNorm = 1.f / std::sqrt((float)uni);
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        fenv.set(s.fA, s.fD, s.fS, s.fR, sr);
        const float p = stepPitch() + pitchOff;
        float dt1[MaxUni], dt2[MaxUni];
        for (int u = 0; u < uni; ++u) {
            dt1[u] = noteHz(p + uniDetune[u]) / sr;
            dt2[u] = noteHz(p + s.semi2 + s.fine2 * 0.01f + uniDetune[u]) / sr;
        }
        const float dtSub = noteHz(p - 12.f) / sr;
        const float res = 0.5f + s.reso * 12.f;
        const float keyOct = (p - 60.f) / 12.f * s.keyTrack;
        const float m1 = 1.f - s.mix, m2 = s.mix;
        const float drive = 1.f + s.drive * 4.f;
        for (int i = 0; i < n; ++i) {
            if ((counter++ & 15) == 0) {
                const float env = fenv.v;
                const float oct = s.fltEnv * env * 6.f + keyOct + s.c.lfoCutoff * s.lfo * 2.f + (vel - 0.8f) * 0.8f;
                const float hz = clampf(s.cutoff * std::pow(2.f, oct), 20.f, sr * 0.45f);
                for (int c = 0; c < 2; ++c) { fl[c].set(hz, s.fltType == 0 ? res * 0.7f : res, sr); fl2[c].set(hz, 0.54f, sr); }
            }
            float l = 0.f, r = 0.f;
            for (int u = 0; u < uni; ++u) {
                float x = (s.w1 == kNoise ? rng.bi() : o1[u].tick(s.w1, dt1[u], s.pw)) * m1;
                x += (s.w2 == kNoise ? rng.bi() : o2[u].tick(s.w2, dt2[u], s.pw)) * m2;
                const float pr = 0.5f + 0.5f * uniPan[u];
                l += x * (1.f - pr); r += x * pr;
            }
            l *= uniNorm * 1.4f; r *= uniNorm * 1.4f;
            const float extra = s.sub * subOsc.tick(kSine, dtSub) + s.noise * rng.bi() * 0.5f;
            l += extra; r += extra;
            float yl, yr;
            fl[0].tick(l * drive); fl[1].tick(r * drive);
            switch (s.fltType) {
                case 0: fl2[0].tick(fl[0].lp); fl2[1].tick(fl[1].lp); yl = fl2[0].lp; yr = fl2[1].lp; break;
                case 1: yl = fl[0].lp; yr = fl[1].lp; break;
                case 2: yl = fl[0].hp; yr = fl[1].hp; break;
                default: yl = fl[0].bp; yr = fl[1].bp; break;
            }
            if (s.drive > 0.f) { yl = fastTanh(yl) ; yr = fastTanh(yr); }
            const float a = amp.tick() * velGain * ampMod * 0.35f;
            fenv.tick();
            L[i] += yl * a; R[i] += yr * a;
        }
    }
};

class AnalogEngine : public PolyEngine<AnalogVoice, AnalogShared> {
public:
    using PolyEngine::PolyEngine;
    void onPrepare() override {
        p.bind(params.get(), { "osc1_wave", "osc1_pw", "osc2_wave", "osc2_semi", "osc2_fine", "osc_mix", "sub", "noise",
                               "unison", "uni_detune", "uni_width", "flt_type", "cutoff", "reso", "flt_env", "flt_key", "drive",
                               "f_a", "f_d", "f_s", "f_r",
                               "a_a", "a_d", "a_s", "a_r", "lfo_shape", "lfo_rate", "lfo_sync", "lfo_pitch", "lfo_cutoff", "lfo_amp",
                               "glide", "mono", "legato", "bend", "transpose", "vel_sens", "level" });
    }
    void updateShared(AnalogShared& s) override {
        s.w1 = (int)p[0]; s.pw = p[1] * 0.01f; s.w2 = (int)p[2]; s.semi2 = p[3]; s.fine2 = p[4]; s.mix = p[5] * 0.01f;
        s.sub = p[6] * 0.01f; s.noise = p[7] * 0.01f; s.unison = (int)p[8]; s.detune = p[9] * 0.01f; s.width = p[10] * 0.01f;
        s.fltType = (int)p[11]; s.cutoff = p[12]; s.reso = p[13] * 0.01f; s.fltEnv = p[14] * 0.01f; s.keyTrack = p[15] * 0.01f;
        s.drive = p[16] * 0.01f; s.fA = p[17]; s.fD = p[18]; s.fS = p[19] * 0.01f; s.fR = p[20];
        readCommon(p, 21, true, true, s.c);
    }
private:
    P p;
};

// ================================================================ FM (3-op stack)
struct FMShared {
    Common c;
    float lfo = 0.f;
    float rc = 1, r1 = 2, i1 = 2.5f, d1 = 0.8f, s1 = 0.25f, r2 = 1, i2 = 0, d2 = 0.4f, fb = 0, velIndex = 0.5f, tone = 20000.f;
};

struct FMVoice : VoiceBase {
    float pc = 0, p1 = 0, p2 = 0, fbState = 0, e1 = 0, e2 = 0;
    ADSR amp;
    OnePole lp[2];
    bool isActive() const { return amp.active(); }
    void kill() { amp.kill(); released = false; }
    void release() { amp.noteOff(); released = true; }
    void start(int n, float v, float vg, float det, const FMShared& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false;
        if (!amp.active()) { pc = p1 = p2 = 0.f; fbState = 0.f; lp[0].reset(); lp[1].reset(); }
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        amp.noteOn();
        e1 = 1.f; e2 = 1.f;
    }
    void render(float* L, float* R, int n, const FMShared& s, float pitchOff, float ampMod) {
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        const float f = noteHz(stepPitch() + pitchOff);
        const float dc = f * s.rc / sr, d1 = f * s.r1 / sr, d2 = f * s.r2 / sr;
        const float velScale = 1.f - s.velIndex + s.velIndex * vel * 1.3f;
        const float k1 = std::exp(-4.6f / (std::max(0.01f, s.d1) * sr));
        const float k2 = std::exp(-4.6f / (std::max(0.01f, s.d2) * sr));
        // keep the modulation index sane at high pitches (aliasing guard)
        const float hiGuard = clampf(4000.f / std::max(f, 1.f), 0.15f, 1.f);
        lp[0].setHz(s.tone, sr);
        const auto& sn = sine();
        for (int i = 0; i < n; ++i) {
            e1 = s.s1 + (e1 - s.s1) * k1;
            e2 *= k2;
            const float m2 = sn(p2 + fbState * s.fb * 0.25f) * s.i2 * e2 * velScale * hiGuard;
            fbState = m2;
            const float m1 = sn(p1 + m2 * 0.159f) * s.i1 * e1 * velScale * hiGuard;
            float y = sn(pc + m1 * 0.159f);
            pc += dc; p1 += d1; p2 += d2;
            if (pc > 1024.f) { pc -= 1024.f; p1 -= std::floor(p1); p2 -= std::floor(p2); }
            y = lp[0].lp(y);
            const float a = amp.tick() * velGain * ampMod * 0.4f;
            L[i] += y * a; R[i] += y * a;
        }
    }
};

class FMEngine : public PolyEngine<FMVoice, FMShared> {
public:
    using PolyEngine::PolyEngine;
    void onPrepare() override {
        p.bind(params.get(), { "ratio_c", "ratio_m1", "index_m1", "dec_m1", "sus_m1", "ratio_m2", "index_m2", "dec_m2", "feedback", "vel_index", "tone",
                               "a_a", "a_d", "a_s", "a_r", "lfo_shape", "lfo_rate", "lfo_sync", "lfo_pitch", "lfo_cutoff", "lfo_amp",
                               "glide", "mono", "legato", "bend", "transpose", "vel_sens", "level" });
    }
    void updateShared(FMShared& s) override {
        s.rc = p[0]; s.r1 = p[1]; s.i1 = p[2]; s.d1 = p[3]; s.s1 = p[4] * 0.01f; s.r2 = p[5]; s.i2 = p[6]; s.d2 = p[7];
        s.fb = p[8] * 0.01f; s.velIndex = p[9] * 0.01f; s.tone = p[10];
        readCommon(p, 11, true, true, s.c);
    }
private:
    P p;
};

// ================================================================ WAVETABLE
// 5 tables x 16 frames x 2048 samples, 11 band-limited mip levels each (built with FFT).
struct Wavetables {
    static constexpr int Size = 2048, Frames = 16, Mips = 11, Tables = 5;
    std::vector<float> data;   // [table][frame][mip][Size+1]
    float* at(int t, int f, int m) { return data.data() + ((((size_t)t * Frames + (size_t)f) * Mips + (size_t)m) * (Size + 1)); }

    Wavetables() {
        data.assign((size_t)Tables * Frames * Mips * (Size + 1), 0.f);
        juce::dsp::FFT fft(11);
        std::vector<float> buf(Size * 2), spec(Size * 2);
        for (int t = 0; t < Tables; ++t)
            for (int f = 0; f < Frames; ++f) {
                const float m = (float)f / (Frames - 1);
                for (int i = 0; i < Size; ++i) buf[(size_t)i] = gen(t, m, (float)i / Size);
                std::fill(buf.begin() + Size, buf.end(), 0.f);
                fft.performRealOnlyForwardTransform(buf.data());
                for (int mip = 0; mip < Mips; ++mip) {
                    const int maxH = (Size / 2) >> mip;
                    spec = buf;
                    spec[0] = spec[1] = 0.f;  // remove DC
                    for (int h = maxH + 1; h <= Size / 2; ++h) { spec[(size_t)h * 2] = 0.f; spec[(size_t)h * 2 + 1] = 0.f; }
                    fft.performRealOnlyInverseTransform(spec.data());
                    float* out = at(t, f, mip);
                    float peak = 1.0e-6f;
                    for (int i = 0; i < Size; ++i) { out[i] = spec[(size_t)i]; peak = std::max(peak, std::fabs(out[i])); }
                    for (int i = 0; i < Size; ++i) out[i] /= peak;
                    out[Size] = out[0];
                }
            }
    }
    static float gen(int table, float m, float ph) {
        switch (table) {
            case 0: {  // Basic: sine -> triangle -> saw -> square
                const float s = std::sin(ph * kTwoPi), tri = 1.f - 4.f * std::fabs(ph - 0.5f);
                const float saw = 2.f * ph - 1.f, sq = ph < 0.5f ? 1.f : -1.f;
                const float x = m * 3.f;
                if (x < 1.f) return lerp(s, std::cos((ph + 0.75f) * kTwoPi) * 0.f + tri, x);
                if (x < 2.f) return lerp(tri, saw, x - 1.f);
                return lerp(saw, sq, x - 2.f);
            }
            case 1: {  // Harmonic: additive, brightening and shifting odd/even balance
                float y = 0.f;
                const int nh = 2 + (int)(m * 40.f);
                for (int h = 1; h <= nh; ++h) {
                    const float odd = (h & 1) ? 1.f : (1.f - m * 0.8f);
                    y += std::sin(ph * kTwoPi * h) * odd / std::pow((float)h, 1.4f - m * 0.6f);
                }
                return y;
            }
            case 2: {  // Vocal: formant-shaped harmonic spectra through A-E-I-O-U
                static const float F[5][3] = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 350, 1700, 2700 }, { 450, 800, 2830 }, { 325, 700, 2530 } };
                const float vx = m * 4.f; const int a = std::min(3, (int)vx); const float fr = vx - a;
                float y = 0.f;
                for (int h = 1; h <= 60; ++h) {
                    const float hz = 110.f * h;
                    float g = 0.f;
                    for (int k = 0; k < 3; ++k) {
                        const float fc = lerp(F[a][k], F[a + 1][k], fr), bw = 80.f + 40.f * k;
                        g += std::exp(-0.5f * ((hz - fc) / bw) * ((hz - fc) / bw)) / (1.f + k);
                    }
                    y += std::sin(ph * kTwoPi * h) * (g + 0.02f / h);
                }
                return y;
            }
            case 3: {  // Digital: hard-synced saw sweep
                const float ratio = 1.f + m * 7.f;
                const float p2 = ph * ratio;
                return (2.f * (p2 - std::floor(p2)) - 1.f) * (1.f - ph * 0.3f);
            }
            default: {  // PWM
                const float w = 0.5f - m * 0.45f;
                return ph < w ? 1.f : -1.f;
            }
        }
    }
};
const Wavetables& wavetables() { static const Wavetables w; return w; }

struct WTShared {
    Common c;
    float lfo = 0.f;
    int table = 0, unison = 1, fltType = 0;
    float pos = 0, posEnv = 0, posLfo = 0, sub = 0, detune = 0.2f, width = 0.6f;
    float cutoff = 12000.f, reso = 0.1f, fltEnv = 0.f, keyTrack = 0.5f, drive = 0.f;
    float fA = 0.005f, fD = 0.4f, fS = 0.3f, fR = 0.3f;
};

struct WTVoice : VoiceBase {
    static constexpr int MaxUni = 7;
    float ph[MaxUni] {}, subPh = 0.f;
    SVF fl[2], fl2[2];
    ADSR amp, fenv;
    Rng rng { 4242u };
    int counter = 0;
    bool isActive() const { return amp.active(); }
    void kill() { amp.kill(); fenv.kill(); released = false; }
    void release() { amp.noteOff(); fenv.noteOff(); released = true; }
    void start(int n, float v, float vg, float det, const WTShared& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false;
        if (!amp.active()) { for (auto& p : ph) p = rng.uni(); for (auto& f : fl) f.reset(); for (auto& f : fl2) f.reset(); }
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        fenv.set(s.fA, s.fD, s.fS, s.fR, sr);
        amp.noteOn(); fenv.noteOn();
        counter = 0;
    }
    void render(float* L, float* R, int n, const WTShared& s, float pitchOff, float ampMod) {
        auto& wt = const_cast<Wavetables&>(wavetables());
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        fenv.set(s.fA, s.fD, s.fS, s.fR, sr);
        const int uni = juce::jlimit(1, MaxUni, s.unison);
        const float p = stepPitch() + pitchOff;
        const float f0 = noteHz(p);
        // mip: harmonics allowed below nyquist
        const float allowed = (sr * 0.5f) / std::max(f0 * std::pow(2.f, s.detune * 0.25f), 1.f);
        int mip = 0;
        while (mip < Wavetables::Mips - 1 && (float)((Wavetables::Size / 2) >> mip) > allowed) ++mip;
        const float position = clampf(s.pos + s.posEnv * fenv.v + s.posLfo * 0.5f * (s.lfo + 1.f), 0.f, 1.f) * (Wavetables::Frames - 1);
        const int fa = std::min((int)position, Wavetables::Frames - 2);
        const float ff = position - (float)fa;
        const float* ta = wt.at(s.table, fa, mip);
        const float* tb = wt.at(s.table, fa + 1, mip);
        float inc[MaxUni], pan[MaxUni];
        for (int u = 0; u < uni; ++u) {
            const float t = uni == 1 ? 0.f : (float)u / (float)(uni - 1) * 2.f - 1.f;
            inc[u] = noteHz(p + t * s.detune * 0.5f) / sr;
            pan[u] = 0.5f + 0.5f * t * s.width;
        }
        const float subInc = noteHz(p - 12.f) / sr;
        const float norm = 1.f / std::sqrt((float)uni);
        const float res = 0.5f + s.reso * 12.f;
        const float keyOct = (p - 60.f) / 12.f * s.keyTrack;
        const auto& sn = sine();
        for (int i = 0; i < n; ++i) {
            if ((counter++ & 15) == 0) {
                const float oct = s.fltEnv * fenv.v * 6.f + keyOct + s.c.lfoCutoff * s.lfo * 2.f;
                const float hz = clampf(s.cutoff * std::pow(2.f, oct), 20.f, sr * 0.45f);
                for (int c = 0; c < 2; ++c) { fl[c].set(hz, s.fltType == 0 ? res * 0.7f : res, sr); fl2[c].set(hz, 0.54f, sr); }
            }
            float l = 0.f, r = 0.f;
            for (int u = 0; u < uni; ++u) {
                const float x = ph[u] * Wavetables::Size;
                const int k = (int)x; const float fr = x - (float)k;
                const float a = ta[k] + (ta[k + 1] - ta[k]) * fr;
                const float b = tb[k] + (tb[k + 1] - tb[k]) * fr;
                const float y = a + (b - a) * ff;
                l += y * (1.f - pan[u]); r += y * pan[u];
                ph[u] += inc[u]; if (ph[u] >= 1.f) ph[u] -= 1.f;
            }
            const float sb = s.sub * sn(subPh);
            subPh += subInc; if (subPh >= 1.f) subPh -= 1.f;
            l = l * norm * 1.4f + sb; r = r * norm * 1.4f + sb;
            float yl, yr;
            fl[0].tick(l); fl[1].tick(r);
            switch (s.fltType) {
                case 0: fl2[0].tick(fl[0].lp); fl2[1].tick(fl[1].lp); yl = fl2[0].lp; yr = fl2[1].lp; break;
                case 1: yl = fl[0].lp; yr = fl[1].lp; break;
                case 2: yl = fl[0].hp; yr = fl[1].hp; break;
                default: yl = fl[0].bp; yr = fl[1].bp; break;
            }
            if (s.drive > 0.f) { const float d = 1.f + s.drive * 4.f; yl = fastTanh(yl * d); yr = fastTanh(yr * d); }
            const float g = amp.tick() * velGain * ampMod * 0.3f;
            fenv.tick();
            L[i] += yl * g; R[i] += yr * g;
        }
    }
};

class WavetableEngine : public PolyEngine<WTVoice, WTShared> {
public:
    using PolyEngine::PolyEngine;
    void onPrepare() override {
        (void)wavetables();
        p.bind(params.get(), { "table", "pos", "pos_env", "pos_lfo", "sub", "unison", "uni_detune", "uni_width",
                               "flt_type", "cutoff", "reso", "flt_env", "flt_key", "drive", "f_a", "f_d", "f_s", "f_r",
                               "a_a", "a_d", "a_s", "a_r", "lfo_shape", "lfo_rate", "lfo_sync", "lfo_pitch", "lfo_cutoff", "lfo_amp",
                               "glide", "mono", "legato", "bend", "transpose", "vel_sens", "level" });
    }
    void updateShared(WTShared& s) override {
        s.table = juce::jlimit(0, Wavetables::Tables - 1, (int)p[0]); s.pos = p[1] * 0.01f; s.posEnv = p[2] * 0.01f; s.posLfo = p[3] * 0.01f;
        s.sub = p[4] * 0.01f; s.unison = (int)p[5]; s.detune = p[6] * 0.01f; s.width = p[7] * 0.01f;
        s.fltType = (int)p[8]; s.cutoff = p[9]; s.reso = p[10] * 0.01f; s.fltEnv = p[11] * 0.01f; s.keyTrack = p[12] * 0.01f; s.drive = p[13] * 0.01f;
        s.fA = p[14]; s.fD = p[15]; s.fS = p[16] * 0.01f; s.fR = p[17];
        readCommon(p, 18, true, true, s.c);
    }
private:
    P p;
};

// ================================================================ PIANO (modal synthesis)
struct PianoShared {
    Common c;
    float lfo = 0.f;
    int model = 0;
    float bright = 0.55f, decay = 7.f, hammer = 0.35f, detuneCt = 1.2f, release = 0.25f, width = 0.6f;
};

struct PianoVoice : VoiceBase {
    static constexpr int MaxPartials = 28;
    // Two detuned "strings" per partial, each a decaying quadrature oscillator.
    float re[2][MaxPartials] {}, im[2][MaxPartials] {}, cr[2][MaxPartials] {}, ci[2][MaxPartials] {};
    int np = 0;
    float relEnv = 1.f, relCoef = 0.f, pan = 0.5f;
    float hammerEnv = 0.f, hammerDecay = 0.f;
    Biquad hammerBp;
    OnePole tone;
    Rng rng { 99u };
    bool active = false;
    int renorm = 0;

    bool isActive() const { return active; }
    void kill() { active = false; released = false; }
    void release() { released = true; }
    void start(int n, float v, float vg, float det, const PianoShared& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false; active = true;
        const float f0 = noteHz((float)n + det);
        const float B = (s.model == 1 ? 0.00025f : 0.00012f) * std::pow(2.f, ((float)n - 60.f) / 24.f);
        np = 0;
        const float brightness = clampf(s.bright + (v - 0.6f) * 0.5f + (s.model == 2 ? 0.25f : 0.f), 0.05f, 1.f);
        const float strike = 1.f / 7.f;  // hammer strike position (notches 7th partial)
        for (int k = 1; k <= MaxPartials; ++k) {
            const float fk = f0 * (float)k * std::sqrt(1.f + B * (float)(k * k));
            if (fk > sr * 0.45f) break;
            float amp = std::fabs(std::sin(kPi * (float)k * strike)) / std::pow((float)k, 1.9f - 1.2f * brightness);
            amp *= 1.f / (1.f + std::pow(fk / (2500.f + 6000.f * brightness), 2.f));
            // frequency-dependent decay; low notes ring longer
            const float regist = std::pow(2.f, -((float)n - 48.f) / 30.f);
            const float t60 = std::max(0.08f, s.decay * regist / (1.f + 0.18f * (float)(k - 1)));
            for (int st = 0; st < 2; ++st) {
                const float ct = (st == 0 ? -0.5f : 0.5f) * s.detuneCt;
                const float w = kTwoPi * fk * std::pow(2.f, ct / 1200.f) / sr;
                const float r = std::pow(0.001f, 1.f / (t60 * (st == 0 ? 1.f : 0.45f) * sr));  // string 2 decays faster -> two-stage decay
                cr[st][np] = r * std::cos(w);
                ci[st][np] = r * std::sin(w);
                re[st][np] = 0.f;
                im[st][np] = amp * (st == 0 ? 0.55f : 0.45f);
            }
            ++np;
        }
        relEnv = 1.f;
        relCoef = 1.f - std::exp(-4.6f / (std::max(0.02f, s.release) * sr));
        pan = 0.5f + clampf(((float)n - 64.f) / 48.f, -1.f, 1.f) * 0.5f * s.width;
        hammerEnv = s.hammer * (0.3f + v);
        hammerDecay = std::exp(-1.f / (0.006f * sr));
        hammerBp.set(Biquad::BP, std::min(sr * 0.4f, f0 * 6.f + 1500.f), 0.8f, 0.f, sr);
        tone.setHz(3000.f + 14000.f * brightness * v, sr);
        renorm = 0;
    }
    void render(float* L, float* R, int n, const PianoShared& s, float pitchOff, float ampMod) {
        (void)pitchOff;  // modal piano ignores bend by design (acoustic instrument)
        const float g = velGain * ampMod * 0.5f;
        float energy = 0.f;
        for (int i = 0; i < n; ++i) {
            float y = 0.f;
            for (int st = 0; st < 2; ++st)
                for (int k = 0; k < np; ++k) {
                    const float a = re[st][k], b = im[st][k];
                    re[st][k] = a * cr[st][k] - b * ci[st][k];
                    im[st][k] = a * ci[st][k] + b * cr[st][k];
                    y += im[st][k];
                }
            if (hammerEnv > 1.0e-4f) { y += hammerBp.tick(rng.bi()) * hammerEnv; hammerEnv *= hammerDecay; }
            if (released) relEnv -= relEnv * relCoef;
            y = tone.lp(y) * relEnv * g;
            energy += std::fabs(y);
            L[i] += y * (1.f - pan) * 1.4f; R[i] += y * pan * 1.4f;
        }
        if (relEnv < 1.0e-4f || (energy < 1.0e-6f * (float)n && hammerEnv < 1.0e-4f)) active = false;
        (void)s;
    }
};

class PianoEngine : public PolyEngine<PianoVoice, PianoShared, 24> {
public:
    bool followsPitchOffset() const override { return false; }
    using PolyEngine::PolyEngine;
    void onPrepare() override {
        p.bind(params.get(), { "model", "bright", "decay", "hammer", "detune", "a_r", "width", "bend", "transpose", "vel_sens", "level" });
    }
    void updateShared(PianoShared& s) override {
        s.model = (int)p[0]; s.bright = p[1] * 0.01f; s.decay = p[2]; s.hammer = p[3] * 0.01f; s.detuneCt = p[4];
        s.release = p[5]; s.width = p[6] * 0.01f; s.c.bendRange = p[7]; s.c.transpose = p[8]; s.c.velSens = p[9] * 0.01f; s.c.level = db2lin(p[10]);
        s.c.lfoPitchCt = s.c.lfoAmp = s.c.lfoCutoff = 0.f; s.c.mono = false; s.c.glideMs = 0.f;
    }
private:
    P p;
};

// ================================================================ ELECTRIC PIANO (tine)
struct EPShared {
    Common c;
    float lfo = 0.f;
    float tine = 0.45f, bark = 0.3f, decay = 4.f, release = 0.2f, tremRate = 4.5f, tremDepth = 0.25f, tone = 6000.f, width = 0.7f;
};

struct EPVoice : VoiceBase {
    float ph0 = 0, ph1 = 0, ph2 = 0;
    float env0 = 0, env1 = 0, env2 = 0, k0 = 0, k1 = 0, k2 = 0;
    float relEnv = 1.f, relCoef = 0.f;
    bool active = false;
    bool isActive() const { return active; }
    void kill() { active = false; released = false; }
    void release() { released = true; }
    void start(int n, float v, float vg, float det, const EPShared& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false; active = true;
        ph0 = ph1 = ph2 = 0.f;
        const float regist = std::pow(2.f, -((float)n - 60.f) / 36.f);
        env0 = 1.f; env1 = s.tine * (0.4f + v); env2 = 0.35f * v;
        k0 = std::exp(-4.6f / (s.decay * regist * sr));
        k1 = std::exp(-4.6f / (0.25f * sr));
        k2 = std::exp(-4.6f / (s.decay * 0.4f * regist * sr));
        relEnv = 1.f;
        relCoef = 1.f - std::exp(-4.6f / (std::max(0.02f, s.release) * sr));
    }
    void render(float* L, float* R, int n, const EPShared& s, float pitchOff, float ampMod) {
        const float f = noteHz(stepPitch() + pitchOff);
        const float d0 = f / sr, d1 = std::min(f * 7.11f, sr * 0.45f) / sr, d2 = f * 2.f / sr;
        const auto& sn = sine();
        const float drive = 1.f + s.bark * 6.f * vel;
        const float g = velGain * ampMod * 0.35f;
        for (int i = 0; i < n; ++i) {
            float y = sn(ph0) * env0 + sn(ph1) * env1 * 0.5f + sn(ph2) * env2;
            ph0 += d0; ph1 += d1; ph2 += d2;
            if (ph0 > 1.f) ph0 -= 1.f;
            if (ph1 > 1.f) ph1 -= 1.f;
            if (ph2 > 1.f) ph2 -= 1.f;
            env0 *= k0; env1 *= k1; env2 *= k2;
            // asymmetric pickup nonlinearity = "bark"
            y = fastTanh(y * drive + 0.15f * y * y * s.bark) / std::sqrt(drive);
            if (released) relEnv -= relEnv * relCoef;
            y *= relEnv * g;
            L[i] += y; R[i] += y;
        }
        if (relEnv < 1.0e-4f || env0 < 1.0e-4f) active = false;
    }
};

class EPEngine : public PolyEngine<EPVoice, EPShared> {
public:
    using PolyEngine::PolyEngine;
    void onPrepare() override {
        p.bind(params.get(), { "tine", "bark", "decay", "a_r", "trem_rate", "trem_depth", "tone", "width", "bend", "transpose", "vel_sens", "level" });
        tremPh = 0.f;
    }
    void updateShared(EPShared& s) override {
        s.tine = p[0] * 0.01f; s.bark = p[1] * 0.01f; s.decay = p[2]; s.release = p[3]; s.tremRate = p[4]; s.tremDepth = p[5] * 0.01f;
        s.tone = p[6]; s.width = p[7] * 0.01f; s.c.bendRange = p[8]; s.c.transpose = p[9]; s.c.velSens = p[10] * 0.01f; s.c.level = db2lin(p[11]);
    }
    void postProcess(float* L, float* R, int n) override {
        lp[0].setHz(shared.tone, sr); lp[1].setHz(shared.tone, sr);
        const auto& sn = sine();
        for (int i = 0; i < n; ++i) {
            const float m = sn(tremPh) * shared.tremDepth;
            tremPh += shared.tremRate / sr; if (tremPh > 1.f) tremPh -= 1.f;
            const float x = 0.5f * (L[i] + R[i]);
            const float gl = 1.f - shared.width * 0.5f * (1.f + m) * 0.8f + (1.f - shared.width) * 0.f;
            const float gr = 1.f - shared.width * 0.5f * (1.f - m) * 0.8f;
            L[i] = lp[0].lp(x * gl * (1.f - (1.f - shared.width) * m * 0.5f));
            R[i] = lp[1].lp(x * gr * (1.f + (1.f - shared.width) * m * 0.5f));
        }
    }
private:
    P p;
    float tremPh = 0.f;
    OnePole lp[2];
};

// ================================================================ ORGAN
struct OrganShared {
    Common c;
    float lfo = 0.f;
    float bars[9] {};
    bool perc = true;
    int percHarm = 1;
    float percDecay = 0.4f, click = 0.3f, release = 0.03f, drive = 0.15f;
    int rotary = 1;
};

struct OrganVoice : VoiceBase {
    static constexpr float ratios[9] = { 0.5f, 1.5f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 8.f };
    float ph[9] {}, percPh = 0.f, percEnv = 0.f, percK = 0.f, clickEnv = 0.f;
    ADSR amp;
    Rng rng { 5150u };
    OnePole clickLp;
    bool isActive() const { return amp.active(); }
    void kill() { amp.kill(); released = false; }
    void release() { amp.noteOff(); released = true; clickEnv = 0.3f * clickEnv + 0.15f; }
    void start(int n, float v, float vg, float det, const OrganShared& s) {
        note = n; vel = v; velGain = 1.f; detune = det; released = false;
        (void)vg;
        amp.set(0.004f, 0.01f, 1.f, s.release, sr);
        amp.noteOn();
        percEnv = s.perc ? 1.f : 0.f;
        percK = std::exp(-4.6f / (s.percDecay * sr));
        clickEnv = s.click;
        clickLp.setHz(3500.f, sr);
    }
    void render(float* L, float* R, int n, const OrganShared& s, float pitchOff, float ampMod) {
        const float f = noteHz(stepPitch() + pitchOff);
        float inc[9];
        for (int k = 0; k < 9; ++k) inc[k] = std::min(f * ratios[k], sr * 0.45f) / sr;
        const float pInc = f * (s.percHarm == 0 ? 2.f : 3.f) / sr;
        const auto& sn = sine();
        float norm = 0.f;
        for (float b : s.bars) norm += b;
        norm = 1.f / std::max(3.f, norm);
        for (int i = 0; i < n; ++i) {
            float y = 0.f;
            for (int k = 0; k < 9; ++k) {
                if (s.bars[k] > 0.f) y += sn(ph[k]) * s.bars[k];
                ph[k] += inc[k]; if (ph[k] > 1.f) ph[k] -= 1.f;
            }
            y *= norm;
            if (percEnv > 1.0e-4f) { y += sn(percPh) * percEnv * 0.5f; percPh += pInc; if (percPh > 1.f) percPh -= 1.f; percEnv *= percK; }
            if (clickEnv > 1.0e-4f) { y += clickLp.lp(rng.bi()) * clickEnv * 0.6f; clickEnv *= 0.995f; }
            y *= amp.tick() * ampMod * 0.3f;
            L[i] += y; R[i] += y;
        }
    }
};
constexpr float OrganVoice::ratios[9];

class OrganEngine : public PolyEngine<OrganVoice, OrganShared, 20> {
public:
    using PolyEngine::PolyEngine;
    void onPrepare() override {
        p.bind(params.get(), { "db16", "db513", "db8", "db4", "db223", "db2", "db135", "db113", "db1",
                               "perc", "perc_harm", "perc_decay", "click", "rotary", "drive", "a_r", "transpose", "level" });
        for (auto& d : dl) d.allocate((int)(sr * 0.02f));
        xo.setHz(800.f, sr);
    }
    void updateShared(OrganShared& s) override {
        for (int k = 0; k < 9; ++k) s.bars[k] = p[k] / 8.f;
        s.perc = p[9] >= 0.5f; s.percHarm = (int)p[10]; s.percDecay = p[11]; s.click = p[12] * 0.01f;
        s.rotary = (int)p[13]; s.drive = p[14] * 0.01f; s.release = p[15]; s.c.transpose = p[16]; s.c.level = db2lin(p[17]);
        s.c.velSens = 0.f; s.c.mono = false; s.c.glideMs = 0.f;
    }
    void postProcess(float* L, float* R, int n) override {
        // rotary: horn (highs) and drum (lows) rotate at different speeds; speeds ramp like a real motor
        const float tgtHorn = shared.rotary == 2 ? 6.7f : (shared.rotary == 1 ? 0.8f : 0.f);
        const float tgtDrum = shared.rotary == 2 ? 5.8f : (shared.rotary == 1 ? 0.65f : 0.f);
        const float drive = 1.f + shared.drive * 5.f;
        const auto& sn = sine();
        for (int i = 0; i < n; ++i) {
            hornSpd += (tgtHorn - hornSpd) * 0.00003f * 48000.f / sr;
            drumSpd += (tgtDrum - drumSpd) * 0.00001f * 48000.f / sr;
            hornPh += hornSpd / sr; if (hornPh > 1.f) hornPh -= 1.f;
            drumPh += drumSpd / sr; if (drumPh > 1.f) drumPh -= 1.f;
            const float x = fastTanh(0.5f * (L[i] + R[i]) * drive) / std::sqrt(drive);
            const float lo = xo.lp(x), hi = x - lo;
            if (shared.rotary == 0 && hornSpd < 0.01f) { L[i] = R[i] = x; continue; }
            const float hs = sn(hornPh), hc = sn(hornPh + 0.25f), ds = sn(drumPh);
            dl[0].push(hi); dl[1].push(lo);
            const float maxD = sr * 0.0012f;
            const float hL = dl[0].read(maxD * (1.f + hs)) * (1.f + 0.35f * hc);
            const float hR = dl[0].read(maxD * (1.f - hs)) * (1.f - 0.35f * hc);
            const float dL = dl[1].read(maxD * 2.f * (1.f + ds)) * (1.f + 0.15f * ds);
            const float dR = dl[1].read(maxD * 2.f * (1.f - ds)) * (1.f - 0.15f * ds);
            L[i] = hL + dL; R[i] = hR + dR;
        }
    }
private:
    P p;
    DelayLine dl[2];
    OnePole xo;
    float hornPh = 0, drumPh = 0, hornSpd = 0.8f, drumSpd = 0.65f;
};

// ================================================================ PLUCKED STRING (Karplus-Strong)
struct StringShared {
    Common c;
    float lfo = 0.f;
    int model = 0;
    float pluck = 0.3f, bright = 0.6f, decay = 3.5f, body = 0.5f, drive = 0.f, tone = 7000.f, release = 0.15f, width = 0.4f;
};

struct StringVoice : VoiceBase {
    DelayLine line;
    float lpState = 0.f, loopGain = 0.99f, damp = 0.5f, period = 100.f;
    float relEnv = 1.f, relCoef = 0.f, pan = 0.5f, dcX = 0.f, dcY = 0.f;
    float apState = 0.f, apCoef = 0.f;
    int excite = 0;
    float exBuf[4096] {};
    int exLen = 0;
    Rng rng { 31337u };
    bool active = false;
    float energy = 1.f;

    void prepare(float s) { VoiceBase::prepare(s); line.allocate((int)(s / 20.f) + 8); }
    bool isActive() const { return active; }
    void kill() { active = false; released = false; }
    void release() { released = true; }
    void start(int n, float v, float vg, float det, const StringShared& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false; active = true;
        line.clear();
        lpState = 0.f; dcX = dcY = 0.f; apState = 0.f;
        const float f = noteHz((float)n + det);
        setPitch(f, s);
        // excitation: noise burst, darker for soft plucks, comb-filtered by pluck position
        exLen = std::min(4095, std::max(4, (int)period));
        const float bright = clampf(s.bright * (0.5f + v * 0.7f), 0.05f, 1.f);
        OnePole shape; shape.setHz(400.f + 9000.f * bright * bright, sr);
        const int pp = std::max(1, (int)(exLen * clampf(s.pluck, 0.02f, 0.5f)));
        float tmp[4096];
        for (int i = 0; i < exLen; ++i) tmp[i] = shape.lp(rng.bi());
        for (int i = 0; i < exLen; ++i) exBuf[i] = tmp[i] - (i >= pp ? tmp[i - pp] : 0.f);
        excite = 0;
        relEnv = 1.f;
        relCoef = 1.f - std::exp(-4.6f / (std::max(0.01f, s.release) * sr));
        pan = 0.5f + clampf(((float)n - 52.f) / 30.f, -1.f, 1.f) * 0.5f * s.width;
        energy = 1.f;
    }
    void setPitch(float f, const StringShared& s) {
        f = clampf(f, 20.f, sr * 0.25f);
        damp = clampf(0.05f + 0.6f * (1.f - s.bright), 0.02f, 0.9f);     // loop low-pass amount
        // loop delay compensation: one-pole LP phase delay ~ damp/(1-damp) samples at low freq
        const float lpDelay = damp / (1.f - damp);
        period = sr / f;
        const float d = period - lpDelay;
        const int di = (int)d;
        const float frac = d - (float)di;
        apCoef = (1.f - frac) / (1.f + frac);                            // allpass fractional delay
        intDelay = std::max(2, di);
        loopGain = std::pow(0.001f, 1.f / (std::max(0.05f, s.decay) * f));
    }
    int intDelay = 100;
    void render(float* L, float* R, int n, const StringShared& s, float pitchOff, float ampMod) {
        const float target = stepPitch() + pitchOff;
        if (std::fabs(target - (float)note - detune) > 0.001f || pitchOff != 0.f) setPitch(noteHz(target), s);
        const float relGain = released ? std::pow(0.001f, 1.f / (std::max(0.01f, s.release) * noteHz(target))) : 1.f;
        const float g = velGain * ampMod * 0.6f;
        float e = 0.f;
        for (int i = 0; i < n; ++i) {
            float in = excite < exLen ? exBuf[excite++] * (0.6f + 0.6f * vel) : 0.f;
            const float delayed = line.tap(intDelay - 1);
            // allpass fractional delay
            const float ap = apCoef * delayed + apState;
            apState = delayed - apCoef * ap;
            lpState += (1.f - damp) * (ap - lpState);
            const float fb = lpState * loopGain * (released ? relGain : 1.f);
            const float y = fb + in;
            line.push(y);
            // DC blocker
            const float out = y - dcX + 0.995f * dcY;
            dcX = y; dcY = out;
            if (released) relEnv -= relEnv * relCoef * 0.2f;
            const float o = out * g * relEnv;
            e += std::fabs(o);
            L[i] += o * (1.f - pan) * 1.4f; R[i] += o * pan * 1.4f;
        }
        energy = 0.99f * energy + e / (float)std::max(1, n);
        if (excite >= exLen && (energy < 1.0e-5f || relEnv < 1.0e-4f)) active = false;
    }
};

class StringEngine : public PolyEngine<StringVoice, StringShared, 12> {
public:
    using PolyEngine::PolyEngine;
    void onPrepare() override {
        p.bind(params.get(), { "model", "pluck", "bright", "decay", "body", "drive", "tone", "a_r", "width",
                               "glide", "mono", "legato", "bend", "transpose", "vel_sens", "level" });
        for (int c = 0; c < 2; ++c) { b1[c].reset(); b2[c].reset(); b3[c].reset(); tlp[c].reset(); cab[c].reset(); }
        lastModel = -1;
    }
    void updateShared(StringShared& s) override {
        s.model = (int)p[0]; s.pluck = p[1] * 0.01f; s.bright = p[2] * 0.01f; s.decay = p[3]; s.body = p[4] * 0.01f;
        s.drive = p[5] * 0.01f; s.tone = p[6]; s.release = p[7]; s.width = p[8] * 0.01f;
        if (s.model == 2) s.bright *= 0.6f;
        s.c.glideMs = p[9]; s.c.mono = p[10] >= 0.5f; s.c.legato = p[11] >= 0.5f; s.c.bendRange = p[12]; s.c.transpose = p[13];
        s.c.velSens = p[14] * 0.01f; s.c.level = db2lin(p[15]);
    }
    void postProcess(float* L, float* R, int n) override {
        const auto& s = shared;
        if (s.model != lastModel || s.body != lastBody || s.tone != lastTone) {
            // body resonances: acoustic/nylon = wooden box, electric = pickup peak, bass = low thump
            const float f1 = s.model == 2 ? 70.f : (s.model == 1 ? 2200.f : 110.f);
            const float f2 = s.model == 2 ? 180.f : (s.model == 1 ? 900.f : 230.f);
            for (int c = 0; c < 2; ++c) {
                b1[c].set(Biquad::Peak, f1, 1.5f, s.body * 9.f, sr);
                b2[c].set(Biquad::Peak, f2, 1.2f, s.body * 6.f, sr);
                b3[c].set(Biquad::Peak, s.model == 3 ? 2800.f : 3500.f, 0.7f, s.model == 1 ? 0.f : -s.body * 3.f, sr);
                tlp[c].set(Biquad::LP, s.tone, 0.7f, 0.f, sr);
                cab[c].set(Biquad::LP, 4800.f, 0.9f, 0.f, sr);
            }
            lastModel = s.model; lastBody = s.body; lastTone = s.tone;
        }
        const float drive = 1.f + s.drive * 30.f;
        for (int i = 0; i < n; ++i) {
            float x[2] = { L[i], R[i] };
            for (int c = 0; c < 2; ++c) {
                float y = b2[c].tick(b1[c].tick(x[c]));
                y = b3[c].tick(y);
                if (s.drive > 0.001f) y = cab[c].tick(fastTanh(y * drive) / std::sqrt(drive) * 1.6f);
                x[c] = tlp[c].tick(y);
            }
            L[i] = x[0]; R[i] = x[1];
        }
    }
private:
    P p;
    Biquad b1[2], b2[2], b3[2], tlp[2], cab[2];
    int lastModel = -1;
    float lastBody = -1.f, lastTone = -1.f;
};

// ================================================================ ENSEMBLE (strings / brass / choir)
struct EnsembleShared {
    Common c;
    float lfo = 0.f;
    int model = 0;
    float detune = 0.4f, bright = 0.5f, vowel = 0.f, breath = 0.1f, vibRate = 5.f, vibDepth = 15.f, vibDelay = 0.4f, width = 0.8f;
};

struct EnsembleVoice : VoiceBase {
    Osc osc[4];
    SVF lp[2], f1[2], f2[2], f3[2];
    ADSR amp;
    Rng rng { 2024u };
    float t = 0.f, vibPh = 0.f, blat = 0.f;
    int counter = 0;
    bool isActive() const { return amp.active(); }
    void kill() { amp.kill(); released = false; }
    void release() { amp.noteOff(); released = true; }
    void start(int n, float v, float vg, float det, const EnsembleShared& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false;
        if (!amp.active()) { for (auto& o : osc) o.phase = rng.uni(); for (int c = 0; c < 2; ++c) { lp[c].reset(); f1[c].reset(); f2[c].reset(); f3[c].reset(); } }
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        amp.noteOn();
        t = 0.f; blat = 1.f; counter = 0;
        vibPh = rng.uni();
    }
    void render(float* L, float* R, int n, const EnsembleShared& s, float pitchOff, float ampMod) {
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        static const float F[5][3] = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 350, 1900, 2800 }, { 450, 800, 2830 }, { 325, 700, 2530 } };
        const float vx = clampf(s.vowel, 0.f, 4.f); const int va = std::min(3, (int)vx); const float vf = vx - (float)va;
        float base = stepPitch() + pitchOff;
        const float dt = (float)n / sr;
        t += dt;
        const float vibAmt = clampf((t - s.vibDelay) / 0.5f, 0.f, 1.f) * s.vibDepth / 100.f;
        vibPh += s.vibRate * dt; if (vibPh > 1.f) vibPh -= 1.f;
        base += sine()(vibPh) * vibAmt;
        if (s.model == 1) { base -= 0.3f * blat; blat *= std::exp(-dt / 0.04f); }   // brass pitch scoop
        const float spread = s.detune * 0.25f;
        float inc[4];
        const float off[4] = { -1.f, -0.33f, 0.33f, 1.f };
        for (int k = 0; k < 4; ++k) inc[k] = noteHz(base + off[k] * spread) / sr;
        // filter
        float cutoff;
        if (s.model == 1) cutoff = 300.f + (1000.f + 6000.f * s.bright) * vel * clampf(t / std::max(0.02f, s.c.ampA * 1.5f), 0.f, 1.f);
        else cutoff = 600.f + 9000.f * s.bright * (0.5f + 0.5f * vel);
        for (int c = 0; c < 2; ++c) {
            lp[c].set(std::min(cutoff * std::pow(2.f, (base - 60.f) / 24.f), sr * 0.45f), 0.8f, sr);
            f1[c].set(lerp(F[va][0], F[va + 1][0], vf), 6.f, sr);
            f2[c].set(lerp(F[va][1], F[va + 1][1], vf), 8.f, sr);
            f3[c].set(lerp(F[va][2], F[va + 1][2], vf), 10.f, sr);
        }
        const float g = velGain * ampMod * 0.3f;
        for (int i = 0; i < n; ++i) {
            float l = 0.f, r = 0.f;
            for (int k = 0; k < 4; ++k) {
                const float x = s.model == 2 ? osc[k].tick(kPulse, inc[k], 0.3f) : osc[k].tick(kSaw, inc[k]);
                const float pk = 0.5f + off[k] * 0.5f * s.width;
                l += x * (1.f - pk); r += x * pk;
            }
            const float br = rng.bi() * s.breath;
            l = l * 0.6f + br; r = r * 0.6f + br;
            float yl, yr;
            if (s.model == 2) {
                f1[0].tick(l); f2[0].tick(l); f3[0].tick(l);
                f1[1].tick(r); f2[1].tick(r); f3[1].tick(r);
                yl = f1[0].bp + f2[0].bp * 0.6f + f3[0].bp * 0.3f;
                yr = f1[1].bp + f2[1].bp * 0.6f + f3[1].bp * 0.3f;
                yl *= 1.5f; yr *= 1.5f;
            } else {
                lp[0].tick(l); lp[1].tick(r);
                yl = lp[0].lp; yr = lp[1].lp;
            }
            const float a = amp.tick() * g;
            L[i] += yl * a; R[i] += yr * a;
        }
    }
};

class EnsembleEngine : public PolyEngine<EnsembleVoice, EnsembleShared> {
public:
    using PolyEngine::PolyEngine;
    void onPrepare() override {
        p.bind(params.get(), { "model", "detune", "bright", "vowel", "breath", "vib_rate", "vib_depth", "vib_delay",
                               "a_a", "a_d", "a_s", "a_r", "width", "glide", "mono", "legato", "bend", "transpose", "vel_sens", "level" });
    }
    void updateShared(EnsembleShared& s) override {
        s.model = (int)p[0]; s.detune = p[1] * 0.01f; s.bright = p[2] * 0.01f; s.vowel = p[3]; s.breath = p[4] * 0.01f;
        s.vibRate = p[5]; s.vibDepth = p[6]; s.vibDelay = p[7];
        s.c.ampA = p[8]; s.c.ampD = p[9]; s.c.ampS = p[10] * 0.01f; s.c.ampR = p[11]; s.width = p[12] * 0.01f;
        s.c.glideMs = p[13]; s.c.mono = p[14] >= 0.5f; s.c.legato = p[15] >= 0.5f; s.c.bendRange = p[16]; s.c.transpose = p[17];
        s.c.velSens = p[18] * 0.01f; s.c.level = db2lin(p[19]);
    }
private:
    P p;
};

// ================================================================ SAMPLER
struct SamplerShared {
    Common c;
    float lfo = 0.f;
    float tune = 0.f, start = 0.f, cutoff = 20000.f, reso = 0.f;
    bool loop = false, reverse = false, keytrack = true;
    const SampleData* sample = nullptr;
    int root = 60;
};

struct SamplerVoice : VoiceBase {
    double pos = 0.0;
    ADSR amp;
    SVF fl[2];
    bool isActive() const { return amp.active(); }
    void kill() { amp.kill(); released = false; }
    void release() { amp.noteOff(); released = true; }
    void start(int n, float v, float vg, float det, const SamplerShared& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false;
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        amp.noteOn(true);
        fl[0].reset(); fl[1].reset();
        if (s.sample) {
            const double len = s.sample->frames();
            pos = s.reverse ? len - 1.0 - s.start * len : s.start * len;
        }
    }
    void render(float* L, float* R, int n, const SamplerShared& s, float pitchOff, float ampMod) {
        if (!s.sample || s.sample->frames() < 2) { amp.kill(); return; }
        amp.set(s.c.ampA, s.c.ampD, s.c.ampS, s.c.ampR, sr);
        const float semis = (s.keytrack ? (stepPitch() - (float)s.root) : detune) + s.tune + pitchOff;
        const double rate = std::pow(2.0, semis / 12.0) * s.sample->sampleRate / sr * (s.reverse ? -1.0 : 1.0);
        const double len = s.sample->frames();
        const double loopStart = s.start * len;
        const bool filt = s.cutoff < 19000.f || s.reso > 0.01f;
        if (filt) for (auto& f : fl) f.set(s.cutoff, 0.6f + s.reso * 10.f, sr);
        const float g = velGain * ampMod * 0.8f;
        for (int i = 0; i < n; ++i) {
            if (pos >= len || pos < 0.0) {
                if (s.loop) pos = s.reverse ? len - 1.0 : loopStart;
                else { amp.kill(); return; }
            }
            float l, r;
            s.sample->read(pos, l, r);
            pos += rate;
            if (filt) { fl[0].tick(l); fl[1].tick(r); l = fl[0].lp; r = fl[1].lp; }
            const float a = amp.tick() * g;
            L[i] += l * a; R[i] += r * a;
        }
    }
};

class SamplerEngine : public PolyEngine<SamplerVoice, SamplerShared> {
public:
    SamplerEngine(ParamSetPtr ps, SamplePtr smp, int root) : PolyEngine(std::move(ps)), sample(std::move(smp)), rootNote(root) {}
    void onPrepare() override {
        p.bind(params.get(), { "tune", "fine", "start", "loop", "reverse", "keytrack", "cutoff", "reso", "a_a", "a_d", "a_s", "a_r",
                               "glide", "mono", "legato", "bend", "transpose", "vel_sens", "level" });
    }
    void updateShared(SamplerShared& s) override {
        s.tune = p[0] + p[1] * 0.01f; s.start = p[2] * 0.01f; s.loop = p[3] >= 0.5f; s.reverse = p[4] >= 0.5f; s.keytrack = p[5] >= 0.5f;
        s.cutoff = p[6]; s.reso = p[7] * 0.01f;
        s.c.ampA = p[8]; s.c.ampD = p[9]; s.c.ampS = p[10] * 0.01f; s.c.ampR = p[11];
        s.c.glideMs = p[12]; s.c.mono = p[13] >= 0.5f; s.c.legato = p[14] >= 0.5f; s.c.bendRange = p[15]; s.c.transpose = p[16];
        s.c.velSens = p[17] * 0.01f; s.c.level = db2lin(p[18]);
        s.sample = sample.get(); s.root = rootNote;
    }
private:
    P p;
    SamplePtr sample;
    int rootNote;
};

#include "StudioSynth.inl"

// ================================================================ DRUM KIT
class DrumKit : public Instrument {
public:
    DrumKit(ParamSetPtr kit, const std::array<SamplePtr, 16>& samples, const std::array<ParamSetPtr, 16>& pads)
        : kitParams(std::move(kit)), padSamples(samples), padParams(pads) {}

    void prepare(double sampleRate, int) override {
        sr = (float)sampleRate;
        k.bind(kitParams.get(), { "level", "vel_sens", "tune", "humanize" });
        for (int i = 0; i < 16; ++i)
            if (padParams[(size_t)i]) pk[(size_t)i].bind(padParams[(size_t)i].get(), { "tune", "gain", "pan", "decay", "cutoff", "choke", "reverse", "mute",
                                                                                     "start", "attack", "pitch_env", "pitch_dec", "hp", "reso", "drive" });
        for (auto& v : voices) v.active = false;
    }
    void noteOn(int note, float vel, float detune) override {
        const int pad = note - kDrumBaseNote;
        if (!juce::isPositiveAndBelow(pad, 16)) return;
        const auto& smp = padSamples[(size_t)pad];
        if (!smp || !padParams[(size_t)pad]) return;
        const auto& p = pk[(size_t)pad];
        if (p[7] >= 0.5f) return;  // muted pad
        const int choke = (int)p[5];
        for (auto& v : voices)
            if (v.active && choke > 0 && v.choke == choke && v.pad != pad) v.fadeOut = 1.f;   // e.g. closed hat cuts the open hat
        Voice* t = nullptr;
        for (auto& v : voices) if (!v.active) { t = &v; break; }
        if (!t) { uint32_t best = UINT32_MAX; for (auto& v : voices) if (v.age < best) { best = v.age; t = &v; } }
        const float velSens = k[1] * 0.01f;
        float vv = vel;
        if (k[3] > 0.f) vv = clampf(vel * (1.f + rng.bi() * k[3] * 0.003f), 0.f, 1.f);
        t->active = true;
        t->pad = pad;
        t->smp = smp.get();
        t->hold = smp;  // not touched on audio thread except via raw pointer; keeps lifetime with kit
        t->choke = choke;
        t->chokeSelf = choke > 0;
        t->reverse = p[6] >= 0.5f;
        const double startAt = smp->frames() * (double)clampf(p[8], 0.f, 90.f) * 0.01;
        t->pos = t->reverse ? smp->frames() - 1.0 - startAt : startAt;
        t->rate = std::pow(2.0, (p[0] + k[2] + detune) / 12.0) * smp->sampleRate / sr * (t->reverse ? -1.0 : 1.0);
        t->gain = db2lin(p[1]) * (1.f - velSens + velSens * vv);
        const float pan = p[2] * 0.01f;
        t->gl = std::cos((pan + 1.f) * kPi * 0.25f) * 1.4142f;
        t->gr = std::sin((pan + 1.f) * kPi * 0.25f) * 1.4142f;
        t->lengthSamples = p[3] >= 3.99f ? 1.0e12 : (double)p[3] * sr;
        t->played = 0.0;
        t->fadeOut = 0.f;
        t->env = 1.f;
        t->attackSamples = p[9] * 0.001f * sr;
        t->pitchEnv = p[10];
        t->pitchDecSamples = std::max(1.f, p[11] * 0.001f * sr);
        t->rateMul = 1.0;
        const float q = 0.707f + p[13] * p[13] * 0.0012f;
        t->lpOn = p[4] < 19500.f;
        t->hpOn = p[12] > 20.5f;
        for (int c = 0; c < 2; ++c) {
            t->lp[c].reset(); t->hp[c].reset();
            if (t->lpOn) t->lp[c].set(p[4], q, sr);
            if (t->hpOn) t->hp[c].set(p[12], q, sr);
        }
        t->drive = p[14] * 0.01f;
        t->age = ++age;
    }
    void noteOff(int) override {}
    void allNotesOff(bool hard) override { for (auto& v : voices) { if (hard) v.active = false; else if (v.active) v.fadeOut = 1.f; } }
    int activeVoices() const override { int n = 0; for (auto& v : voices) n += v.active ? 1 : 0; return n; }

    void render(float* L, float* R, int n) override {
        const float level = db2lin(k[0]);
        const float fadeK = std::exp(-1.f / (0.004f * sr));
        for (auto& v : voices) {
            if (!v.active) continue;
            const double len = v.smp->frames();
            const float g0 = 1.f + v.drive * 8.f, comp = 1.f / fastTanh(g0 * 0.7f);   // a 0.7 peak comes out at full scale
            for (int i = 0; i < n; ++i) {
                if (v.pos < 0.0 || v.pos >= len) { v.active = false; break; }
                if (v.pitchEnv != 0.f && ((int)v.played & 15) == 0)
                    v.rateMul = std::pow(2.0, v.pitchEnv * std::exp(-(float)v.played / v.pitchDecSamples) / 12.0);
                float l, r;
                v.smp->read(v.pos, l, r);
                v.pos += v.rate * v.rateMul;
                v.played += 1.0;
                if (v.played > v.lengthSamples) v.fadeOut = 1.f;
                if (v.fadeOut > 0.f) { v.env *= fadeK; if (v.env < 1.0e-4f) { v.active = false; break; } }
                if (v.hpOn) { v.hp[0].tick(l); v.hp[1].tick(r); l = v.hp[0].hp; r = v.hp[1].hp; }
                if (v.lpOn) { v.lp[0].tick(l); v.lp[1].tick(r); l = v.lp[0].lp; r = v.lp[1].lp; }
                if (v.drive > 0.001f) { l = fastTanh(l * g0) * comp; r = fastTanh(r * g0) * comp; }
                float g = v.gain * v.env * level;
                if (v.played < v.attackSamples) g *= (float)v.played / v.attackSamples;
                L[i] += l * g * v.gl; R[i] += r * g * v.gr;
            }
        }
    }
private:
    struct Voice {
        bool active = false, reverse = false, chokeSelf = false, lpOn = false, hpOn = false;
        int pad = 0, choke = 0;
        const SampleData* smp = nullptr;
        SamplePtr hold;
        double pos = 0, rate = 1, lengthSamples = 0, played = 0;
        float gain = 1, gl = 1, gr = 1, env = 1, fadeOut = 0;
        float attackSamples = 0, pitchEnv = 0, pitchDecSamples = 1, drive = 0;
        double rateMul = 1.0;
        SVF lp[2], hp[2];
        uint32_t age = 0;
    };
    ParamSetPtr kitParams;
    std::array<SamplePtr, 16> padSamples;
    std::array<ParamSetPtr, 16> padParams;
    std::array<P, 16> pk;
    P k;
    std::array<Voice, 32> voices;
    float sr = 48000.f;
    uint32_t age = 0;
    Rng rng { 1u };
};

}  // namespace

std::unique_ptr<Instrument> createInstrument(const InstrumentSetup& s) {
    const auto& e = s.engine;
    if (e == "fm") return std::make_unique<FMEngine>(s.params);
    if (e == "wavetable") return std::make_unique<WavetableEngine>(s.params);
    if (e == "piano") return std::make_unique<PianoEngine>(s.params);
    if (e == "epiano") return std::make_unique<EPEngine>(s.params);
    if (e == "organ") return std::make_unique<OrganEngine>(s.params);
    if (e == "string") return std::make_unique<StringEngine>(s.params);
    if (e == "ensemble") return std::make_unique<EnsembleEngine>(s.params);
    if (e == "sampler") return std::make_unique<SamplerEngine>(s.params, s.sample, s.sampleRoot);
    if (e == "drums") return std::make_unique<DrumKit>(s.params, s.padSamples, s.padParams);
    if (e == "studio") return std::make_unique<StudioEngine>(s.params);
    return std::make_unique<AnalogEngine>(s.params);
}
