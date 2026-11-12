// Studio Synth: the sound-design engine. Included by Instruments.cpp inside its anonymous
// namespace so it shares the band-limited wavetables, the voice manager and the Shape section.
//
// Per voice: 3 oscillators (sine / saw / square / triangle / pulse / noise / supersaw / any
// wavetable, up to 8 unison voices each), FM of osc 1 by osc 2, hard sync, ring mod, sub and
// coloured noise, two multimode filters (serial or parallel), 3 envelopes, 3 LFOs and an
// 8-slot modulation matrix. Modulation runs at control rate (every 16 samples).

namespace studio {

enum Src { SrcNone, SrcEnv2, SrcEnv3, SrcLfo1, SrcLfo2, SrcLfo3, SrcVel, SrcKey, SrcWheel, SrcBend, SrcRandom, SrcMac1, SrcMac2, SrcMac3, SrcMac4, SrcAmp, NumSrc };
enum Dst { DNone, DPitch, DOsc1Pitch, DOsc2Pitch, DOsc3Pitch, DOsc1Level, DOsc2Level, DOsc3Level, DOsc1Pos, DOsc2Pos, DOsc3Pos,
           DPw, DFm, DRing, DDetune, DSub, DNoise, DF1Cut, DF1Res, DF2Cut, DF2Res, DDrive, DPan, DVolume, DLfo1Rate, DLfo2Rate, DLfo3Rate, NumDst };
enum WaveT { WSine, WSaw, WSquare, WTri, WPulse, WNoise, WSuper, WTable };
enum FiltT { FLp12, FLp24, FHp12, FHp24, FBp, FNotch, FPeak, FComb, FFormant };
constexpr int kCtl = 16, kMaxUni = 8;

}  // namespace studio

struct StudioShared {
    Common c;
    float lfo = 0.f;   // unused (PolyEngine global LFO is off for this engine)
    struct Osc { bool on = false; int wave = 1, table = 0; float pos = 0, pw = 0.5f, pitch = 0, level = 0.8f, pan = 0; int uni = 1; float det = 0.25f, spread = 0.7f; bool freePhase = true; } osc[3];
    float fm = 0, ring = 0; bool sync = false;
    float subLevel = 0; int subOct = 0, subWave = 0; float noiseLevel = 0, noiseColor = 0;
    struct Filt { bool on = true; int type = 1; float cut = 8000, res = 0.15f, drive = 0, key = 0.5f, env = 0.2f, vel = 0; } flt[2];
    bool parallel = false;
    float env[3][4] {};
    struct Lfo { int shape = 0; float rate = 5; int sync = 0, mode = 0; float delay = 0, fade = 0; } lfos[3];
    struct Mod { int src = 0, dst = 0; float amt = 0; } mod[8];
    float macro[4] {};
    float bendRaw = 0, wheel = 0;
    float sharedLfo[3] {};
    int voices = 8;
    double bpm = 120.0;
};

struct StudioVoice : VoiceBase {
    using S = StudioShared;
    float ph[3][studio::kMaxUni] {};
    float subPh = 0.f;
    struct FiltState {
        SVF a[2], b[2], fmt[2][3];
        DelayLine comb[2];
        float combLp[2] {};
        // control-rate settings
        int type = 0; float hz = 1000, q = 0.7f, drive = 1, fb = 0, peak = 1; float fmtGain[3] { 1, 0.5f, 0.25f };
    } flt[2];
    ADSR amp, env2, env3;
    float lfoPh[3] {}, lfoHold[3] {}, lfoPrev[3] {};
    float age = 0.f, rnd = 0.f;
    Rng rng { 0xC0FFEEu };
    OnePole noiseLp, noiseHp;
    bool noiseDark = false, noiseBright = false;
    // control-rate cache
    struct OscRun { bool on = false; int uni = 1, wave = 0; const float* ta = nullptr; const float* tb = nullptr; float ff = 0, level = 0; float dt[studio::kMaxUni] {}, gl[studio::kMaxUni] {}, gr[studio::kMaxUni] {}; float norm = 1; } run[3];
    const float* subT = nullptr; float subDt = 0, subLevel = 0, noiseLevel = 0;
    float fm = 0, ring = 0, outL = 1, outR = 1;
    bool sync = false, parallel = false, f2on = false;

