#include "Effects.h"
#include "Dsp.h"
#include "Catalog.h"

using namespace dsp;

namespace {

struct P {
    const ParamSet* ps = nullptr;
    std::vector<int> idx;
    void bind(const ParamSet* p, std::initializer_list<const char*> ids) {
        ps = p; idx.clear();
        for (auto* id : ids) idx.push_back(p->indexOf(id));
    }
    float operator[](int k) const { const int i = idx[(size_t)k]; return i >= 0 ? ps->get(i) : 0.f; }
};

class Base : public Effect {
public:
    explicit Base(ParamSetPtr p) : params(std::move(p)) {}
    void prepare(double s, int) override { sr = (float)s; bindParams(); onPrepare(); }
    virtual void bindParams() = 0;
    virtual void onPrepare() {}
protected:
    ParamSetPtr params;
    P p;
    float sr = 48000.f;
    float syncedSeconds(int syncIdx, float freeMs) const {
        return syncIdx > 0 ? (float)(60.0 / bpm()) * syncBeats(syncIdx - 1) : freeMs * 0.001f;
    }
    float syncedHz(int syncIdx, float freeHz) const {
        return syncIdx > 0 ? 1.f / ((float)(60.0 / bpm()) * syncBeats(syncIdx - 1)) : freeHz;
    }
};


// ================================================================ DYNAMICS
class Compressor : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "threshold", "ratio", "attack", "release", "knee", "makeup", "mix" }); }
    void reset() override { env = 0.f; grDb = 0.f; }
    void process(float* L, float* R, int n) override {
        const float thr = p[0], ratio = p[1], knee = p[4], makeup = db2lin(p[5]), mix = p[6] * 0.01f;
        const float att = onePoleCoef(p[2], sr), rel = onePoleCoef(p[3], sr);
        float maxGr = 0.f;
        for (int i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(L[i]), std::fabs(R[i]));
            const float xdb = lin2db(x);
            // soft-knee gain computer
            float over = xdb - thr, gr = 0.f;
            if (knee > 0.f && std::fabs(over) <= knee * 0.5f) gr = (1.f / ratio - 1.f) * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.f * knee);
            else if (over > 0.f) gr = (1.f / ratio - 1.f) * over;
            // smooth in the gain domain (attack when reducing more)
            grDb += (gr < grDb ? att : rel) * (gr - grDb);
            const float g = db2lin(grDb) * makeup;
            const float wl = L[i] * g, wr = R[i] * g;
            L[i] = L[i] + (wl - L[i]) * mix;
            R[i] = R[i] + (wr - R[i]) * mix;
            maxGr = std::min(maxGr, grDb);
        }
        meterDb.store(maxGr);
    }
private:
    float env = 0.f, grDb = 0.f;
};

// Look-ahead brickwall limiter: sliding-window minimum of required gain, then a box filter
// of the same length, applied to audio delayed by the window. Output never exceeds the ceiling.
class Limiter : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "gain", "ceiling", "release" }); }
    void onPrepare() override {
        la = std::max(8, (int)(sr * 0.0015f));
        dl[0].allocate(la + 4); dl[1].allocate(la + 4);
        ring.assign((size_t)la + 1, 1.f);
        box.assign((size_t)la, 1.f);
        dqIdx.assign((size_t)la + 2, 0); dqVal.assign((size_t)la + 2, 1.f);
        reset();
    }
    void reset() override {
        for (auto& d : dl) d.clear();
        std::fill(box.begin(), box.end(), 1.f);
        boxSum = (double)la; boxPos = 0; head = tail = 0; count = 0; relState = 1.f;
    }
    int latencySamples() const override { return la; }
    void process(float* L, float* R, int n) override {
        const float in = db2lin(p[0]), ceil = db2lin(p[1]);
        const float relK = onePoleCoef(p[2], sr);
        float minG = 1.f;
        for (int i = 0; i < n; ++i) {
            const float l = L[i] * in, r = R[i] * in;
            const float pk = std::max(std::fabs(l), std::fabs(r));
            float req = pk > ceil ? ceil / pk : 1.f;
            // release smoothing on the requirement (attack is handled by min+box look-ahead)
            relState = req < relState ? req : relState + (req - relState) * relK;
            req = relState;
            // sliding minimum over the last la+1 samples (monotonic deque in a ring)
            const size_t cap = dqIdx.size();
            while (head != tail && dqVal[(tail + cap - 1) % cap] >= req) tail = (tail + cap - 1) % cap;
            dqVal[tail] = req; dqIdx[tail] = count; tail = (tail + 1) % cap;
            while (dqIdx[head] + (uint64_t)la < count) head = (head + 1) % cap;
            const float wmin = dqVal[head];
            ++count;
            // box filter (moving average) of length la
            boxSum += wmin - box[(size_t)boxPos];
            box[(size_t)boxPos] = wmin;
            boxPos = (boxPos + 1) % la;
            const float g = (float)(boxSum / la);
            dl[0].push(l); dl[1].push(r);
            float ol = dl[0].tap(la - 1) * g, orr = dl[1].tap(la - 1) * g;
            ol = clampf(ol, -ceil, ceil); orr = clampf(orr, -ceil, ceil);   // numerical safety
            L[i] = ol; R[i] = orr;
            minG = std::min(minG, g);
        }
        meterDb.store(lin2db(minG));
    }
private:
    int la = 64, boxPos = 0;
    DelayLine dl[2];
    std::vector<float> ring, box, dqVal;
    std::vector<uint64_t> dqIdx;
    size_t head = 0, tail = 0;
    uint64_t count = 0;
    double boxSum = 0.0;
    float relState = 1.f;
};

class Gate : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "threshold", "range", "attack", "hold", "release" }); }
    void reset() override { env = 0.f; g = 0.f; holdLeft = 0; }
    void process(float* L, float* R, int n) override {
        const float thr = db2lin(p[0]), floor = db2lin(p[1]);
        const float att = onePoleCoef(p[2], sr), rel = onePoleCoef(p[4], sr);
        const int hold = (int)(p[3] * 0.001f * sr);
        const float detA = onePoleCoef(0.1f, sr), detR = onePoleCoef(20.f, sr);
        for (int i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(L[i]), std::fabs(R[i]));
            env += (x > env ? detA : detR) * (x - env);
            bool open = env > thr;
            if (open) holdLeft = hold; else if (holdLeft > 0) { --holdLeft; open = true; }
            const float target = open ? 1.f : floor;
            g += (target > g ? att : rel) * (target - g);
            L[i] *= g; R[i] *= g;
        }
        meterDb.store(lin2db(g));
    }
private:
    float env = 0.f, g = 0.f;
    int holdLeft = 0;
};

class Expander : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "threshold", "ratio", "range", "attack", "release" }); }
    void reset() override { env = 0.f; grDb = 0.f; }
    void process(float* L, float* R, int n) override {
        const float thr = p[0], ratio = p[1], range = p[2];
        const float att = onePoleCoef(p[3], sr), rel = onePoleCoef(p[4], sr);
        const float detA = onePoleCoef(0.5f, sr), detR = onePoleCoef(30.f, sr);
        for (int i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(L[i]), std::fabs(R[i]));
            env += (x > env ? detA : detR) * (x - env);
            const float xdb = lin2db(env);
            const float gr = xdb < thr ? std::max(range, (xdb - thr) * (ratio - 1.f)) : 0.f;
            grDb += (gr > grDb ? att : rel) * (gr - grDb);
            const float g = db2lin(grDb);
            L[i] *= g; R[i] *= g;
        }
        meterDb.store(grDb);
    }
