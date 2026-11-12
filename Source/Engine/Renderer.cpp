#include "Renderer.h"
#include "Catalog.h"
#include "Dsp.h"
#include <mutex>

using namespace dsp;

// =================================================================== snapshot time helpers
double Snapshot::bpmAt(double tick) const {
    double b = tempo.front().bpm;
    for (auto& t : tempo) { if (t.tick <= tick) b = t.bpm; else break; }
    return b;
}
double Snapshot::ticksToSeconds(double tick) const {
    double sec = 0.0, prev = 0.0, bpm = tempo.front().bpm;
    for (size_t i = 1; i < tempo.size(); ++i) {
        if (tempo[i].tick >= tick) break;
        sec += (tempo[i].tick - prev) * 60.0 / (bpm * PPQ);
        prev = tempo[i].tick; bpm = tempo[i].bpm;
    }
    return sec + (tick - prev) * 60.0 / (bpm * PPQ);
}
double Snapshot::secondsToTicks(double target) const {
    double sec = 0.0, prev = 0.0, bpm = tempo.front().bpm;
    for (size_t i = 1; i < tempo.size(); ++i) {
        const double seg = (tempo[i].tick - prev) * 60.0 / (bpm * PPQ);
        if (sec + seg > target) break;
        sec += seg; prev = tempo[i].tick; bpm = tempo[i].bpm;
    }
    return prev + (target - sec) * bpm * PPQ / 60.0;
}
static int tpb(const TimeSigPoint& t) { return t.num * PPQ * 4 / std::max(1, t.den); }
int Snapshot::barToTick(int bar) const {
    int tick = 0, cur = 0;
    TimeSigPoint ts = timeSig.front();
    for (size_t i = 1; i < timeSig.size() && timeSig[i].bar < bar; ++i) { tick += (timeSig[i].bar - cur) * tpb(ts); cur = timeSig[i].bar; ts = timeSig[i]; }
    return tick + (bar - cur) * tpb(ts);
}
int Snapshot::tickToBar(int tick) const {
    int t0 = 0, cur = 0;
    TimeSigPoint ts = timeSig.front();
    for (size_t i = 1; i < timeSig.size(); ++i) {
        const int next = t0 + (timeSig[i].bar - cur) * tpb(ts);
        if (next > tick) break;
        t0 = next; cur = timeSig[i].bar; ts = timeSig[i];
    }
    return cur + (tick - t0) / std::max(1, tpb(ts));
}
int Snapshot::ticksPerBarAt(int tick) const {
    const int bar = tickToBar(tick);
    TimeSigPoint r = timeSig.front();
    for (auto& t : timeSig) { if (t.bar <= bar) r = t; else break; }
    return tpb(r);
}
int Snapshot::beatTicksAt(int tick) const {
    const int bar = tickToBar(tick);
    TimeSigPoint r = timeSig.front();
    for (auto& t : timeSig) { if (t.bar <= bar) r = t; else break; }
    return PPQ * 4 / std::max(1, r.den);
}

// =================================================================== WSOLA alignment
std::shared_ptr<const std::vector<float>> grainShifts(const SamplePtr& smp, double offsetSec, double speed, double pitch,
                                                      bool reverse, bool loop, double clipDur) {
    static std::mutex m;
    static std::map<juce::String, std::shared_ptr<const std::vector<float>>> cache;
    static std::map<const SampleData*, std::shared_ptr<std::vector<float>>> mono;   // decimated mono copies
    const juce::String key = juce::String::toHexString((juce::pointer_sized_int)smp.get()) + "|" + juce::String(offsetSec, 5) + "|" + juce::String(speed, 6)
                             + "|" + juce::String(pitch, 6) + (reverse ? "r" : "") + (loop ? "l" : "") + "|" + juce::String(clipDur, 3);
    std::lock_guard<std::mutex> g(m);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const SampleData& s = *smp;
    const int dec = std::max(1, (int)std::lround(s.sampleRate / 11025.0));
    const double fs = s.sampleRate / dec;
    auto& mp = mono[smp.get()];
    if (!mp) {
        mp = std::make_shared<std::vector<float>>((size_t)(s.frames() / dec + 1), 0.f);
        for (int i = 0, j = 0; i + dec <= s.frames(); i += dec, ++j) {
            float acc = 0.f;
            for (int k = 0; k < dec; ++k) for (int c = 0; c < s.buffer.getNumChannels(); ++c) acc += s.buffer.getSample(c, i + k);
            (*mp)[(size_t)j] = acc / (float)(dec * s.buffer.getNumChannels());
        }
    }
    const auto& x = *mp;
    const double srcLen = s.seconds();
    auto at = [&](double t) -> float {   // same logical->physical mapping as the renderer
        if (loop) { const double span = srcLen - offsetSec; if (span > 0.001 && t >= srcLen) t = offsetSec + std::fmod(t - offsetSec, span); }
        if (t < 0.0 || t >= srcLen) return 0.f;
        if (reverse) t = srcLen - t;
        const int i = (int)(t * fs);
        return i >= 0 && i < (int)x.size() ? x[(size_t)i] : 0.f;
    };
    const int grains = (int)std::min(20000.0, std::ceil(clipDur / kGrainHop) + 2.0);
    auto out = std::make_shared<std::vector<float>>((size_t)grains, 0.f);
    const double D = 0.010, W = 0.012, dt = 1.0 / fs;
    const int cand = (int)(2 * D * fs), win = (int)(W * fs);
    std::vector<float> ref((size_t)win);
    for (int k = 1; k < grains; ++k) {
        const double prevStart = offsetSec + (k - 1) * kGrainHop * speed + (*out)[(size_t)k - 1];
        const double target = prevStart + kGrainHop * pitch;    // where the previous grain naturally continues
        const double nominal = offsetSec + k * kGrainHop * speed;
        for (int j = 0; j < win; ++j) ref[(size_t)j] = at(target + j * dt);
        double best = -1e30; int bestC = cand / 2;
        for (int c = 0; c <= cand; ++c) {
            const double st = nominal - D + c * dt;
            double acc = 0.0, en = 1e-9;
            for (int j = 0; j < win; ++j) { const float v = at(st + j * dt); acc += ref[(size_t)j] * v; en += v * v; }
            const double score = acc / std::sqrt(en);
            if (score > best) { best = score; bestC = c; }
        }
        (*out)[(size_t)k] = (float)(-D + bestC * dt);
    }
    if (cache.size() > 48) cache.clear();
    cache[key] = out;
    return out;
}