    void prepare(float s) {
        sr = s;
        for (auto& f : flt) for (auto& d : f.comb) d.allocate((int)(s / 20.f) + 8);
    }
    bool isActive() const { return amp.active(); }
    void kill() { amp.kill(); env2.kill(); env3.kill(); released = false; }
    void release() { amp.noteOff(); env2.noteOff(); env3.noteOff(); released = true; }
    void start(int n, float v, float vg, float det, const S& s) {
        note = n; vel = v; velGain = vg; detune = det; released = false;
        const bool fresh = !amp.active();
        amp.set(s.env[0][0], s.env[0][1], s.env[0][2], s.env[0][3], sr);
        env2.set(s.env[1][0], s.env[1][1], s.env[1][2], s.env[1][3], sr);
        env3.set(s.env[2][0], s.env[2][1], s.env[2][2], s.env[2][3], sr);
        amp.noteOn(); env2.noteOn(); env3.noteOn();
        if (fresh) {
            for (int o = 0; o < 3; ++o)
                for (int u = 0; u < studio::kMaxUni; ++u) ph[o][u] = s.osc[o].freePhase ? rng.uni() : 0.f;
            subPh = 0.f;
            for (auto& f : flt) {
                for (int c = 0; c < 2; ++c) { f.a[c].reset(); f.b[c].reset(); for (auto& x : f.fmt[c]) x.reset(); f.comb[c].clear(); f.combLp[c] = 0.f; }
            }
            noiseLp.reset(); noiseHp.reset();
        }
        for (int k = 0; k < 3; ++k) if (s.lfos[k].mode == 0) { lfoPh[k] = 0.f; lfoPrev[k] = lfoHold[k]; lfoHold[k] = rng.bi(); }
        age = 0.f;
        rnd = rng.bi();
    }

    float lfoValue(int k, const S& s) const {
        const auto& L = s.lfos[k];
        float v = L.mode == 1 ? s.sharedLfo[k] : shapeAt(L.shape, lfoPh[k], lfoHold[k], lfoPrev[k]);
        // delay + fade-in after the note starts
        if (age < L.delay) return 0.f;
        if (L.fade > 0.f) v *= std::min(1.f, (age - L.delay) / L.fade);
        return v;
    }
    static float shapeAt(int shape, float phase, float hold, float prev) {
        switch (shape) {
            case 0: return std::sin(phase * kTwoPi);
            case 1: return 1.f - 4.f * std::fabs(phase - 0.5f);
            case 2: return 2.f * phase - 1.f;
            case 3: return phase < 0.5f ? 1.f : -1.f;
            case 4: return hold;
            default: { const float t = phase * phase * (3.f - 2.f * phase); return prev + (hold - prev) * t; }   // smooth random
        }
    }
    void advanceLfos(const S& s, const float* rateMul, int len, double bpm) {
        for (int k = 0; k < 3; ++k) {
            const auto& L = s.lfos[k];
            if (L.mode == 1) continue;
            float rate = L.sync > 0 ? (float)(bpm / 60.0) / syncBeats(L.sync - 1) : L.rate;
            lfoPh[k] += rate * rateMul[k] * (float)len / sr;
            if (lfoPh[k] >= 1.f) { lfoPh[k] -= std::floor(lfoPh[k]); lfoPrev[k] = lfoHold[k]; lfoHold[k] = rng.bi(); }
        }
    }