private:
    float env = 0.f, grDb = 0.f;
};

// ================================================================ EQ / FILTERS
class EQ3 : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "low", "low_f", "mid", "mid_f", "mid_q", "high", "high_f", "output" }); }
    void reset() override { for (auto& b : bq) b.reset(); }
    void process(float* L, float* R, int n) override {
        const float key = p[0] + p[1] * 3 + p[2] * 7 + p[3] * 11 + p[4] * 13 + p[5] * 17 + p[6] * 19;
        if (key != lastKey) {
            for (int c = 0; c < 2; ++c) {
                bq[c * 3 + 0].set(Biquad::LowShelf, p[1], 0.707f, p[0], sr);
                bq[c * 3 + 1].set(Biquad::Peak, p[3], p[4], p[2], sr);
                bq[c * 3 + 2].set(Biquad::HighShelf, p[6], 0.707f, p[5], sr);
            }
            lastKey = key;
        }
        const float out = db2lin(p[7]);
        for (int i = 0; i < n; ++i) {
            L[i] = bq[2].tick(bq[1].tick(bq[0].tick(L[i]))) * out;
            R[i] = bq[5].tick(bq[4].tick(bq[3].tick(R[i]))) * out;
        }
    }
private:
    Biquad bq[6];
    float lastKey = -1.e9f;
};

class ParametricEQ : public Base {
public:
    using Base::Base;
    void bindParams() override {
        p.bind(params.get(), { "lc_on", "lc_f", "b1_f", "b1_g", "b2_f", "b2_g", "b2_q", "b3_f", "b3_g", "b3_q", "b4_f", "b4_g", "hc_on", "hc_f", "output" });
    }
    void reset() override { for (auto& c : ch) for (auto& b : c) b.reset(); }
    void process(float* L, float* R, int n) override {
        float key = 0.f;
        for (int k = 0; k < 14; ++k) key += p[k] * (float)(k * 7 + 1);
        if (key != lastKey) {
            for (auto& c : ch) {
                c[0].set(Biquad::HP, p[1], 0.541f, 0, sr); c[1].set(Biquad::HP, p[1], 1.307f, 0, sr);
                c[2].set(Biquad::LowShelf, p[2], 0.707f, p[3], sr);
                c[3].set(Biquad::Peak, p[4], p[6], p[5], sr);
                c[4].set(Biquad::Peak, p[7], p[9], p[8], sr);
                c[5].set(Biquad::HighShelf, p[10], 0.707f, p[11], sr);
                c[6].set(Biquad::LP, p[13], 0.541f, 0, sr); c[7].set(Biquad::LP, p[13], 1.307f, 0, sr);
            }
            lastKey = key;
        }
        const bool lc = p[0] >= 0.5f, hc = p[12] >= 0.5f;
        const float out = db2lin(p[14]);
        float* io[2] = { L, R };
        for (int c = 0; c < 2; ++c) {
            auto& b = ch[c];
            float* x = io[c];
            for (int i = 0; i < n; ++i) {
                float y = x[i];
                if (lc) y = b[1].tick(b[0].tick(y));
                y = b[5].tick(b[4].tick(b[3].tick(b[2].tick(y))));
                if (hc) y = b[7].tick(b[6].tick(y));
                x[i] = y * out;
            }
        }
    }
private:
    Biquad ch[2][8];
    float lastKey = -1.e9f;
};

class FilterFx : public Base {
public:
    FilterFx(ParamSetPtr ps, int m) : Base(std::move(ps)), mode(m) {}
    void bindParams() override { p.bind(params.get(), { "cutoff", "reso", "slope", "mix" }); }
    void reset() override { for (auto& f : f1) f.reset(); for (auto& f : f2) f.reset(); }
    void process(float* L, float* R, int n) override {
        const float q = 0.5f + p[1] * 0.01f * 10.f;
        const bool steep = p[2] >= 0.5f;
        for (int c = 0; c < 2; ++c) { f1[c].set(p[0], steep ? q * 0.7f : q, sr); f2[c].set(p[0], 0.54f, sr); }
        const float mix = p[3] * 0.01f;
        float* io[2] = { L, R };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                const float x = io[c][i];
                f1[c].tick(x);
                float y = mode == 0 ? f1[c].hp : (mode == 1 ? f1[c].lp : f1[c].bp);
                if (steep) { f2[c].tick(y); y = mode == 0 ? f2[c].hp : (mode == 1 ? f2[c].lp : f2[c].bp * 1.5f); }
                io[c][i] = x + (y - x) * mix;
            }
    }
private:
    int mode;
    SVF f1[2], f2[2];
};

// ================================================================ TIME
// 8-line feedback delay network with Hadamard mixing, damping and modulation.
class Reverb : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "size", "decay", "damping", "predelay", "lowcut", "width", "mix" }); }
    void onPrepare() override {
        for (auto& d : lines) d.allocate((int)(sr * 0.2f));
        pre.allocate((int)(sr * 0.25f));
        for (auto& a : ap) a.allocate((int)(sr * 0.02f));
        reset();
    }
    void reset() override {
        for (auto& d : lines) d.clear();
        pre.clear();
        for (auto& a : ap) a.clear();
        for (auto& f : damp) f.reset();
        hp[0].reset(); hp[1].reset();
        modPh = 0.f;
    }
    void process(float* L, float* R, int n) override {
        static const float base[8] = { 29.7f, 37.1f, 41.1f, 43.7f, 47.3f, 53.9f, 59.3f, 67.1f };
        static const float apMs[4] = { 4.77f, 3.59f, 12.73f, 9.31f };
        const float size = 0.35f + p[0] * 0.01f * 1.6f;
        const float decay = std::max(0.1f, p[1]);
        const float dampHz = 18000.f * std::pow(0.04f, p[2] * 0.01f);
        const int preD = (int)(p[3] * 0.001f * sr);
        const float width = p[5] * 0.01f, mix = p[6] * 0.01f;
        float len[8], gain[8];
        for (int k = 0; k < 8; ++k) {
            len[k] = base[k] * size * 0.001f * sr;
            gain[k] = std::pow(10.f, -3.f * (len[k] / sr) / decay);
            damp[k].setHz(dampHz, sr);
        }
        hp[0].setHz(p[4], sr); hp[1].setHz(p[4], sr);
        for (int i = 0; i < n; ++i) {
            const float dl = L[i], dr = R[i];
            pre.push(0.5f * (dl + dr));
            float x = hp[0].hp(pre.tap(std::min(preD, pre.capacity() - 1)));
            for (int a = 0; a < 4; ++a) {    // input diffusion (Schroeder allpasses)
                const int d = (int)(apMs[a] * 0.001f * sr);
                const float delayed = ap[a].tap(d - 1);
                const float y = -0.6f * x + delayed;
                ap[a].push(x + 0.6f * y);
                x = y;
            }
            modPh += 0.7f / sr; if (modPh > 1.f) modPh -= 1.f;
            const float mod = std::sin(modPh * kTwoPi) * 8.f;
            float s[8];
            for (int k = 0; k < 8; ++k) {
                const float d = len[k] + (k == 1 ? mod : (k == 5 ? -mod : 0.f));
                s[k] = damp[k].lp(lines[k].read(d)) * gain[k];
            }
            // fast Walsh-Hadamard transform (orthogonal mixing)
            for (int h = 1; h < 8; h <<= 1)
                for (int a = 0; a < 8; a += h * 2)
                    for (int b = a; b < a + h; ++b) { const float u = s[b], v = s[b + h]; s[b] = u + v; s[b + h] = u - v; }
            for (int k = 0; k < 8; ++k) lines[k].push(s[k] * 0.35355339f + x * (k & 1 ? 0.5f : -0.5f));
            float wl = (s[0] + s[2] + s[4] + s[6]) * 0.25f, wr = (s[1] + s[3] + s[5] + s[7]) * 0.25f;
            const float m = 0.5f * (wl + wr), sd = 0.5f * (wl - wr) * width;
            wl = m + sd; wr = m - sd;
            L[i] = dl * (1.f - mix) + wl * mix * 1.3f;
            R[i] = dr * (1.f - mix) + wr * mix * 1.3f;
        }
    }