// =================================================================== compiler
namespace Compiler {

uint32_t uidFor(const juce::String& id) { return (uint32_t)id.hashCode() | 1u; }

static SamplePtr sampleFor(const juce::String& ref, bool blocking) {
    if (ref.isEmpty()) return nullptr;
    return blocking ? SampleBank::get().loadNow(ref) : SampleBank::get().request(ref);
}

static void compileNotes(const Project& p, const Track& t, std::vector<CompiledNote>& out) {
    for (auto& c : t.clips) {
        if (c.muted) continue;
        if (c.type == ClipType::Midi) {
            const int loopLen = std::max(1, c.contentLength - c.contentOffset);
            for (int rep = 0; rep * loopLen < c.length; ++rep)
                for (auto& n : c.notes) {
                    if (n.start < c.contentOffset || n.start >= c.contentLength) continue;
                    const int pos = (n.start - c.contentOffset) + rep * loopLen;
                    if (pos >= c.length) continue;
                    const int s = c.start + pos;
                    const int e = std::min(c.end(), s + std::max(1, n.length));
                    out.push_back({ s, e, n.pitch, n.velocity, 0.f });
                }
        } else if (c.type == ClipType::Pattern) {
            const Pattern* pat = p.findPattern(c.patternId);
            if (!pat) continue;
            const int plen = std::max(1, pat->lengthTicks());
            const uint32_t clipHash = (uint32_t)c.id.hashCode();
            for (int rep = 0; rep * plen < c.length; ++rep)
                for (int pad = 0; pad < kDrumPads; ++pad)
                    for (int s = 0; s < pat->steps; ++s) {
                        const auto& st = pat->grid[(size_t)pad][(size_t)s];
                        if (!st.on) continue;
                        int pos = rep * plen + s * pat->stepTicks;
                        if (s & 1) pos += (int)(pat->swing * pat->stepTicks);
                        const int rat = juce::jlimit(1, 4, st.ratchet);
                        const int sub = pat->stepTicks / rat;
                        for (int r = 0; r < rat; ++r) {
                            const int at = pos + r * sub;
                            if (at >= c.length) continue;
                            if (st.probability < 0.999f && hash01(clipHash, (uint32_t)rep, (uint32_t)(pad * 1024 + s * 4 + r)) >= st.probability) continue;
                            const int start = c.start + at;
                            out.push_back({ start, start + std::max(1, sub - 1), 36 + pad, st.velocity, (float)st.pitch });
                        }
                    }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const CompiledNote& a, const CompiledNote& b) { return a.start < b.start; });
}

static void compileAudio(const Project& p, const Track& t, std::vector<CompiledAudioClip>& out, bool blocking) {
    for (auto& c : t.clips) {
        if (c.muted || c.type != ClipType::Audio || c.audio.file.isEmpty()) continue;
        auto smp = sampleFor(c.audio.file, blocking);
        if (!smp) continue;
        CompiledAudioClip a;
        a.sample = smp;
        a.startTick = c.start; a.endTick = c.end();
        a.startSec = p.ticksToSeconds(c.start);
        a.endSec = p.ticksToSeconds(c.end());
        a.offsetSec = c.audio.offsetSec;
        float g = db2lin(c.audio.gainDb);
        if (c.audio.normalize && smp->peak > 1.0e-5f) g *= 0.891f / smp->peak;
        a.gain = g;
        a.fadeIn = c.audio.fadeInSec; a.fadeOut = c.audio.fadeOutSec;
        a.reverse = c.audio.reverse; a.loop = c.audio.loop; a.keepPitch = c.audio.keepPitch;
        a.pitchRatio = std::pow(2.f, c.audio.pitchSt / 12.f);
        a.stretch = juce::jlimit(0.25f, 4.f, c.audio.stretch);
        if (a.keepPitch && (std::fabs(a.stretch - 1.f) > 1.0e-3f || std::fabs(a.pitchRatio - 1.f) > 1.0e-3f))
            a.grainShift = grainShifts(smp, a.offsetSec, 1.0 / a.stretch, a.pitchRatio, a.reverse, a.loop, a.endSec - a.startSec);
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.startTick < b.startTick; });
}

static void resolveLane(const Track& t, const AutomationLane& l, CompiledLane& cl) {
    cl.lane = l;
    const auto tgt = l.target;
    if (tgt.startsWith("ch:")) { cl.params = t.channel; }
    else if (tgt.startsWith("inst:")) { cl.params = t.inst.params; }
    else if (tgt.startsWith("fx:")) {
        const auto fxId = tgt.fromFirstOccurrenceOf("fx:", false, false).upToFirstOccurrenceOf(":", false, false);
        for (auto& e : t.effects) if (e.id == fxId) cl.params = e.params;
    }
    if (cl.params) cl.index = cl.params->indexOf(tgt.fromLastOccurrenceOf(":", false, false));
}

static void buildChannelAndFx(const Track& t, TrackRender& tr, ProcRegistry& reg, double sr, std::set<juce::String>& used) {
    auto& ch = reg.channels[t.id];
    if (!ch) {
        ch = std::make_shared<ChannelProc>();
        ch->gl.prepare((float)sr, 15.f); ch->gr.prepare((float)sr, 15.f);
    }
    used.insert("ch:" + t.id);
    tr.ch = ch;
    tr.channel = t.channel;
    auto build = [&](const std::vector<EffectData>& chain, std::vector<std::shared_ptr<FxSlot>>& out) {
        for (auto& e : chain) {
            auto& slot = reg.effects[e.id];
            if (!slot || slot->type != e.type || slot->paramsPtr != e.params.get()) {
                slot = std::make_shared<FxSlot>();
                slot->fx = createEffect(e.type, e.params);
                slot->type = e.type;
                slot->id = e.id;
                slot->paramsPtr = e.params.get();
                if (slot->fx) slot->fx->prepare(sr, kQuantum);
                slot->wet = e.bypass->load() ? 0.f : 1.f;
            }
            slot->bypass = e.bypass;
            used.insert("fx:" + e.id);
            if (slot->fx) out.push_back(slot);
        }
    };
    build(t.effects, tr.fx);
    if (t.type == TrackType::Audio) { build(t.inputEffects, tr.inFx); tr.input = t.input; }
    for (auto& l : t.automation) {
        if (!l.enabled || l.points.empty()) continue;
        CompiledLane cl;
        resolveLane(t, l, cl);
        if (cl.params && cl.index >= 0) tr.lanes.push_back(std::move(cl));
    }
}