    void setupFilter(FiltState& f, const S::Filt& p, float pitch, float e2, float cutMod, float resMod, float driveMod) {
        f.type = p.type;
        const float oct = p.env * e2 * 6.f + (pitch - 60.f) / 12.f * p.key + p.vel * (vel - 1.f) * 3.f + cutMod;
        f.hz = clampf(p.cut * std::pow(2.f, oct), 20.f, sr * 0.45f);
        const float res = clampf(p.res + resMod, 0.f, 1.f);
        f.drive = 1.f + clampf(p.drive + driveMod, 0.f, 1.f) * 4.f;
        f.q = 0.6f + res * res * 18.f;
        switch (f.type) {
            case studio::FLp24: case studio::FHp24:
                for (int c = 0; c < 2; ++c) { f.a[c].set(f.hz, 0.54f + res * res * 12.f, sr); f.b[c].set(f.hz, 0.54f, sr); }
                break;
            case studio::FComb:
                f.fb = res * 0.97f;
                break;
            case studio::FFormant: {
                static const float F[5][3] = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 350, 1700, 2700 }, { 450, 800, 2830 }, { 325, 700, 2530 } };
                const float vx = clampf(std::log2(f.hz / 20.f) / std::log2(1000.f), 0.f, 1.f) * 4.f;
                const int a = std::min(3, (int)vx); const float fr = vx - (float)a;
                const float q = 4.f + res * 16.f;
                for (int k = 0; k < 3; ++k) {
                    const float fc = lerp(F[a][k], F[a + 1][k], fr);
                    for (int c = 0; c < 2; ++c) f.fmt[c][k].set(fc, q * (1.f + 0.5f * k), sr);
                }
                break;
            }
            default:
                for (int c = 0; c < 2; ++c) f.a[c].set(f.hz, f.q, sr);
                f.peak = 1.f + res * 4.f;
                break;
        }
    }
    inline float runFilter(FiltState& f, int c, float x) {
        x *= f.drive;
        float y;
        switch (f.type) {
            case studio::FLp12: f.a[c].tick(x); y = f.a[c].lp; break;
            case studio::FLp24: f.a[c].tick(x); f.b[c].tick(f.a[c].lp); y = f.b[c].lp; break;
            case studio::FHp12: f.a[c].tick(x); y = f.a[c].hp; break;
            case studio::FHp24: f.a[c].tick(x); f.b[c].tick(f.a[c].hp); y = f.b[c].hp; break;
            case studio::FBp: f.a[c].tick(x); y = f.a[c].bp * f.a[c].k; break;
            case studio::FNotch: f.a[c].tick(x); y = f.a[c].lp + f.a[c].hp; break;
            case studio::FPeak: f.a[c].tick(x); y = x + f.a[c].bp * f.a[c].k * (f.peak - 1.f); break;
            case studio::FComb: {
                const float d = std::min(sr / f.hz, (float)f.comb[c].capacity() - 2.f);
                f.combLp[c] += 0.7f * (f.comb[c].read(d - 1.f) - f.combLp[c]);
                f.combLp[c] = flush(f.combLp[c]);
                y = x + f.combLp[c] * f.fb;
                f.comb[c].push(y);
                y *= 1.f - f.fb * 0.6f;
                break;
            }
            default: {
                y = 0.f;
                for (int k = 0; k < 3; ++k) { f.fmt[c][k].tick(x); y += f.fmt[c][k].bp * f.fmt[c][k].k * f.fmtGain[k]; }
                y *= 1.6f;
                break;
            }
        }
        return f.drive > 1.01f ? fastTanh(y) : y;
    }

    void control(const S& s, float basePitch, int len, double bpm) {
        using namespace studio;
        auto& wt = const_cast<Wavetables&>(wavetables());
        // ---- sources
        float src[NumSrc] {};
        src[SrcEnv2] = env2.v; src[SrcEnv3] = env3.v;
        src[SrcVel] = vel; src[SrcKey] = ((float)note - 60.f) / 60.f;
        src[SrcWheel] = s.wheel; src[SrcBend] = s.bendRaw; src[SrcRandom] = rnd;
        for (int k = 0; k < 4; ++k) src[SrcMac1 + k] = s.macro[k];
        src[SrcAmp] = amp.v;
        for (int k = 0; k < 3; ++k) src[SrcLfo1 + k] = lfoValue(k, s);
        // ---- matrix
        float d[NumDst] {};
        for (auto& m : s.mod) if (m.src > 0 && m.dst > 0 && m.amt != 0.f) d[juce::jlimit(0, NumDst - 1, m.dst)] += src[juce::jlimit(0, NumSrc - 1, m.src)] * m.amt;
        const float rateMul[3] = { std::pow(2.f, d[DLfo1Rate] * 3.f), std::pow(2.f, d[DLfo2Rate] * 3.f), std::pow(2.f, d[DLfo3Rate] * 3.f) };
        advanceLfos(s, rateMul, len, bpm);
        age += (float)len / sr;

        const float pitch = basePitch + d[DPitch] * 24.f;
        fm = clampf(s.fm + d[DFm], 0.f, 1.f);
        ring = clampf(s.ring + d[DRing], 0.f, 1.f);
        sync = s.sync;
        const float detMod = d[DDetune];
        for (int o = 0; o < 3; ++o) {
            const auto& P = s.osc[o];
            auto& R = run[o];
            R.on = P.on;
            if (!R.on) continue;
            R.wave = P.wave;
            R.level = std::max(0.f, P.level + d[DOsc1Level + o]);
            const float op = pitch + P.pitch + d[DOsc1Pitch + o] * 24.f;
            int uni = P.wave == WSuper ? std::max(P.uni, 7) : P.uni;
            uni = juce::jlimit(1, kMaxUni, uni);
            R.uni = uni;
            const float det = clampf(P.det + detMod, 0.f, 1.f) * (P.wave == WSuper ? 1.f : 0.5f);
            float maxHz = 1.f;
            for (int u = 0; u < uni; ++u) {
                const float t = uni == 1 ? 0.f : (float)u / (float)(uni - 1) * 2.f - 1.f;
                const float hz = noteHz(op + t * det);
                maxHz = std::max(maxHz, hz);
                R.dt[u] = hz / sr;
                const float pan = clampf(P.pan + t * P.spread, -1.f, 1.f);
                R.gl[u] = std::cos((pan + 1.f) * kPi * 0.25f);
                R.gr[u] = std::sin((pan + 1.f) * kPi * 0.25f);
            }
            R.norm = 1.f / std::sqrt((float)uni);
            if (P.wave == WNoise) continue;
            const float allowed = (sr * 0.5f) / maxHz;
            int mip = 0;
            while (mip < Wavetables::Mips - 1 && (float)((Wavetables::Size / 2) >> mip) > allowed) ++mip;
            int table = 0; float frame = 0.f;
            switch (P.wave) {
                case WSine: frame = 0.f; break;
                case WTri: frame = 5.f; break;
                case WSaw: case WSuper: frame = 10.f; break;
                case WSquare: frame = 15.f; break;
                case WPulse: table = 4; frame = clampf(std::fabs(0.5f - clampf(P.pw + d[DPw] * 0.45f, 0.05f, 0.95f)) / 0.45f, 0.f, 1.f) * (Wavetables::Frames - 1); break;
                default: table = P.table; frame = clampf(P.pos + d[DOsc1Pos + o], 0.f, 1.f) * (Wavetables::Frames - 1); break;
            }
            const int fa = std::min((int)frame, Wavetables::Frames - 2);
            R.ff = frame - (float)fa;
            R.ta = wt.at(table, fa, mip);
            R.tb = wt.at(table, fa + 1, mip);
        }
        // sub
        subLevel = clampf(s.subLevel + d[DSub], 0.f, 1.f);
        if (subLevel > 0.f) {
            const float hz = noteHz(pitch - 12.f * (1 + s.subOct));
            subDt = hz / sr;
            int mip = 0;
            while (mip < Wavetables::Mips - 1 && (float)((Wavetables::Size / 2) >> mip) > (sr * 0.5f) / hz) ++mip;
            subT = wt.at(0, s.subWave == 0 ? 0 : (s.subWave == 1 ? 5 : 15), mip);
        }
        noiseLevel = clampf(s.noiseLevel + d[DNoise], 0.f, 1.f);
        noiseDark = s.noiseColor < -0.01f; noiseBright = s.noiseColor > 0.01f;
        if (noiseDark) noiseLp.setHz(20000.f * std::pow(2.f, s.noiseColor * 8.f), sr);
        if (noiseBright) noiseHp.setHz(20.f * std::pow(2.f, s.noiseColor * 9.f), sr);
        // filters
        parallel = s.parallel;
        f2on = s.flt[1].on;
        setupFilter(flt[0], s.flt[0], pitch, env2.v, d[DF1Cut] * 6.f, d[DF1Res], d[DDrive]);
        if (f2on) setupFilter(flt[1], s.flt[1], pitch, env2.v, d[DF2Cut] * 6.f, d[DF2Res], d[DDrive]);
        // output
        const float vol = std::max(0.f, 1.f + d[DVolume]);
        const float pan = clampf(d[DPan], -1.f, 1.f);
        outL = vol * std::cos((pan + 1.f) * kPi * 0.25f) * 1.4142f;
        outR = vol * std::sin((pan + 1.f) * kPi * 0.25f) * 1.4142f;
    }

    inline float readTable(const OscRun& R, float phase) const {
        phase -= std::floor(phase);
        const float x = phase * Wavetables::Size;
        const int k = std::min((int)x, Wavetables::Size - 1); const float fr = x - (float)k;
        const float a = R.ta[k] + (R.ta[k + 1] - R.ta[k]) * fr;
        const float b = R.tb[k] + (R.tb[k + 1] - R.tb[k]) * fr;
        return a + (b - a) * R.ff;
    }
    // Renders one oscillator; returns its mono sum (for FM / ring).
    inline float tickOsc(int o, float pmOffset, float& l, float& r) {
        auto& R = run[o];
        float sl = 0.f, sr2 = 0.f;
        for (int u = 0; u < R.uni; ++u) {
            const float y = R.wave == studio::WNoise ? rng.bi() : readTable(R, ph[o][u] + pmOffset);
            sl += y * R.gl[u]; sr2 += y * R.gr[u];
            ph[o][u] += R.dt[u];
            if (ph[o][u] >= 1.f) ph[o][u] -= 1.f;
        }
        const float g = R.norm * R.level;
        l = sl * g; r = sr2 * g;
        return (sl + sr2) * 0.7071f * R.norm;
    }

    void render(float* L, float* R, int n, const StudioShared& s, float pitchOff, float ampMod) {
        amp.set(s.env[0][0], s.env[0][1], s.env[0][2], s.env[0][3], sr);
        env2.set(s.env[1][0], s.env[1][1], s.env[1][2], s.env[1][3], sr);
        env3.set(s.env[2][0], s.env[2][1], s.env[2][2], s.env[2][3], sr);
        const float base = stepPitch() + pitchOff;
        for (int off = 0; off < n; off += studio::kCtl) {
            const int len = std::min(studio::kCtl, n - off);
            control(s, base, len, s.bpm);
            for (int i = off; i < off + len; ++i) {
                float l = 0.f, r = 0.f, ol, or_;
                float m2 = 0.f, m1 = 0.f;
                if (run[2].on) { tickOsc(2, 0.f, ol, or_); l += ol; r += or_; }
                float l2 = 0.f, r2 = 0.f;
                if (run[1].on) m2 = tickOsc(1, 0.f, l2, r2);
                float l1 = 0.f, r1 = 0.f;
                if (run[0].on) {
                    const float before = ph[0][0];
                    m1 = tickOsc(0, fm * 1.5f * m2, l1, r1);
                    if (sync && run[1].on && ph[0][0] < before)   // osc 1 wrapped: restart osc 2
                        for (int u = 0; u < run[1].uni; ++u) ph[1][u] = ph[0][0] * run[1].dt[u] / std::max(run[0].dt[0], 1.0e-9f);
                }
                const float dry = 1.f - ring;
                l += (l1 + l2) * dry; r += (r1 + r2) * dry;
                if (ring > 0.f) { const float rm = m1 * m2 * ring * 1.4f; l += rm; r += rm; }
                if (subLevel > 0.f) {
                    const float x = subPh * Wavetables::Size; const int k = (int)x;
                    const float y = (subT[k] + (subT[k + 1] - subT[k]) * (x - (float)k)) * subLevel;
                    l += y; r += y;
                    subPh += subDt; if (subPh >= 1.f) subPh -= 1.f;
                }
                if (noiseLevel > 0.f) {
                    float nz = rng.bi();
                    if (noiseDark) nz = noiseLp.lp(nz) * 1.5f;
                    else if (noiseBright) nz = noiseHp.hp(nz);
                    nz *= noiseLevel * 0.6f;
                    l += nz; r += nz;
                }
                float yl, yr;
                if (f2on && parallel) {
                    yl = 0.5f * (runFilter(flt[0], 0, l) + runFilter(flt[1], 0, l));
                    yr = 0.5f * (runFilter(flt[0], 1, r) + runFilter(flt[1], 1, r));
                } else {
                    yl = runFilter(flt[0], 0, l); yr = runFilter(flt[0], 1, r);
                    if (f2on) { yl = runFilter(flt[1], 0, yl); yr = runFilter(flt[1], 1, yr); }
                }
                env2.tick(); env3.tick();
                const float a = amp.tick() * velGain * ampMod * 0.3f;
                L[i] += yl * a * outL; R[i] += yr * a * outR;
            }
            if (!amp.active()) break;
        }
    }
};