private:
    DelayLine lines[8], pre, ap[4];
    OnePole damp[8], hp[2];
    float modPh = 0.f;
};

class Delay : public Base {
public:
    Delay(ParamSetPtr ps, bool pingPong) : Base(std::move(ps)), pp(pingPong) {}
    void bindParams() override {
        if (pp) p.bind(params.get(), { "sync", "time", "feedback", "tone", "mix", "width" });
        else p.bind(params.get(), { "sync", "time", "feedback", "tone", "mix" });
    }
    void onPrepare() override { dl[0].allocate((int)(sr * 4.2f)); dl[1].allocate((int)(sr * 4.2f)); reset(); }
    void reset() override { dl[0].clear(); dl[1].clear(); lp[0].reset(); lp[1].reset(); cur = -1.f; }
    void process(float* L, float* R, int n) override {
        const float target = clampf(syncedSeconds((int)p[0], p[1]) * sr, 1.f, (float)dl[0].capacity() - 2.f);
        if (cur < 0.f) cur = target;
        const float fb = p[2] * 0.01f, mix = p[4] * 0.01f;
        const float width = pp ? p[5] * 0.01f : 1.f;
        lp[0].setHz(p[3], sr); lp[1].setHz(p[3], sr);
        for (int i = 0; i < n; ++i) {
            cur += (target - cur) * 0.0005f;  // smooth time changes (tape-like glide, no clicks)
            const float yl = lp[0].lp(dl[0].read(cur)), yr = lp[1].lp(dl[1].read(cur));
            if (pp) {
                const float in = 0.5f * (L[i] + R[i]);
                dl[0].push(in + yr * fb);
                dl[1].push(yl * fb);
                const float m = 0.5f * (yl + yr), s = 0.5f * (yl - yr) * width;
                L[i] = L[i] * (1.f - mix * 0.5f) + (m + s) * mix;
                R[i] = R[i] * (1.f - mix * 0.5f) + (m - s) * mix;
            } else {
                dl[0].push(L[i] + yl * fb);
                dl[1].push(R[i] + yr * fb);
                L[i] = L[i] * (1.f - mix * 0.5f) + yl * mix;
                R[i] = R[i] * (1.f - mix * 0.5f) + yr * mix;
            }
        }
    }
private:
    bool pp;
    DelayLine dl[2];
    OnePole lp[2];
    float cur = -1.f;
};

class TapeEcho : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "sync", "time", "feedback", "wow", "sat", "tone", "lowcut", "mix" }); }
    void onPrepare() override { dl.allocate((int)(sr * 4.2f)); reset(); }
    void reset() override { dl.clear(); lp.reset(); hp.reset(); cur = -1.f; ph1 = ph2 = 0.f; }
    void process(float* L, float* R, int n) override {
        const float target = clampf(syncedSeconds((int)p[0], p[1]) * sr, 4.f, (float)dl.capacity() - sr * 0.05f);
        if (cur < 0.f) cur = target;
        const float fb = p[2] * 0.01f, wow = p[3] * 0.01f, sat = p[4] * 0.01f, mix = p[7] * 0.01f;
        lp.setHz(p[5], sr); hp.setHz(p[6], sr);
        const float drive = 1.f + sat * 4.f;
        for (int i = 0; i < n; ++i) {
            cur += (target - cur) * 0.0003f;
            ph1 += 0.6f / sr; ph2 += 6.3f / sr;
            if (ph1 > 1.f) ph1 -= 1.f;
            if (ph2 > 1.f) ph2 -= 1.f;
            const float mod = (std::sin(ph1 * kTwoPi) * 0.004f + std::sin(ph2 * kTwoPi) * 0.0006f) * wow * sr;
            float y = dl.read(cur + mod);
            y = hp.hp(lp.lp(y));
            const float in = 0.5f * (L[i] + R[i]);
            dl.push(fastTanh((in + y * fb) * drive) / drive * (1.f + sat * 0.5f));
            L[i] = L[i] * (1.f - mix * 0.5f) + y * mix;
            R[i] = R[i] * (1.f - mix * 0.5f) + y * mix;
        }
    }
private:
    DelayLine dl;
    OnePole lp, hp;
    float cur = -1.f, ph1 = 0.f, ph2 = 0.f;
};

class Chorus : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "rate", "depth", "delay", "voices", "width", "mix" }); }
    void onPrepare() override { dl[0].allocate((int)(sr * 0.06f)); dl[1].allocate((int)(sr * 0.06f)); reset(); }
    void reset() override { dl[0].clear(); dl[1].clear(); ph = 0.f; }
    void process(float* L, float* R, int n) override {
        const float rate = p[0], depth = p[1] * 0.01f, base = p[2] * 0.001f * sr, width = p[4] * 0.01f, mix = p[5] * 0.01f;
        const int voices = juce::jlimit(1, 4, (int)p[3]);
        const float swing = base * 0.45f * depth;
        for (int i = 0; i < n; ++i) {
            dl[0].push(L[i]); dl[1].push(R[i]);
            ph += rate / sr; if (ph > 1.f) ph -= 1.f;
            float wl = 0.f, wr = 0.f;
            for (int v = 0; v < voices; ++v) {
                const float o = (float)v / (float)voices;
                const float ml = std::sin((ph + o) * kTwoPi), mr = std::sin((ph + o + 0.25f * width) * kTwoPi);
                wl += dl[0].read(base + swing * ml);
                wr += dl[1].read(base + swing * mr);
            }
            wl /= (float)voices; wr /= (float)voices;
            L[i] = L[i] * (1.f - mix * 0.5f) + wl * mix;
            R[i] = R[i] * (1.f - mix * 0.5f) + wr * mix;
        }
    }
private:
    DelayLine dl[2];
    float ph = 0.f;
};