std::unique_ptr<Snapshot> compile(const Project& p, ProcRegistry& reg, double sr, bool blocking) {
    if (reg.sampleRate != sr) { reg.clear(); reg.sampleRate = sr; }
    auto s = std::make_unique<Snapshot>();
    s->tempo = p.tempo; s->timeSig = p.timeSig;
    s->loopOn = p.loopOn; s->loopStart = p.loopStart; s->loopEnd = std::max(p.loopStart + PPQ / 4, p.loopEnd);
    // Preview mode: only one pattern/clip plays, looped from tick 0.
    const bool preview = p.previewTrack.isNotEmpty();
    Project previewProj;
    if (preview) {
        if (const Track* src = p.findTrack(p.previewTrack)) {
            Clip pc;
            if (p.previewPattern.isNotEmpty()) {
                if (const Pattern* pat = p.findPattern(p.previewPattern)) {
                    pc.id = "preview"; pc.type = ClipType::Pattern; pc.patternId = pat->id; pc.length = pat->lengthTicks();
                    previewProj.patterns.push_back(*pat);
                }
            } else for (auto& c : src->clips) if (c.id == p.previewClip) { pc = c; }
            pc.start = 0;
            pc.muted = false;
            s->loopOn = true; s->loopStart = 0; s->loopEnd = std::max(PPQ / 4, pc.length);
            previewProj.tracks.push_back(Track());
            previewProj.tracks.back().clips.push_back(pc);
        }
    }
    s->sampleRate = sr;
    std::set<juce::String> used;
    std::map<juce::String, int> index;
    const int n = std::min((int)p.tracks.size(), kMaxTracks - 1);
    s->tracks.resize((size_t)n);
    for (int i = 0; i < n; ++i) index[p.tracks[(size_t)i].id] = i;

    for (int i = 0; i < n; ++i) {
        const Track& t = p.tracks[(size_t)i];
        TrackRender& tr = s->tracks[(size_t)i];
        tr.id = t.id; tr.uid = uidFor(t.id); tr.type = t.type;
        tr.armed = t.armed; tr.inputCh = t.inputChannel; tr.stereoIn = t.stereoInput;
        buildChannelAndFx(t, tr, reg, sr, used);
        if (t.hasInstrument() && t.inst.params) {
            InstrumentSetup setup;
            setup.engine = t.inst.engine;
            setup.params = t.inst.params;
            juce::String sig = t.inst.engine;
            if (t.inst.engine == "sampler") {
                setup.sample = sampleFor(t.inst.sample, blocking);
                setup.sampleRoot = t.inst.sampleRoot;
                sig << "|" << t.inst.sample << (setup.sample ? "+" : "-") << t.inst.sampleRoot;
            } else if (t.inst.engine == "drums") {
                for (int k = 0; k < kDrumPads; ++k) {
                    setup.padSamples[(size_t)k] = sampleFor(t.inst.pads[(size_t)k].sample, blocking);
                    setup.padParams[(size_t)k] = t.inst.pads[(size_t)k].params;
                    sig << "|" << t.inst.pads[(size_t)k].sample << (setup.padSamples[(size_t)k] ? "+" : "-");
                }
            }
            sig << "@" << juce::String::toHexString((juce::pointer_sized_int)t.inst.params.get());
            auto& ip = reg.instruments[t.id];
            if (!ip || ip->signature != sig) {
                ip = std::make_shared<InstrumentProc>();
                ip->inst = createInstrument(setup);
                ip->inst->prepare(sr, kQuantum);
                ip->signature = sig;
            }
            ip->arp = t.inst.arp;
            used.insert("inst:" + t.id);
            tr.inst = ip;
            if (preview) { if (t.id == p.previewTrack && !previewProj.tracks.empty()) compileNotes(previewProj, previewProj.tracks[0], tr.notes); }
            else compileNotes(p, t, tr.notes);
        }
        if (t.type == TrackType::Audio) {
            if (!preview) compileAudio(p, t, tr.clips, blocking);
            else if (t.id == p.previewTrack && !previewProj.tracks.empty()) compileAudio(p, previewProj.tracks[0], tr.clips, blocking);
        }
        for (auto& c : t.clips) s->songEnd = std::max(s->songEnd, c.end());
    }

    // routing: outputs (with cycle protection), sends to returns
    std::vector<int> returns;
    for (int i = 0; i < n; ++i) if (s->tracks[(size_t)i].type == TrackType::Return) returns.push_back(i);
    for (int i = 0; i < n; ++i) {
        auto& tr = s->tracks[(size_t)i];
        const Track& t = p.tracks[(size_t)i];
        int out = -1;
        auto it = index.find(t.output);
        if (it != index.end() && s->tracks[(size_t)it->second].type == TrackType::Bus && it->second != i && tr.type != TrackType::Return) out = it->second;
        // walk to detect cycles
        int cur = out, guard = 0;
        while (cur >= 0 && guard++ < kMaxTracks) {
            if (cur == i) { out = -1; break; }
            auto jt = index.find(p.tracks[(size_t)cur].output);
            cur = (jt != index.end() && s->tracks[(size_t)jt->second].type == TrackType::Bus) ? jt->second : -1;
        }
        tr.out = out;
        if (tr.type != TrackType::Return)
            for (int k = 0; k < 4; ++k) tr.sends[k] = k < (int)returns.size() ? returns[(size_t)k] : -1;
    }
    // processing order: sources, then buses (children first), then returns
    std::vector<int> order;
    std::vector<bool> done((size_t)n, false);
    for (int i = 0; i < n; ++i) if (s->tracks[(size_t)i].type != TrackType::Bus && s->tracks[(size_t)i].type != TrackType::Return) { order.push_back(i); done[(size_t)i] = true; }
    for (int pass = 0; pass < n; ++pass) {
        bool progress = false;
        for (int i = 0; i < n; ++i) {
            if (done[(size_t)i] || s->tracks[(size_t)i].type != TrackType::Bus) continue;
            bool ready = true;
            for (int j = 0; j < n; ++j) if (!done[(size_t)j] && s->tracks[(size_t)j].out == i) ready = false;
            if (ready) { order.push_back(i); done[(size_t)i] = true; progress = true; }
        }
        if (!progress) break;
    }
    for (int i = 0; i < n; ++i) if (!done[(size_t)i] && s->tracks[(size_t)i].type == TrackType::Bus) order.push_back(i);
    for (int i : returns) order.push_back(i);
    s->order = order;
    // solo links: self, ancestors (buses it feeds) and descendants (tracks feeding it)
    for (int i = 0; i < n; ++i) {
        auto& links = s->tracks[(size_t)i].soloLinks;
        links.push_back(i);
        for (int a = s->tracks[(size_t)i].out, g = 0; a >= 0 && g < kMaxTracks; a = s->tracks[(size_t)a].out, ++g) links.push_back(a);
        for (int j = 0; j < n; ++j) {
            if (j == i) continue;
            for (int a = s->tracks[(size_t)j].out, g = 0; a >= 0 && g < kMaxTracks; a = s->tracks[(size_t)a].out, ++g)
                if (a == i) { links.push_back(j); break; }
        }
    }
    // master
    s->master.id = "master"; s->master.uid = uidFor("master"); s->master.type = TrackType::Master;
    buildChannelAndFx(p.master, s->master, reg, sr, used);

    // drop processors no longer referenced by the project (old snapshot may still hold them)
    for (auto it = reg.instruments.begin(); it != reg.instruments.end();) it = used.count("inst:" + it->first) ? std::next(it) : reg.instruments.erase(it);
    for (auto it = reg.effects.begin(); it != reg.effects.end();) it = used.count("fx:" + it->first) ? std::next(it) : reg.effects.erase(it);
    for (auto it = reg.channels.begin(); it != reg.channels.end();) it = used.count("ch:" + it->first) ? std::next(it) : reg.channels.erase(it);
    return s;
}
}  // namespace Compiler