class StudioEngine : public PolyEngine<StudioVoice, StudioShared, 16> {
public:
    using PolyEngine::PolyEngine;
    int voiceLimit() const override { return shared.voices; }
    void onPrepare() override {
        idx.clear();
        auto add = [this](const juce::String& id) { idx.push_back(params->indexOf(id)); };
        for (int o = 1; o <= 3; ++o)
            for (auto k : { "_on", "_wave", "_table", "_pos", "_pw", "_oct", "_semi", "_fine", "_level", "_pan", "_uni", "_det", "_spread", "_phase" }) add("o" + juce::String(o) + k);
        for (auto k : { "fm", "sync", "ring", "sub_level", "sub_oct", "sub_wave", "noise_level", "noise_color" }) add(k);
        for (int f = 1; f <= 2; ++f) for (auto k : { "_type", "_cut", "_res", "_drive", "_key", "_env", "_vel" }) add("f" + juce::String(f) + k);
        for (auto k : { "f2_on", "f_route" }) add(k);
        for (auto e : { "a", "e2", "e3" }) for (auto k : { "_a", "_d", "_s", "_r" }) add(juce::String(e) + k);
        for (int l = 1; l <= 3; ++l) for (auto k : { "_shape", "_rate", "_sync", "_mode", "_delay", "_fade" }) add("l" + juce::String(l) + k);
        for (int m = 1; m <= 8; ++m) for (auto k : { "_src", "_dst", "_amt" }) add("m" + juce::String(m) + k);
        for (auto k : { "mac1", "mac2", "mac3", "mac4", "voices", "mode", "glide", "bend", "transpose", "vel_sens", "level" }) add(k);
    }
    float v(int k) const { const int i = idx[(size_t)k]; return i >= 0 ? params->get(i) : 0.f; }
    void updateShared(StudioShared& s) override {
        int k = 0;
        for (auto& o : s.osc) {
            o.on = v(k) >= 0.5f; o.wave = (int)v(k + 1); o.table = juce::jlimit(0, Wavetables::Tables - 1, (int)v(k + 2)); o.pos = v(k + 3) * 0.01f;
            o.pw = v(k + 4) * 0.01f; o.pitch = v(k + 5) * 12.f + v(k + 6) + v(k + 7) * 0.01f; o.level = v(k + 8) * 0.01f; o.pan = v(k + 9) * 0.01f;
            o.uni = (int)v(k + 10); o.det = v(k + 11) * 0.01f; o.spread = v(k + 12) * 0.01f; o.freePhase = v(k + 13) >= 0.5f;
            k += 14;
        }
        s.fm = v(k) * 0.01f; s.sync = v(k + 1) >= 0.5f; s.ring = v(k + 2) * 0.01f;
        s.subLevel = v(k + 3) * 0.01f; s.subOct = (int)v(k + 4); s.subWave = (int)v(k + 5); s.noiseLevel = v(k + 6) * 0.01f; s.noiseColor = v(k + 7) * 0.01f;
        k += 8;
        for (auto& f : s.flt) {
            f.type = (int)v(k); f.cut = v(k + 1); f.res = v(k + 2) * 0.01f; f.drive = v(k + 3) * 0.01f; f.key = v(k + 4) * 0.01f; f.env = v(k + 5) * 0.01f; f.vel = v(k + 6) * 0.01f;
            k += 7;
        }
        s.flt[0].on = true;
        s.flt[1].on = v(k) >= 0.5f; s.parallel = (int)v(k + 1) == 1;
        k += 2;
        for (auto& e : s.env) { e[0] = v(k); e[1] = v(k + 1); e[2] = v(k + 2) * 0.01f; e[3] = v(k + 3); k += 4; }
        for (auto& l : s.lfos) { l.shape = (int)v(k); l.rate = v(k + 1); l.sync = (int)v(k + 2); l.mode = (int)v(k + 3); l.delay = v(k + 4); l.fade = v(k + 5); k += 6; }
        for (auto& m : s.mod) { m.src = (int)v(k); m.dst = (int)v(k + 1); m.amt = v(k + 2) * 0.01f; k += 3; }
        for (auto& mc : s.macro) { mc = v(k) * 0.01f; ++k; }
        s.voices = juce::jlimit(1, 16, (int)v(k));
        const int mode = (int)v(k + 1);
        s.c.mono = mode >= 1; s.c.legato = mode == 2;
        s.c.glideMs = v(k + 2); s.c.bendRange = v(k + 3); s.c.transpose = v(k + 4); s.c.velSens = v(k + 5) * 0.01f; s.c.level = db2lin(v(k + 6));
        s.c.lfoPitchCt = 0.f; s.c.lfoAmp = 0.f; s.c.lfoCutoff = 0.f; s.c.modWheelVib = 0.f;
        s.bpm = transport ? transport->bpm : 120.0;
        s.bendRaw = bend; s.wheel = modWheel;
        for (int i = 0; i < 3; ++i) s.sharedLfo[i] = sharedLfo[i];
    }
    void render(float* L, float* R, int n) override {
        updateShared(shared);
        // free-running (shared) LFOs
        const double bpm = transport ? transport->bpm : 120.0;
        for (int k = 0; k < 3; ++k) {
            const auto& lf = shared.lfos[k];
            const float rate = lf.sync > 0 ? (float)(bpm / 60.0) / syncBeats(lf.sync - 1) : lf.rate;
            freePh[k] += rate * (float)n / sr;
            if (freePh[k] >= 1.f) { freePh[k] -= std::floor(freePh[k]); freePrev[k] = freeHold[k]; freeHold[k] = freeRng.bi(); }
            sharedLfo[k] = StudioVoice::shapeAt(lf.shape, freePh[k], freeHold[k], freePrev[k]);
        }
        PolyEngine::render(L, R, n);   // re-reads the params and copies the shared LFO values
    }
private:
    std::vector<int> idx;
    float freePh[3] {}, freeHold[3] {}, freePrev[3] {}, sharedLfo[3] {};
    Rng freeRng { 99u };
};