class Flanger : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "rate", "depth", "delay", "feedback", "mix" }); }
    void onPrepare() override { dl[0].allocate((int)(sr * 0.03f)); dl[1].allocate((int)(sr * 0.03f)); reset(); }
    void reset() override { dl[0].clear(); dl[1].clear(); ph = 0.f; fbL = fbR = 0.f; }
    void process(float* L, float* R, int n) override {
        const float rate = p[0], depth = p[1] * 0.01f, base = p[2] * 0.001f * sr, fb = p[3] * 0.01f, mix = p[4] * 0.01f;
        for (int i = 0; i < n; ++i) {
            ph += rate / sr; if (ph > 1.f) ph -= 1.f;
            const float ml = 0.5f + 0.5f * std::sin(ph * kTwoPi), mr = 0.5f + 0.5f * std::sin((ph + 0.25f) * kTwoPi);
            dl[0].push(L[i] + fbL * fb); dl[1].push(R[i] + fbR * fb);
            const float wl = dl[0].read(1.f + base * (1.f - depth + depth * ml * 2.f) );
            const float wr = dl[1].read(1.f + base * (1.f - depth + depth * mr * 2.f));
            fbL = flush(wl); fbR = flush(wr);
            L[i] = L[i] * (1.f - mix * 0.5f) + wl * mix;
            R[i] = R[i] * (1.f - mix * 0.5f) + wr * mix;
        }
    }
private:
    DelayLine dl[2];
    float ph = 0.f, fbL = 0.f, fbR = 0.f;
};

class Phaser : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "rate", "depth", "stages", "feedback", "center", "mix" }); }
    void reset() override { std::memset(st, 0, sizeof(st)); fb[0] = fb[1] = 0.f; ph = 0.f; }
    void process(float* L, float* R, int n) override {
        const float rate = p[0], depth = p[1] * 0.01f, feedback = p[3] * 0.01f, center = p[4], mix = p[5] * 0.01f;
        const int stages = juce::jlimit(2, 12, (int)p[2]);
        float* io[2] = { L, R };
        for (int i = 0; i < n; ++i) {
            ph += rate / sr; if (ph > 1.f) ph -= 1.f;
            for (int c = 0; c < 2; ++c) {
                const float m = std::sin((ph + (c ? 0.25f : 0.f)) * kTwoPi);
                const float hz = clampf(center * std::pow(2.f, m * depth * 2.f), 40.f, sr * 0.4f);
                const float t = std::tan(kPi * hz / sr);
                const float a = (t - 1.f) / (t + 1.f);
                float x = io[c][i] + fb[c] * feedback;
                for (int s = 0; s < stages; ++s) {
                    const float y = a * x + st[c][s];
                    st[c][s] = flush(x - a * y);
                    x = y;
                }
                fb[c] = flush(x);
                io[c][i] = io[c][i] * (1.f - mix * 0.5f) + x * mix;
            }
        }
    }
private:
    float st[2][12] {};
    float fb[2] {}, ph = 0.f;
};

// ================================================================ DISTORTION
class Saturation : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "drive", "tone", "output", "mix" }); }
    void reset() override { lp[0].reset(); lp[1].reset(); }
    void process(float* L, float* R, int n) override {
        const float d = db2lin(p[0]), out = db2lin(p[2]), mix = p[3] * 0.01f;
        const float norm = 1.f / std::max(0.3f, fastTanh(d));
        lp[0].setHz(p[1], sr); lp[1].setHz(p[1], sr);
        float* io[2] = { L, R };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                const float x = io[c][i];
                const float y = lp[c].lp(fastTanh(x * d) * norm * 0.8f) * out;
                io[c][i] = x + (y - x) * mix;
            }
    }
private:
    OnePole lp[2];
};

class Overdrive : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "drive", "tone", "output", "mix" }); }
    void reset() override { for (auto& f : pre) f.reset(); for (auto& f : post) f.reset(); dcx[0] = dcx[1] = dcy[0] = dcy[1] = 0.f; }
    void process(float* L, float* R, int n) override {
        const float k = 1.f + p[0] * 0.01f * 40.f, out = db2lin(p[2]), mix = p[3] * 0.01f;
        for (int c = 0; c < 2; ++c) { pre[c].set(Biquad::HighShelf, 700.f, 0.7f, 6.f, sr); post[c].set(Biquad::LP, p[1], 0.7f, 0.f, sr); }
        float* io[2] = { L, R };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                const float x = io[c][i];
                float y = pre[c].tick(x) * k;
                y = y > 0.f ? fastTanh(y) : fastTanh(y * 0.6f) / 0.6f * 0.8f;     // asymmetric
                const float dc = y - dcx[c] + 0.995f * dcy[c];
                dcx[c] = y; dcy[c] = dc;
                y = post[c].tick(dc) * out / std::sqrt(std::max(1.f, k * 0.25f));
                io[c][i] = x + (y - x) * mix;
            }
    }
private:
    Biquad pre[2], post[2];
    float dcx[2] {}, dcy[2] {};
};

class Distortion : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "type", "drive", "tone", "output", "mix" }); }
    void reset() override { lp[0].reset(); lp[1].reset(); dcx[0] = dcx[1] = dcy[0] = dcy[1] = 0.f; }
    void process(float* L, float* R, int n) override {
        const int type = (int)p[0];
        const float d = db2lin(p[1]), out = db2lin(p[3]), mix = p[4] * 0.01f;
        lp[0].set(Biquad::LP, p[2], 0.7f, 0, sr); lp[1] = lp[0];
        float* io[2] = { L, R };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                const float x = io[c][i];
                float v = x * d, y;
                switch (type) {
                    case 0: y = clampf(v, -1.f, 1.f); break;
                    case 1: y = std::sin(v); break;                                   // wavefold
                    case 2: y = (v > 0.f ? 1.f : -1.f) * (1.f - std::exp(-std::fabs(v))); break;  // fuzz
                    default: y = std::fabs(fastTanh(v)) * 2.f - 0.6f; break;           // rectify (octave up)
                }
                const float dc = y - dcx[c] + 0.995f * dcy[c];
                dcx[c] = y; dcy[c] = dc;
                y = lp[c].tick(dc) * out;
                io[c][i] = x + (y - x) * mix;
            }
    }
private:
    Biquad lp[2];
    float dcx[2] {}, dcy[2] {};
};

class Bitcrusher : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "bits", "down", "output", "mix" }); }
    void reset() override { hold[0] = hold[1] = 0.f; cnt = 0; }
    void process(float* L, float* R, int n) override {
        const float q = std::pow(2.f, p[0] - 1.f);   // 2^bits levels, mid-rise
        const int down = std::max(1, (int)p[1]);
        const float out = db2lin(p[2]), mix = p[3] * 0.01f;
        for (int i = 0; i < n; ++i) {
            if (cnt++ % down == 0) {
                hold[0] = clampf(std::floor(L[i] * q) + 0.5f, -q + 0.5f, q - 0.5f) / q;
                hold[1] = clampf(std::floor(R[i] * q) + 0.5f, -q + 0.5f, q - 0.5f) / q;
            }
            L[i] = L[i] + (hold[0] * out - L[i]) * mix;
            R[i] = R[i] + (hold[1] * out - R[i]) * mix;
        }
    }