// =================================================================== renderer
Renderer::Renderer() = default;

void Renderer::prepare(double sampleRate) {
    sr = sampleRate;
    auto makeClick = [this](float hz, float amp) {
        std::vector<float> v((size_t)(sr * 0.04));
        for (size_t i = 0; i < v.size(); ++i) {
            const float t = (float)i / (float)sr;
            v[i] = std::sin(kTwoPi * hz * t) * std::exp(-t / 0.008f) * amp;
        }
        return v;
    };
    clickHi = makeClick(1760.f, 0.9f);
    clickLo = makeClick(1175.f, 0.6f);
    clickCur = nullptr;
}

void Renderer::setSnapshot(Snapshot* s) {
    snap = s;
    if (snap) for (auto& t : snap->tracks) if (t.inst) t.inst->inst->setTransport(&transport);
}

void Renderer::play() { if (!playing) { playing = true; needsRelease = false; } }

void Renderer::allNotesOff(bool hard) {
    if (!snap) return;
    for (auto& t : snap->tracks)
        if (t.inst) {
            t.inst->inst->allNotesOff(hard);
            t.inst->numActive = 0; t.inst->numHeld = 0; t.inst->arpNote = -1;
        }
}

void Renderer::stop() {
    if (!playing) return;
    playing = false;
    allNotesOff(false);
    if (snap) {
        auto clear = [](TrackRender& t) { for (auto& l : t.lanes) l.params->clearOverrides(); };
        for (auto& t : snap->tracks) clear(t);
        clear(snap->master);
    }
    countInLeft = 0;
}

void Renderer::startCountIn(int beats) {
    if (!snap || beats <= 0) { play(); return; }
    const double bpm = snap->bpmAt(curTick);
    countInBeatLen = std::max(1, (int)std::lround(sr * 60.0 / bpm * snap->beatTicksAt((int)curTick) / PPQ));
    countInLeft = beats * countInBeatLen;
    countInPos = 0;
    countInBeatsPerBar = std::max(1, snap->ticksPerBarAt((int)curTick) / std::max(1, snap->beatTicksAt((int)curTick)));
}

void Renderer::seek(double tick) {
    curTick = std::max(0.0, tick);
    curSec = snap ? snap->ticksToSeconds(curTick) : 0.0;
    if (snap) for (auto& t : snap->tracks) if (t.inst) releaseSequenced(t, 0);
}

void Renderer::releaseSequenced(TrackRender& t, int) {
    auto& ip = *t.inst;
    for (int i = 0; i < ip.numActive; ++i) ip.inst->noteOff(ip.active[(size_t)i].pitch);
    ip.numActive = 0;
    for (int i = 0; i < ip.numHeld; ++i) if (ip.heldEnd[(size_t)i] >= 0.0) { ip.held[(size_t)i] = -1; }
    int k = 0;
    for (int i = 0; i < ip.numHeld; ++i) if (ip.held[(size_t)i] >= 0) { ip.held[(size_t)k] = ip.held[(size_t)i]; ip.heldEnd[(size_t)k] = ip.heldEnd[(size_t)i]; ++k; }
    ip.numHeld = k;
    if (ip.arpNote >= 0 && k == 0) { ip.inst->noteOff(ip.arpNote); ip.arpNote = -1; }
}

bool Renderer::trackAudible(const TrackRender& t, bool anySolo) const {
    if (!anySolo || t.type == TrackType::Return) return true;
    static const int soloIdx = 3;
    for (int k : t.soloLinks) if (snap->tracks[(size_t)k].channel->getBool(soloIdx)) return true;
    return false;
}

// chord intervals for the chord-trigger MIDI effect
static int chordNotes(int mode, int root, int* out) {
    static const int ch[8][4] = { { 0 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 7, 12 }, { 0, 5, 7 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 12 } };
    static const int cnt[8] = { 1, 3, 3, 3, 3, 4, 4, 2 };
    mode = juce::jlimit(0, 7, mode);
    for (int i = 0; i < cnt[mode]; ++i) out[i] = juce::jlimit(0, 127, root + ch[mode][i]);
    return cnt[mode];
}