private:
    float hold[2] {};
    uint32_t cnt = 0;
};

// ================================================================ STEREO / MODULATION / OTHER
class Widener : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "width", "haas", "bass_mono" }); }
    void onPrepare() override { dl.allocate((int)(sr * 0.035f)); reset(); }
    void reset() override { dl.clear(); lp.reset(); }
    void process(float* L, float* R, int n) override {
        const float w = p[0] * 0.01f;
        const int haas = (int)(p[1] * 0.001f * sr);
        lp.setHz(p[2], sr);
        for (int i = 0; i < n; ++i) {
            const float m = 0.5f * (L[i] + R[i]);
            float s = 0.5f * (L[i] - R[i]);
            s = (s - lp.lp(s)) * w;                 // side above the bass-mono frequency only
            float l = m + s, r = m - s;
            dl.push(r);
            if (haas > 0) r = dl.tap(haas);
            L[i] = l; R[i] = r;
        }
    }
private:
    DelayLine dl;
    OnePole lp;
};

class Imager : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "xover", "low_w", "high_w", "balance" }); }
    void reset() override { for (auto& b : lp) b.reset(); }
    void process(float* L, float* R, int n) override {
        if (p[0] != lastX) { for (int c = 0; c < 2; ++c) { lp[c * 2].set(Biquad::LP, p[0], 0.707f, 0, sr); lp[c * 2 + 1] = lp[c * 2]; } lastX = p[0]; }
        const float lw = p[1] * 0.01f, hw = p[2] * 0.01f, bal = p[3] * 0.01f;
        const float gl = bal > 0.f ? 1.f - bal : 1.f, gr = bal < 0.f ? 1.f + bal : 1.f;
        for (int i = 0; i < n; ++i) {
            const float lo_l = lp[1].tick(lp[0].tick(L[i])), lo_r = lp[3].tick(lp[2].tick(R[i]));
            const float hi_l = L[i] - lo_l, hi_r = R[i] - lo_r;
            const float lm = 0.5f * (lo_l + lo_r), ls = 0.5f * (lo_l - lo_r) * lw;
            const float hm = 0.5f * (hi_l + hi_r), hs = 0.5f * (hi_l - hi_r) * hw;
            L[i] = (lm + ls + hm + hs) * gl;
            R[i] = (lm - ls + hm - hs) * gr;
        }
    }
private:
    Biquad lp[4];
    float lastX = -1.f;
};

// Dual-head delay-line pitch shifter with sine-squared crossfades.
class PitchShifter : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "semis", "cents", "window", "mix" }); }
    void onPrepare() override { dl[0].allocate((int)(sr * 0.25f)); dl[1].allocate((int)(sr * 0.25f)); reset(); }
    void reset() override { dl[0].clear(); dl[1].clear(); phase = 0.f; }
    void process(float* L, float* R, int n) override {
        const float ratio = std::pow(2.f, (p[0] + p[1] * 0.01f) / 12.f);
        const float win = clampf(p[2] * 0.001f * sr, 64.f, (float)dl[0].capacity() - 8.f);
        const float mix = p[3] * 0.01f;
        const float step = (1.f - ratio) / win;  // phase advance per sample
        for (int i = 0; i < n; ++i) {
            dl[0].push(L[i]); dl[1].push(R[i]);
            phase += step;
            phase -= std::floor(phase);
            const float p2 = phase + 0.5f - std::floor(phase + 0.5f);
            const float d1 = phase * win + 1.f, d2 = p2 * win + 1.f;
            const float g1 = std::sin(phase * kPi), g2 = std::sin(p2 * kPi);
            const float w1 = g1 * g1, w2 = g2 * g2;
            const float yl = dl[0].read(d1) * w1 + dl[0].read(d2) * w2;
            const float yr = dl[1].read(d1) * w1 + dl[1].read(d2) * w2;
            L[i] = L[i] + (yl - L[i]) * mix;
            R[i] = R[i] + (yr - R[i]) * mix;
        }
    }
private:
    DelayLine dl[2];
    float phase = 0.f;
};

inline float shapeLfo(int shape, float ph) {
    switch (shape) {
        case 1: return 1.f - 4.f * std::fabs(ph - 0.5f);
        case 2: return 2.f * ph - 1.f;
        case 3: return ph < 0.5f ? 1.f : -1.f;
        default: return std::sin(ph * kTwoPi);
    }
}

class AutoPan : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "sync", "rate", "depth", "shape" }); }
    void reset() override { ph = 0.f; sm = 0.f; }
    void process(float* L, float* R, int n) override {
        const float hz = syncedHz((int)p[0], p[1]);
        // lock phase to song position when synced and playing
        if ((int)p[0] > 0 && transport && transport->playing) {
            const double beats = transport->tick / 960.0;
            const double cyc = beats / syncBeats((int)p[0] - 1);
            ph = (float)(cyc - std::floor(cyc));
        }
        const float depth = p[2] * 0.01f;
        const int shape = (int)p[3];
        for (int i = 0; i < n; ++i) {
            ph += hz / sr; if (ph >= 1.f) ph -= 1.f;
            const float target = shapeLfo(shape, ph) * depth;
            sm += (target - sm) * 0.01f;                 // de-click square shapes
            const float a = (sm + 1.f) * kPi * 0.25f;
            const float m = 0.5f * (L[i] + R[i]);
            const float side = 0.5f * (L[i] - R[i]);
            L[i] = (m + side) * std::cos(a) * 1.4142f;
            R[i] = (m - side) * std::sin(a) * 1.4142f;
        }
    }
private:
    float ph = 0.f, sm = 0.f;
};

class Tremolo : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "sync", "rate", "depth", "shape", "stereo" }); }
    void reset() override { ph = 0.f; sl = sr2 = 1.f; }
    void process(float* L, float* R, int n) override {
        const float hz = syncedHz((int)p[0], p[1]);
        if ((int)p[0] > 0 && transport && transport->playing) {
            const double cyc = transport->tick / 960.0 / syncBeats((int)p[0] - 1);
            ph = (float)(cyc - std::floor(cyc));
        }
        const float depth = p[2] * 0.01f, off = p[4] / 360.f;
        const int shape = (int)p[3];
        for (int i = 0; i < n; ++i) {
            ph += hz / sr; if (ph >= 1.f) ph -= 1.f;
            float p2 = ph + off; if (p2 >= 1.f) p2 -= 1.f;
            const float gl = 1.f - depth * 0.5f * (1.f + shapeLfo(shape, ph));
            const float gr = 1.f - depth * 0.5f * (1.f + shapeLfo(shape, p2));
            sl += (gl - sl) * 0.02f; sr2 += (gr - sr2) * 0.02f;
            L[i] *= sl; R[i] *= sr2;
        }
    }
private:
    float ph = 0.f, sl = 1.f, sr2 = 1.f;
};

class TransientShaper : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "attack", "sustain", "output" }); }
    void onPrepare() override { fast.set(0.5f, 30.f, sr); slow.set(25.f, 250.f, sr); reset(); }
    void reset() override { fast.env = slow.env = 0.f; g = 1.f; }
    void process(float* L, float* R, int n) override {
        const float a = p[0] * 0.01f, s = p[1] * 0.01f, out = db2lin(p[2]);
        for (int i = 0; i < n; ++i) {
            const float x = 0.5f * (std::fabs(L[i]) + std::fabs(R[i]));
            const float f = fast.tick(x), sl = slow.tick(x);
            const float ratio = (f + 1.0e-5f) / (sl + 1.0e-5f);      // >1 during attacks, <1 in tails
            const float atk = std::pow(clampf(ratio, 0.25f, 4.f), a * 1.5f);
            const float sus = std::pow(clampf(1.f / ratio, 0.25f, 4.f), s * 1.5f);
            const float target = clampf(atk * sus, 0.1f, 6.f);
            g += (target - g) * 0.2f;
            L[i] *= g * out; R[i] *= g * out;
        }
    }
private:
    EnvFollower fast, slow;
    float g = 1.f;
};

// ================================================================ DE-ESSER
// Sidechain: high-pass band -> fast envelope -> gain reduction on the highs (or on everything).
class DeEsser : public Base {
public:
    using Base::Base;
    void bindParams() override { p.bind(params.get(), { "freq", "threshold", "range", "mode", "listen" }); }
    void onPrepare() override { env.set(0.5f, 60.f, sr); reset(); }
    void reset() override { for (auto& f : split) f.reset(); det.reset(); env.env = 0.f; grDb = 0.f; }
    void process(float* L, float* R, int n) override {
        const float hz = p[0], thr = p[1], range = p[2];
        const bool wide = p[3] >= 0.5f, listen = p[4] >= 0.5f;
        for (auto& f : split) f.set(hz, 0.707f, sr);
        det.set(hz, 0.707f, sr);
        const float rel = onePoleCoef(40.f, sr), att = onePoleCoef(0.5f, sr);
        float maxGr = 0.f;
        for (int i = 0; i < n; ++i) {
            det.tick(0.5f * (L[i] + R[i]));
            const float lvl = lin2db(env.tick(det.hp) * 1.4142f);
            const float over = lvl - thr;
            const float want = over > 0.f ? -std::min(range, over * 0.75f) : 0.f;   // ~4:1 above the threshold
            grDb += (want < grDb ? att : rel) * (want - grDb);
            const float g = db2lin(grDb);
            split[0].tick(L[i]); split[1].tick(R[i]);
            if (listen) { L[i] = split[0].hp; R[i] = split[1].hp; }
            else if (wide) { L[i] *= g; R[i] *= g; }
            else {   // keep the low-passed part, turn down everything above it (exact complement)
                L[i] = split[0].lp + (L[i] - split[0].lp) * g;
                R[i] = split[1].lp + (R[i] - split[1].lp) * g;
            }
            maxGr = std::min(maxGr, grDb);
        }
        meterDb.store(maxGr);
    }
private:
    SVF split[2], det;
    EnvFollower env;
    float grDb = 0.f;
};

// ================================================================ EFFECT DESIGNER
// Six modules in series. Each knob is 0..1; Catalog::designerValue maps it to real units, the
// same mapping the editor uses for its labels. Two LFOs and an envelope follower can move any knob.
class Designer : public Base {
public:
    using Base::Base;
    static constexpr int kSlots = 6, kMods = 4, kCtl = 16;
    void bindParams() override {
        std::vector<juce::String> ids = { "in", "out", "mix", "l1_shape", "l1_rate", "l1_sync", "l2_shape", "l2_rate", "l2_sync", "ef_att", "ef_rel", "ef_gain" };
        for (int s = 1; s <= kSlots; ++s) for (auto k : { "_type", "_a", "_b", "_c", "_mix" }) ids.push_back("s" + juce::String(s) + k);
        for (int m = 1; m <= kMods; ++m) for (auto k : { "_src", "_dst", "_amt" }) ids.push_back("m" + juce::String(m) + k);
        idx.clear();
        for (auto& id : ids) idx.push_back(params->indexOf(id));
    }
    float v(int k) const { return idx[(size_t)k] >= 0 ? params->get(idx[(size_t)k]) : 0.f; }
    void onPrepare() override {
        const int maxDelay = (int)(sr * 2.1f);
        for (auto& s : slots) {
            for (auto& d : s.d) d.allocate(maxDelay);
            for (int c = 0; c < 2; ++c) {
                for (int k = 0; k < 4; ++k) { s.combN[c][k] = (int)((kCombLen[k] + (c ? 23 : 0)) * sr / 44100.f); s.comb[c][k].allocate(s.combN[c][k] + 4); }
                for (int k = 0; k < 2; ++k) { s.apN[c][k] = (int)((kApLen[k] + (c ? 23 : 0)) * sr / 44100.f); s.ap[c][k].allocate(s.apN[c][k] + 4); }
            }
        }
        reset();
    }
    void reset() override {
        for (auto& s : slots) {
            for (auto& d : s.d) d.clear();
            for (int c = 0; c < 2; ++c) {
                s.f[c].reset(); s.pk[c].reset(); s.tone[c].reset(); s.fbTone[c].reset(); s.hold[c] = 0.f; s.combLp[c] = 0.f;
                for (auto& cb : s.comb[c]) cb.clear();
                for (auto& a : s.ap[c]) a.clear();
                for (auto& z : s.combDamp[c]) z = 0.f;
            }
            s.timeMs.snap(-1.f); s.ph = 0.f; s.holdCount = 0;
        }
        follower = 0.f; ctl = 0;
    }
    void process(float* L, float* R, int n) override {
        for (int done = 0; done < n;) {
            const int len = std::min(kCtl, n - done);
            control(L + done, R + done, len);
            run(L + done, R + done, len);
            done += len;
        }
        meterDb.store(lin2db(follower));
    }
private:
    static constexpr int kCombLen[4] = { 1116, 1188, 1277, 1356 };
    static constexpr int kApLen[2] = { 556, 441 };
    struct Slot {
        int type = 0;
        float a = 0, b = 0, c = 0, mix = 1;
        SVF f[2];
        Biquad pk[2];
        OnePole tone[2], fbTone[2];
        DelayLine d[2];
        DelayLine comb[2][4], ap[2][2];
        float combDamp[2][4] {}, combLp[2] {};
        int combN[2][4] {}, apN[2][2] {};
        Smoothed timeMs;
        float ph = 0.f, hold[2] {};
        int holdCount = 0;
    };
    std::array<Slot, kSlots> slots;
    std::vector<int> idx;
    LFO lfo1, lfo2;
    float follower = 0.f, inGain = 1.f, outGain = 1.f, wetMix = 1.f;
    int ctl = 0;