void Renderer::handleLive(const LiveEvent& e) {
    if (!snap) return;
    for (auto& t : snap->tracks) {
        if (t.uid != e.trackUid || !t.inst) continue;
        auto& ip = *t.inst;
        auto& ins = *ip.inst;
        const bool arpOn = ip.arp && ip.arp->getBool(0);
        const int chordMode = ip.arp ? ip.arp->getInt(5) : 0;
        int notes[4];
        switch (e.type) {
            case LiveEvent::NoteOn: {
                const int c = chordNotes(chordMode, e.note, notes);
                for (int k = 0; k < c; ++k) {
                    if (arpOn) { if (ip.numHeld < 64) { ip.held[(size_t)ip.numHeld] = notes[k]; ip.heldEnd[(size_t)ip.numHeld] = -1.0; ++ip.numHeld; } }
                    else ins.noteOn(notes[k], e.value);
                }
                if (midiRecording && playing && t.armed) recordedNotes.push({ t.uid, e.note, e.value, curTick, true });
                break;
            }
            case LiveEvent::NoteOff: {
                const int c = chordNotes(chordMode, e.note, notes);
                for (int k = 0; k < c; ++k) {
                    if (arpOn) {
                        for (int i = 0; i < ip.numHeld; ++i)
                            if (ip.held[(size_t)i] == notes[k] && ip.heldEnd[(size_t)i] < 0.0) {
                                for (int j = i; j < ip.numHeld - 1; ++j) { ip.held[(size_t)j] = ip.held[(size_t)j + 1]; ip.heldEnd[(size_t)j] = ip.heldEnd[(size_t)j + 1]; }
                                --ip.numHeld; break;
                            }
                    } else ins.noteOff(notes[k]);
                }
                if (midiRecording && playing && t.armed) recordedNotes.push({ t.uid, e.note, 0.f, curTick, false });
                break;
            }
            case LiveEvent::PitchBend: ins.setPitchBend(e.value); break;
            case LiveEvent::ModWheel: ins.setModWheel(e.value); break;
            case LiveEvent::Sustain: ins.setSustain(e.value >= 0.5f); break;
            case LiveEvent::AllOff: ins.allNotesOff(false); ip.numHeld = 0; break;
        }
    }
}

void Renderer::applyAutomation(TrackRender& t, double tick) {
    for (auto& l : t.lanes) {
        const float v = l.lane.valueAt((int)tick);
        l.params->setOverride(l.index, l.params->fromNorm(l.index, v));
    }
}

void Renderer::processInstrumentTrack(TrackRender& t, double t0, double t1, double tps, int n) {
    auto& ip = *t.inst;
    auto& ins = *ip.inst;
    float* L = bufL[0];  // placeholder, replaced below
    (void)L;
    struct Ev { int off; bool on; int pitch; float vel; float det; };
    Ev ev[192];
    int ne = 0;
    auto add = [&](int off, bool on, int pitch, float vel, float det) {
        if (ne < 192) ev[ne++] = { juce::jlimit(0, n - 1, off), on, pitch, vel, det };
    };
    const bool arpOn = ip.arp && ip.arp->getBool(0);
    const int chordMode = ip.arp ? ip.arp->getInt(5) : 0;
    if (playing) {
        // sequenced note-offs
        for (int i = 0; i < ip.numActive;) {
            auto& a = ip.active[(size_t)i];
            if (a.endTick < t1) {
                add((int)std::floor((a.endTick - t0) / tps), false, a.pitch, 0.f, 0.f);
                ip.active[(size_t)i] = ip.active[(size_t)--ip.numActive];
            } else ++i;
        }
        // sequenced note-ons (binary search for the first note at or after t0)
        auto& notes = t.notes;
        size_t lo = 0, hi = notes.size();
        while (lo < hi) { const size_t mid = (lo + hi) / 2; if ((double)notes[mid].start < t0) lo = mid + 1; else hi = mid; }
        for (size_t k = lo; k < notes.size() && (double)notes[k].start < t1; ++k) {
            const auto& nt = notes[k];
            const int off = (int)std::floor((nt.start - t0) / tps);
            add(off, true, nt.pitch, nt.velocity, nt.detune);
            if ((double)nt.end < t1) add(std::max(off, (int)std::floor((nt.end - t0) / tps)), false, nt.pitch, 0.f, 0.f);
            else if (ip.numActive < 256) ip.active[(size_t)ip.numActive++] = { nt.pitch, (double)nt.end };
        }
    }
    // chord trigger + arpeggiator (MIDI effects)
    if (chordMode > 0 || arpOn) {
        Ev in[192];
        const int nin = ne;
        std::copy(ev, ev + ne, in);
        ne = 0;
        for (int i = 0; i < nin; ++i) {
            int ch[4];
            const int c = chordNotes(chordMode, in[i].pitch, ch);
            for (int k = 0; k < c; ++k) {
                if (!arpOn) { add(in[i].off, in[i].on, ch[k], in[i].vel, in[i].det); continue; }
                if (in[i].on) { if (ip.numHeld < 64) { ip.held[(size_t)ip.numHeld] = ch[k]; ip.heldEnd[(size_t)ip.numHeld] = 1.0; ++ip.numHeld; } }
                else for (int h = 0; h < ip.numHeld; ++h)
                    if (ip.held[(size_t)h] == ch[k] && ip.heldEnd[(size_t)h] >= 0.0) {
                        for (int j = h; j < ip.numHeld - 1; ++j) { ip.held[(size_t)j] = ip.held[(size_t)j + 1]; ip.heldEnd[(size_t)j] = ip.heldEnd[(size_t)j + 1]; }
                        --ip.numHeld; break;
                    }
            }
        }
        if (arpOn) {
            const double rate = syncBeats(ip.arp->getInt(2)) * PPQ;
            const int octaves = juce::jlimit(1, 4, ip.arp->getInt(3));
            const double gate = ip.arp->get(4) * 0.01;
            const int mode = ip.arp->getInt(1);
            const double c0 = playing ? t0 : ip.arpClock;
            const double c1 = c0 + (t1 - t0);
            ip.arpClock = c1;
            if (ip.arpNote >= 0 && ip.arpOffTick >= c0 && ip.arpOffTick < c1) { add((int)((ip.arpOffTick - c0) / tps), false, ip.arpNote, 0.f, 0.f); ip.arpNote = -2 - ip.arpNote; }
            for (double b = std::ceil(c0 / rate) * rate; b < c1; b += rate) {
                const int off = (int)((b - c0) / tps);
                if (ip.arpNote >= 0) add(off, false, ip.arpNote, 0.f, 0.f);
                ip.arpNote = -1;
                if (ip.numHeld == 0) continue;
                // sorted copy of held notes
                int sorted[64]; const int nh = ip.numHeld;
                std::copy(ip.held.begin(), ip.held.begin() + nh, sorted);
                if (mode != 4) std::sort(sorted, sorted + nh);
                const int total = nh * octaves;
                int idx = 0;
                switch (mode) {
                    case 1: idx = total - 1 - (ip.arpStep % total); break;
                    case 2: { const int cyc = std::max(1, total * 2 - 2); const int s = ip.arpStep % cyc; idx = s < total ? s : cyc - s; break; }
                    case 3: ip.arpRng = ip.arpRng * 1664525u + 1013904223u; idx = (int)((ip.arpRng >> 8) % (uint32_t)total); break;
                    default: idx = ip.arpStep % total; break;
                }
                const int pitch = juce::jlimit(0, 127, sorted[idx % nh] + 12 * (idx / nh));
                add(off, true, pitch, 0.8f, 0.f);
                ip.arpNote = pitch;
                ip.arpOffTick = b + rate * gate;
                ++ip.arpStep;
                if (ip.arpOffTick < c1) { add((int)((ip.arpOffTick - c0) / tps), false, pitch, 0.f, 0.f); ip.arpNote = -2 - pitch; }
            }
            if (ip.arpNote <= -2) ip.arpNote = -1;   // already released inside this block
            if (ip.numHeld == 0 && ip.arpNote >= 0) { add(0, false, ip.arpNote, 0.f, 0.f); ip.arpNote = -1; ip.arpStep = 0; }
        }
    }
    // stable insertion sort by offset (note-offs before note-ons at the same offset)
    for (int i = 1; i < ne; ++i) {
        Ev v = ev[i]; int j = i - 1;
        while (j >= 0 && (ev[j].off > v.off || (ev[j].off == v.off && ev[j].on && !v.on))) { ev[j + 1] = ev[j]; --j; }
        ev[j + 1] = v;
    }
    const int ti = (int)(&t - snap->tracks.data());
    float* outL = bufL[ti];
    float* outR = bufR[ti];
    int pos = 0;
    for (int i = 0; i < ne; ++i) {
        if (ev[i].off > pos) { ins.render(outL + pos, outR + pos, ev[i].off - pos); pos = ev[i].off; }
        if (ev[i].on) ins.noteOn(ev[i].pitch, ev[i].vel, ev[i].det);
        else ins.noteOff(ev[i].pitch);
    }
    if (pos < n) ins.render(outL + pos, outR + pos, n - pos);
    const int v = ins.activeVoices();
    t.ch->voices.store(v);
    activeVoices += v;
}

// Stateless clip renderer: output depends only on song time, so seeking/looping is exact.
void Renderer::renderAudioClips(TrackRender& t, double sec0, int n, float* L, float* R) {
    const double dt = 1.0 / sr;
    const double sec1 = sec0 + n * dt;
    for (auto& c : t.clips) {
        if (c.endSec <= sec0 || c.startSec >= sec1) continue;
        const SampleData& s = *c.sample;
        const double srcLen = s.seconds();
        const double clipDur = c.endSec - c.startSec;
        const double speed = c.keepPitch ? 1.0 / c.stretch : (double)c.pitchRatio / c.stretch;
        constexpr double G = kGrainSec, H = kGrainHop;
        const std::vector<float>* shifts = c.grainShift.get();
        auto srcAt = [&](double srcSec, float& l, float& r) {
            double ss = srcSec;
            if (c.loop) {
                const double span = srcLen - c.offsetSec;
                if (span > 0.001 && ss >= srcLen) ss = c.offsetSec + std::fmod(ss - c.offsetSec, span);
            }
            if (ss < 0.0 || ss >= srcLen) { l = r = 0.f; return; }
            if (c.reverse) ss = srcLen - ss;
            s.read(ss * s.sampleRate, l, r);
        };
        for (int i = 0; i < n; ++i) {
            const double tt = sec0 + i * dt - c.startSec;   // time since clip start
            if (tt < 0.0 || tt >= clipDur) continue;
            float l = 0.f, r = 0.f;
            if (c.keepPitch && (std::fabs(c.stretch - 1.f) > 1.0e-3f || std::fabs(c.pitchRatio - 1.f) > 1.0e-3f)) {
                // two overlapping grains with sin^2 windows (sum to 1)
                const double k1 = std::floor(tt / H);
                for (int g = 0; g < 2; ++g) {
                    const double k = k1 - g;
                    if (k < 0) continue;
                    const double tau = tt - k * H;
                    if (tau >= G) continue;
                    const double w = std::sin(kPi * tau / G);
                    float gl, gr;
                    const double shift = shifts && k < (double)shifts->size() ? (*shifts)[(size_t)k] : 0.0;
                    srcAt(c.offsetSec + k * H * speed + shift + tau * c.pitchRatio, gl, gr);
                    l += gl * (float)(w * w); r += gr * (float)(w * w);
                }
            } else {
                srcAt(c.offsetSec + tt * speed, l, r);
            }
            float g = c.gain;
            if (c.fadeIn > 0.0 && tt < c.fadeIn) g *= (float)(tt / c.fadeIn);
            const double toEnd = clipDur - tt;
            if (c.fadeOut > 0.0 && toEnd < c.fadeOut) g *= (float)(toEnd / c.fadeOut);
            else if (toEnd < 0.002) g *= (float)(toEnd / 0.002);  // de-click at clip end
            if (tt < 0.001) g *= (float)(tt / 0.001);
            L[i] += l * g; R[i] += r * g;
        }
    }
}

int Renderer::inputChainLatency(const TrackRender& t) {
    int lat = 0;
    for (auto& s : t.inFx) if (s->fx && !(s->bypass && s->bypass->load())) lat += s->fx->latencySamples();
    return lat;
}