    void control(const float* L, const float* R, int len) {
        // modulation sources
        const float r1 = syncedHz((int)v(5), v(4)), r2 = syncedHz((int)v(8), v(7));
        const float s1 = lfo1.advance(r1, sr, len, (int)v(3));
        const float s2 = lfo2.advance(r2, sr, len, (int)v(6));
        inGain = db2lin(v(0));
        const float att = onePoleCoef(v(9), sr), rel = onePoleCoef(v(10), sr), sens = db2lin(v(11));
        for (int i = 0; i < len; ++i) {
            const float x = std::max(std::fabs(L[i]), std::fabs(R[i])) * inGain * sens;
            follower += (x > follower ? att : rel) * (x - follower);
        }
        follower = flush(follower);
        const float srcVal[4] = { 0.f, s1, s2, std::min(follower, 1.f) };
        // base values
        float knob[kSlots][4];
        for (int s = 0; s < kSlots; ++s) {
            slots[(size_t)s].type = (int)v(12 + s * 5);
            for (int k = 0; k < 4; ++k) knob[s][k] = v(13 + s * 5 + k) * 0.01f;
        }
        float outDb = v(1);
        for (int m = 0; m < kMods; ++m) {
            const int src = (int)v(12 + kSlots * 5 + m * 3), dst = (int)v(12 + kSlots * 5 + m * 3 + 1);
            const float amt = v(12 + kSlots * 5 + m * 3 + 2) * 0.01f;
            if (src <= 0 || dst <= 0 || amt == 0.f) continue;
            const float mv = srcVal[juce::jlimit(0, 3, src)] * amt;
            if (dst == 25) outDb += mv * 24.f;
            else { const int t = dst - 1; knob[t / 4][t % 4] += mv; }
        }
        outGain = db2lin(outDb);
        wetMix = v(2) * 0.01f;
        for (int s = 0; s < kSlots; ++s) {
            auto& sl = slots[(size_t)s];
            sl.a = clampf(knob[s][0], 0.f, 1.f); sl.b = clampf(knob[s][1], 0.f, 1.f); sl.c = clampf(knob[s][2], 0.f, 1.f); sl.mix = clampf(knob[s][3], 0.f, 1.f);
            const int t = sl.type;
            auto val = [&](int k) { return Catalog::designerValue(t, k, k == 0 ? sl.a : (k == 1 ? sl.b : sl.c)); };
            if (t >= 1 && t <= 4) for (auto& f : sl.f) f.set(val(0), 0.6f + val(1) * val(1) * 18.f, sr);
            else if (t == 5) { sl.pk[0].set(Biquad::Peak, val(0), val(2), val(1), sr); sl.pk[1].b0 = sl.pk[0].b0; sl.pk[1].b1 = sl.pk[0].b1; sl.pk[1].b2 = sl.pk[0].b2; sl.pk[1].a1 = sl.pk[0].a1; sl.pk[1].a2 = sl.pk[0].a2; }
            else if (t == 6 || t == 9) for (auto& o : sl.tone) o.setHz(val(t == 6 ? 1 : 2), sr);
            else if (t == 7 || t == 8) for (auto& o : sl.tone) o.setHz(val(2), sr);
            if (t == 9) { sl.timeMs.coef = onePoleCoef(60.f, sr); sl.timeMs.setTarget(val(0)); if (sl.timeMs.cur < 0.f) sl.timeMs.snap(val(0)); for (auto& o : sl.fbTone) o.setHz(val(2), sr); }
            if (t == 10) { sl.timeMs.coef = onePoleCoef(30.f, sr); sl.timeMs.setTarget(val(2)); if (sl.timeMs.cur < 0.f) sl.timeMs.snap(val(2)); }
        }
    }