// Live input of an armed audio track: gain -> meters/tuner -> (input effects) -> monitor + recorder.
void Renderer::processInput(TrackRender& t, const float* const* in, int numIn, int offset, int n, float* L, float* R, double t0) {
    const int c0 = juce::jlimit(0, numIn - 1, t.inputCh);
    const int c1 = t.stereoIn ? juce::jlimit(0, numIn - 1, t.inputCh + 1) : c0;
    if (!in[c0] || !in[c1] || !t.input) return;
    const auto& ip = *t.input;
    const float gain = db2lin(ip.get(0));
    float pkL = 0.f, pkR = 0.f;
    auto& cp = *t.ch;
    int rp = cp.inRingPos.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        inL[i] = in[c0][offset + i] * gain;
        inR[i] = in[c1][offset + i] * gain;
        pkL = std::max(pkL, std::fabs(inL[i])); pkR = std::max(pkR, std::fabs(inR[i]));
        cp.inRing[(size_t)rp].store(0.5f * (inL[i] + inR[i]), std::memory_order_relaxed);
        rp = (rp + 1) & (ChannelProc::kRing - 1);
    }
    cp.inRingPos.store(rp, std::memory_order_release);
    if (pkL > cp.inPeakL.load(std::memory_order_relaxed)) cp.inPeakL.store(pkL, std::memory_order_relaxed);
    if (pkR > cp.inPeakR.load(std::memory_order_relaxed)) cp.inPeakR.store(pkR, std::memory_order_relaxed);
    const int mode = ip.getInt(1);
    const bool print = ip.getInt(3) == 1;
    const bool rec = playing && onTrackInput && t0 >= punchInTick;
    if (rec && !print) onTrackInput(t.uid, inL, inR, n);
    if (mode == Track::MonitorThrough || print) runInserts(t.inFx, inL, inR, n);
    if (rec && print) onTrackInput(t.uid, inL, inR, n);
    if (mode == Track::MonitorThrough) {
        const float mg = db2lin(ip.get(2));
        for (int i = 0; i < n; ++i) { L[i] += inL[i] * mg; R[i] += inR[i] * mg; }
    }
}

void Renderer::runInserts(std::vector<std::shared_ptr<FxSlot>>& chain, float* L, float* R, int n) {
    // inserts with click-free bypass crossfade
    float dl[kQuantum], dr[kQuantum];
    for (auto& slotPtr : chain) {
        auto& slot = *slotPtr;
        const float target = slot.bypass && slot.bypass->load(std::memory_order_relaxed) ? 0.f : 1.f;
        if (slot.wet <= 0.f && target <= 0.f) continue;
        std::copy(L, L + n, dl); std::copy(R, R + n, dr);
        slot.fx->setTransport(&transport);
        slot.fx->process(L, R, n);
        if (slot.wet != target || slot.wet < 1.f) {
            const float inc = (target - slot.wet) / (float)n;
            for (int i = 0; i < n; ++i) {
                const float w = slot.wet + inc * (float)(i + 1);
                L[i] = dl[i] + (L[i] - dl[i]) * w;
                R[i] = dr[i] + (R[i] - dr[i]) * w;
            }
            slot.wet = target;
        }
        // NaN/denormal guard: a misbehaving effect must not poison the mix
        if (!std::isfinite(L[0]) || !std::isfinite(R[n - 1])) { std::fill(L, L + n, 0.f); std::fill(R, R + n, 0.f); slot.fx->reset(); }
    }
}

void Renderer::channelStrip(TrackRender& t, float* L, float* R, int n, bool audible) {
    runInserts(t.fx, L, R, n);
    const auto& ch = *t.channel;
    if (ch.getBool(4)) for (int i = 0; i < n; ++i) { L[i] = -L[i]; R[i] = -R[i]; }        // phase
    if (ch.getBool(5)) for (int i = 0; i < n; ++i) { const float m = 0.5f * (L[i] + R[i]); L[i] = R[i] = m; }  // mono
    auto sendTo = [&](int target, float levelDb) {
        if (target < 0 || levelDb <= -59.9f) return;
        const float g = db2lin(levelDb);
        auto& bus = *snap->tracks[(size_t)target].ch;
        for (int i = 0; i < n; ++i) { bus.busL[i] += L[i] * g; bus.busR[i] += R[i] * g; }
    };
    for (int k = 0; k < 4; ++k) if (ch.getBool(10 + k) && audible && !ch.getBool(2)) sendTo(t.sends[k], ch.get(6 + k));
    const float volDb = ch.get(0);
    float vol = volDb <= -59.9f ? 0.f : db2lin(volDb);
    if (!audible || ch.getBool(2)) vol = 0.f;
    const float pan = ch.get(1) * 0.01f;
    const float a = (pan + 1.f) * kPi * 0.25f;
    auto& cp = *t.ch;
    cp.gl.setTarget(vol * std::cos(a) * 1.41421356f);
    cp.gr.setTarget(vol * std::sin(a) * 1.41421356f);
    float pkL = 0.f, pkR = 0.f;
    for (int i = 0; i < n; ++i) {
        L[i] *= cp.gl.next(); R[i] *= cp.gr.next();
        pkL = std::max(pkL, std::fabs(L[i])); pkR = std::max(pkR, std::fabs(R[i]));
    }
    for (int k = 0; k < 4; ++k) if (!ch.getBool(10 + k)) sendTo(t.sends[k], ch.get(6 + k));
    if (pkL > cp.peakL.load(std::memory_order_relaxed)) cp.peakL.store(pkL, std::memory_order_relaxed);
    if (pkR > cp.peakR.load(std::memory_order_relaxed)) cp.peakR.store(pkR, std::memory_order_relaxed);
    if (pkL > 1.f || pkR > 1.f) cp.clipped.store(true, std::memory_order_relaxed);
}