    void run(float* L, float* R, int n) {
        float dryL[kCtl], dryR[kCtl];
        for (int i = 0; i < n; ++i) { dryL[i] = L[i]; dryR[i] = R[i]; L[i] *= inGain; R[i] *= inGain; }
        for (auto& sl : slots) {
            if (sl.type <= 0) continue;
            const int t = sl.type;
            auto val = [&](int k) { return Catalog::designerValue(t, k, k == 0 ? sl.a : (k == 1 ? sl.b : sl.c)); };
            const float m = sl.mix;
            switch (t) {
                case 1: case 2: case 3: case 4: {
                    const float g = db2lin(val(2));
                    for (int i = 0; i < n; ++i) {
                        float* ch[2] = { L + i, R + i };
                        for (int c = 0; c < 2; ++c) {
                            float x = *ch[c];
                            const float in = g > 1.01f ? fastTanh(x * g) : x;
                            sl.f[c].tick(in);
                            const float y = t == 1 ? sl.f[c].lp : (t == 2 ? sl.f[c].hp : (t == 3 ? sl.f[c].bp : sl.f[c].lp + sl.f[c].hp));
                            *ch[c] = x + (y - x) * m;
                        }
                    }
                    break;
                }
                case 5:
                    for (int i = 0; i < n; ++i) { L[i] += (sl.pk[0].tick(L[i]) - L[i]) * m; R[i] += (sl.pk[1].tick(R[i]) - R[i]) * m; }
                    break;
                case 6: {
                    const float g = db2lin(val(0)), bias = val(2) * 0.4f, comp = db2lin(-val(0) * 0.4f), off = fastTanh(bias * g);
                    for (int i = 0; i < n; ++i) {
                        const float yl = sl.tone[0].lp(fastTanh((L[i] + bias) * g) - off) * comp;
                        const float yr = sl.tone[1].lp(fastTanh((R[i] + bias) * g) - off) * comp;
                        L[i] += (yl - L[i]) * m; R[i] += (yr - R[i]) * m;
                    }
                    break;
                }
                case 7: {
                    const float fold = val(0), bias = val(1) * 0.5f;
                    for (int i = 0; i < n; ++i) {
                        const float yl = sl.tone[0].lp(std::sin((L[i] * fold + bias) * kPi * 0.5f));
                        const float yr = sl.tone[1].lp(std::sin((R[i] * fold + bias) * kPi * 0.5f));
                        L[i] += (yl - L[i]) * m; R[i] += (yr - R[i]) * m;
                    }
                    break;
                }
                case 8: {
                    const float q = std::pow(2.f, val(0) - 1.f);
                    const int down = std::max(1, (int)std::lround(val(1)));
                    for (int i = 0; i < n; ++i) {
                        if (sl.holdCount <= 0) { sl.hold[0] = (std::floor(L[i] * q) + 0.5f) / q; sl.hold[1] = (std::floor(R[i] * q) + 0.5f) / q; sl.holdCount = down; }
                        --sl.holdCount;
                        const float yl = sl.tone[0].lp(clampf(sl.hold[0], -1.f, 1.f)), yr = sl.tone[1].lp(clampf(sl.hold[1], -1.f, 1.f));
                        L[i] += (yl - L[i]) * m; R[i] += (yr - R[i]) * m;
                    }
                    break;
                }
                case 9: {
                    const float fb = val(1);
                    for (int i = 0; i < n; ++i) {
                        const float d = std::min(sl.timeMs.next() * 0.001f * sr, (float)sl.d[0].capacity() - 2.f);
                        const float yl = sl.d[0].read(d), yr = sl.d[1].read(d);
                        sl.d[0].push(L[i] + sl.fbTone[0].lp(yl) * fb);
                        sl.d[1].push(R[i] + sl.fbTone[1].lp(yr) * fb);
                        L[i] += (yl - L[i]) * m; R[i] += (yr - R[i]) * m;
                    }
                    break;
                }
                case 10: {
                    const float rate = val(0), depth = val(1);
                    for (int i = 0; i < n; ++i) {
                        sl.ph += rate / sr; if (sl.ph >= 1.f) sl.ph -= 1.f;
                        const float base = sl.timeMs.next() * 0.001f * sr, sw = depth * 0.004f * sr;
                        const float dl = base + sw * (0.5f + 0.5f * std::sin(sl.ph * kTwoPi));
                        const float dr = base + sw * (0.5f + 0.5f * std::sin((sl.ph + 0.5f) * kTwoPi));
                        sl.d[0].push(L[i]); sl.d[1].push(R[i]);
                        const float yl = sl.d[0].read(dl), yr = sl.d[1].read(dr);
                        L[i] += (yl - L[i]) * m * 0.5f; R[i] += (yr - R[i]) * m * 0.5f;
                    }
                    break;
                }
                case 11: {
                    const float f = val(0), shape = val(1), off = val(2) / 360.f;
                    for (int i = 0; i < n; ++i) {
                        sl.ph += f / sr; if (sl.ph >= 1.f) sl.ph -= 1.f;
                        auto car = [shape](float ph) { const float s = std::sin(ph * kTwoPi); return s + ((s >= 0.f ? 1.f : -1.f) - s) * shape; };
                        const float cl = car(sl.ph), cr = car(sl.ph + off);
                        L[i] += (L[i] * cl - L[i]) * m; R[i] += (R[i] * cr - R[i]) * m;
                    }
                    break;
                }
                case 12: {
                    const float delay = std::min(sr / val(0), (float)sl.d[0].capacity() - 2.f), fb = val(1), damp = val(2) * 0.9f;
                    for (int i = 0; i < n; ++i) {
                        float* ch[2] = { L + i, R + i };
                        for (int c = 0; c < 2; ++c) {
                            const float x = *ch[c];
                            sl.combLp[c] += (1.f - damp) * (sl.d[c].read(delay - 1.f) - sl.combLp[c]);
                            sl.combLp[c] = flush(sl.combLp[c]);
                            const float y = fastTanh(x + sl.combLp[c] * fb);
                            sl.d[c].push(y);
                            *ch[c] = x + (y * 0.6f - x) * m;
                        }
                    }
                    break;
                }
                case 13: {
                    const float g = db2lin(val(0)), pan = val(1), width = val(2);
                    const float gl = g * std::cos((pan + 1.f) * kPi * 0.25f) * 1.4142f, gr = g * std::sin((pan + 1.f) * kPi * 0.25f) * 1.4142f;
                    for (int i = 0; i < n; ++i) {
                        const float mid = 0.5f * (L[i] + R[i]), side = 0.5f * (L[i] - R[i]) * width;
                        const float yl = (mid + side) * gl, yr = (mid - side) * gr;
                        L[i] += (yl - L[i]) * m; R[i] += (yr - R[i]) * m;
                    }
                    break;
                }
                case 14: {
                    const float fb = 0.7f + sl.a * 0.28f, damp = sl.b * 0.6f, width = sl.c;
                    for (int i = 0; i < n; ++i) {
                        const float x = (L[i] + R[i]) * 0.5f * 0.05f;
                        float out[2] = { 0.f, 0.f };
                        for (int c = 0; c < 2; ++c) {
                            for (int k = 0; k < 4; ++k) {
                                auto& dl = sl.comb[c][k];
                                const float y = dl.tap(sl.combN[c][k] - 1);
                                sl.combDamp[c][k] = flush(y * (1.f - damp) + sl.combDamp[c][k] * damp);
                                dl.push(x + sl.combDamp[c][k] * fb);
                                out[c] += y;
                            }
                            for (int k = 0; k < 2; ++k) {
                                auto& ap = sl.ap[c][k];
                                const float bo = ap.tap(sl.apN[c][k] - 1);
                                const float y = -out[c] + bo;
                                ap.push(out[c] + bo * 0.5f);
                                out[c] = y;
                            }
                        }
                        const float yl = out[0] * (0.5f + 0.5f * width) + out[1] * (0.5f - 0.5f * width);
                        const float yr = out[1] * (0.5f + 0.5f * width) + out[0] * (0.5f - 0.5f * width);
                        L[i] += yl * m; R[i] += yr * m;   // reverb adds to the signal: Mix = amount of reverb
                    }
                    break;
                }
                case 15: {
                    const float rate = val(0), depth = sl.b, shape = sl.c;
                    for (int i = 0; i < n; ++i) {
                        sl.ph += rate / sr; if (sl.ph >= 1.f) sl.ph -= 1.f;
                        const float s = std::sin(sl.ph * kTwoPi);
                        const float w = s + ((s >= 0.f ? 1.f : -1.f) - s) * shape;
                        const float g = 1.f - depth * (0.5f - 0.5f * w);
                        L[i] += (L[i] * g - L[i]) * m; R[i] += (R[i] * g - R[i]) * m;
                    }
                    break;
                }
                default: break;
            }
        }
        for (int i = 0; i < n; ++i) {
            L[i] = (dryL[i] + (L[i] - dryL[i]) * wetMix) * outGain;
            R[i] = (dryR[i] + (R[i] - dryR[i]) * wetMix) * outGain;
        }
    }
};

}  // namespace

std::unique_ptr<Effect> createEffect(const juce::String& t, ParamSetPtr ps) {
    if (t == "compressor") return std::make_unique<Compressor>(ps);
    if (t == "limiter") return std::make_unique<Limiter>(ps);
    if (t == "gate") return std::make_unique<Gate>(ps);
    if (t == "expander") return std::make_unique<Expander>(ps);
    if (t == "eq3") return std::make_unique<EQ3>(ps);
    if (t == "parametric") return std::make_unique<ParametricEQ>(ps);
    if (t == "highpass") return std::make_unique<FilterFx>(ps, 0);
    if (t == "lowpass") return std::make_unique<FilterFx>(ps, 1);
    if (t == "bandpass") return std::make_unique<FilterFx>(ps, 2);
    if (t == "reverb") return std::make_unique<Reverb>(ps);
    if (t == "delay") return std::make_unique<Delay>(ps, false);
    if (t == "pingpong") return std::make_unique<Delay>(ps, true);
    if (t == "echo") return std::make_unique<TapeEcho>(ps);
    if (t == "chorus") return std::make_unique<Chorus>(ps);
    if (t == "flanger") return std::make_unique<Flanger>(ps);
    if (t == "phaser") return std::make_unique<Phaser>(ps);
    if (t == "saturation") return std::make_unique<Saturation>(ps);
    if (t == "overdrive") return std::make_unique<Overdrive>(ps);
    if (t == "distortion") return std::make_unique<Distortion>(ps);
    if (t == "bitcrusher") return std::make_unique<Bitcrusher>(ps);
    if (t == "widener") return std::make_unique<Widener>(ps);
    if (t == "imager") return std::make_unique<Imager>(ps);
    if (t == "pitch") return std::make_unique<PitchShifter>(ps);
    if (t == "autopan") return std::make_unique<AutoPan>(ps);
    if (t == "tremolo") return std::make_unique<Tremolo>(ps);
    if (t == "transient") return std::make_unique<TransientShaper>(ps);
    if (t == "deesser") return std::make_unique<DeEsser>(ps);
    if (t == "designer") return std::make_unique<Designer>(ps);
    return nullptr;
}