void Renderer::render(float* outL, float* outR, int n, const float* const* in, int numIn) {
    loopWrapped = false;
    // battery saver: wake on anything that can make sound
    bool monitoring = false;
    if (snap && in && numIn > 0)
        for (auto& t : snap->tracks) monitoring |= t.armed && t.input && t.input->getInt(1) == Track::MonitorThrough;
    const bool busy = playing || countInLeft > 0 || previewSample.load(std::memory_order_relaxed) != nullptr || monitoring;
    if (idle) {
        if (busy || liveQueue.size() > 0 || midiQueue.size() > 0) { idle = false; silentSeconds = 0.0; }
        else { std::fill(outL, outL + n, 0.f); std::fill(outR, outR + n, 0.f); return; }
    }
    activeVoices = 0;
    int done = 0;
    if (countInLeft > 0) {
        // song waits; clicks on each beat; playback starts exactly when the count-in ends
        const int len = std::min(n, countInLeft);
        for (int pos = 0; pos < len;) {
            const int sub = std::min(kQuantum, len - pos);
            renderSub(pos, sub, in, numIn, outL, outR);
            pos += sub;
        }
        for (int i = 0; i < len; ++i, ++countInPos) {
            const int beatIdx = countInPos / countInBeatLen, inBeat = countInPos % countInBeatLen;
            const auto& c = (beatIdx % countInBeatsPerBar) == 0 ? clickHi : clickLo;
            if (inBeat < (int)c.size()) { outL[i] += c[(size_t)inBeat] * metronomeGain; outR[i] += c[(size_t)inBeat] * metronomeGain; }
        }
        countInLeft -= len;
        done = len;
        if (countInLeft <= 0) play();
    }
    while (done < n) {
        int len = std::min(kQuantum, n - done);
        if (playing && snap) {
            const double tps = snap->bpmAt(curTick + 1.0e-6) / 60.0 * PPQ / sr;
            if (snap->loopOn && curTick < snap->loopEnd) {
                const double toEnd = (snap->loopEnd - curTick) / tps;
                if (toEnd < len) len = std::max(1, (int)std::ceil(toEnd));
            }
            // never let a sub-block straddle a tempo change (keeps timing exact)
            for (auto& tp : snap->tempo)
                if (tp.tick > curTick + 1.0e-6) {
                    const double toTempo = (tp.tick - curTick) / tps;
                    if (toTempo < len) len = std::max(1, (int)std::ceil(toTempo));
                    break;
                }
        }
        renderSub(done, len, in, numIn, outL, outR);
        done += len;
        if (playing && snap && snap->loopOn && curTick >= snap->loopEnd - 1.0e-6 && curTick - snap->loopEnd < PPQ * 4) {
            seek(snap->loopStart);
            loopWrapped = true;
        }
    }
    if (!busy && activeVoices == 0 && idleSleepEnabled.load(std::memory_order_relaxed)) {
        float pk = 0.f;
        for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(outL[i]), std::fabs(outR[i])));
        silentSeconds = pk < 1.0e-5f ? silentSeconds + n / sr : 0.0;   // wait for reverb/delay tails to finish
        if (silentSeconds > 2.0) idle = true;
    } else silentSeconds = 0.0;
}

void Renderer::renderSub(int offset, int n, const float* const* in, int numIn, float* outL, float* outR) {
    float* oL = outL + offset;
    float* oR = outR + offset;
    if (!snap) { std::fill(oL, oL + n, 0.f); std::fill(oR, oR + n, 0.f); return; }
    LiveEvent e;
    while (liveQueue.pop(e)) handleLive(e);
    while (midiQueue.pop(e)) handleLive(e);

    const double bpm = snap->bpmAt(curTick + 1.0e-6);   // tolerance: float drift must not delay a tempo change
    const double tps = bpm / 60.0 * PPQ / sr;
    const double t0 = curTick, t1 = curTick + n * tps;
    transport.bpm = bpm; transport.tick = curTick; transport.playing = playing;

    bool anySolo = false;
    for (auto& t : snap->tracks) if (t.channel->getBool(3)) { anySolo = true; break; }
    std::fill(mixL, mixL + n, 0.f);
    std::fill(mixR, mixR + n, 0.f);

    for (int idx : snap->order) {
        auto& t = snap->tracks[(size_t)idx];
        float* L = bufL[idx];
        float* R = bufR[idx];
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
        if (playing) applyAutomation(t, t0);
        if (t.inst) {
            processInstrumentTrack(t, t0, t1, tps, n);
            auto& cp0 = *t.ch;
            int sp = cp0.scopePos.load(std::memory_order_relaxed);
            for (int i = 0; i < n; ++i) { cp0.scope[(size_t)sp].store(0.5f * (L[i] + R[i]), std::memory_order_relaxed); sp = (sp + 1) & 2047; }
            cp0.scopePos.store(sp, std::memory_order_release);
        }
        if (t.type == TrackType::Audio) {
            if (playing) renderAudioClips(t, curSec, n, L, R);
            if (t.armed && in && numIn > 0) processInput(t, in, numIn, offset, n, L, R, t0);
        }
        auto& cp = *t.ch;
        if (t.type == TrackType::Bus || t.type == TrackType::Return) {
            for (int i = 0; i < n; ++i) { L[i] += cp.busL[i]; R[i] += cp.busR[i]; }
        }
        std::fill(cp.busL, cp.busL + n, 0.f);
        std::fill(cp.busR, cp.busR + n, 0.f);
        channelStrip(t, L, R, n, trackAudible(t, anySolo));
        float* dL = t.out >= 0 ? snap->tracks[(size_t)t.out].ch->busL : mixL;
        float* dR = t.out >= 0 ? snap->tracks[(size_t)t.out].ch->busR : mixR;
        for (int i = 0; i < n; ++i) { dL[i] += L[i]; dR[i] += R[i]; }
    }
    // master
    if (playing) applyAutomation(snap->master, t0);
    channelStrip(snap->master, mixL, mixR, n, true);
    for (int i = 0; i < n; ++i) { oL[i] = mixL[i]; oR[i] = mixR[i]; }

    // metronome (after master, real-time only)
    if (metronome && playing) {
        const int beat = snap->beatTicksAt((int)t0);
        const double next = std::ceil(t0 / beat) * beat;
        if (next < t1) {
            const int bar = snap->tickToBar((int)next);
            clickCur = (int)next == snap->barToTick(bar) ? &clickHi : &clickLo;
            clickPos = -(int)((next - t0) / tps);
        }
    }
    if (clickCur) {
        for (int i = 0; i < n; ++i, ++clickPos) {
            if (clickPos < 0) continue;
            if (clickPos >= (int)clickCur->size()) { clickCur = nullptr; break; }
            const float v = (*clickCur)[(size_t)clickPos] * metronomeGain;
            oL[i] += v; oR[i] += v;
        }
    }
    if (const SampleData* pv = previewSample.load(std::memory_order_acquire)) {
        const double rate = pv->sampleRate / sr;
        for (int i = 0; i < n; ++i) {
            if (previewPos >= pv->frames()) { previewSample.store(nullptr); break; }
            float l, r;
            pv->read(previewPos, l, r);
            previewPos += rate;
            oL[i] += l * 0.8f; oR[i] += r * 0.8f;
        }
    }
    if (playing) { curTick = t1; curSec += n / sr; }
}
