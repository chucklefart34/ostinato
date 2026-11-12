#include "Project.h"
#include "Engine/Catalog.h"

// ------------------------------------------------------------------ automation
float AutomationLane::valueAt(int tick) const {
    if (points.empty()) return 0.5f;
    if (tick <= points.front().tick) return points.front().value;
    if (tick >= points.back().tick) return points.back().value;
    // binary search for segment
    size_t lo = 0, hi = points.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (points[mid].tick <= tick) lo = mid; else hi = mid;
    }
    const auto& a = points[lo];
    const auto& b = points[hi];
    if (b.tick == a.tick) return b.value;
    float t = (float)(tick - a.tick) / (float)(b.tick - a.tick);
    switch (a.curve) {
        case 1: t = 0.5f - 0.5f * std::cos(t * 3.14159265f); break;  // smooth
        case 2: t = 0.f; break;                                       // hold
        default: break;
    }
    return a.value + (b.value - a.value) * t;
}

AutomationLane* Track::findLane(const juce::String& target) {
    for (auto& l : automation) if (l.target == target) return &l;
    return nullptr;
}

// ------------------------------------------------------------------ project
Project::Project() {
    master.id = "master";
    master.name = "Master";
    master.type = TrackType::Master;
    master.colour = juce::Colour(0xffe8eaed);
    master.channel = Catalog::makeParams(Catalog::channelParams());
    master.input = Catalog::makeParams(Catalog::inputParams());
    created = modified = juce::Time::getCurrentTime().toISO8601(true);
}

juce::String Project::newId(const char* prefix) { return juce::String(prefix) + juce::String(idCounter++); }

juce::Colour Project::trackColour(int i) {
    static const juce::uint32 c[] = { 0xffff6b6b, 0xffff9f43, 0xfffeca57, 0xff1dd1a1, 0xff48dbfb, 0xff54a0ff,
                                      0xff5f27cd, 0xffc56cf0, 0xffff9ff3, 0xff00d2d3, 0xff10ac84, 0xffee5253 };
    return juce::Colour(c[(size_t)std::abs(i) % 12]);
}

Track* Project::findTrack(const juce::String& id) {
    if (id == "master") return &master;
    for (auto& t : tracks) if (t.id == id) return &t;
    return nullptr;
}
const Track* Project::findTrack(const juce::String& id) const { return const_cast<Project*>(this)->findTrack(id); }
int Project::trackIndex(const juce::String& id) const {
    for (size_t i = 0; i < tracks.size(); ++i) if (tracks[i].id == id) return (int)i;
    return -1;
}
Pattern* Project::findPattern(const juce::String& id) {
    for (auto& p : patterns) if (p.id == id) return &p;
    return nullptr;
}
const Pattern* Project::findPattern(const juce::String& id) const { return const_cast<Project*>(this)->findPattern(id); }
std::vector<const Track*> Project::returnTracks() const {
    std::vector<const Track*> r;
    for (auto& t : tracks) if (t.type == TrackType::Return && r.size() < 4) r.push_back(&t);
    return r;
}

void Project::initInstrument(InstrumentData& inst, const juce::String& engine) {
    inst.engine = engine;
    inst.params = Catalog::makeParams(Catalog::engineParams(engine));
    if (!inst.arp) inst.arp = Catalog::makeParams(Catalog::arpParams());
    if (engine == "drums") {
        const auto& kit = Catalog::drumKits().front().second;
        for (int i = 0; i < kDrumPads; ++i) {
            auto& pad = inst.pads[(size_t)i];
            pad.params = Catalog::makeParams(Catalog::drumPadParams());
            pad.sample = "builtin:" + kit[i];
            pad.name = kit[i];
            for (auto& d : Catalog::drumSounds()) if (kit[i] == d.id) pad.name = d.name;
        }
        // open hat chokes closed hat
        inst.pads[3].params->set(inst.pads[3].params->indexOf("choke"), 1);
        inst.pads[4].params->set(inst.pads[4].params->indexOf("choke"), 1);
    }
}

static void applyKit(InstrumentData& inst, const juce::String& kitName) {
    for (auto& [name, sounds] : Catalog::drumKits()) {
        if (name != kitName) continue;
        for (int i = 0; i < kDrumPads; ++i) {
            auto& pad = inst.pads[(size_t)i];
            pad.sample = "builtin:" + sounds[i];
            pad.name = sounds[i];
            for (auto& d : Catalog::drumSounds()) if (sounds[i] == d.id) pad.name = d.name;
        }
    }
}

Track& Project::addTrack(TrackType type, const juce::String& engine, const juce::String& preset, int insertAt) {
    Track t;
    t.id = newId("t");
    t.type = type;
    t.colour = trackColour((int)tracks.size());
    t.channel = Catalog::makeParams(Catalog::channelParams());
    t.input = Catalog::makeParams(Catalog::inputParams());
    int count = 1;
    for (auto& o : tracks) if (o.type == type) ++count;
    switch (type) {
        case TrackType::Instrument: {
            const auto* p = Catalog::findPreset(preset.isEmpty() ? "Subtractive Synth" : preset);
            initInstrument(t.inst, p ? juce::String(p->engine) : (engine.isEmpty() ? juce::String("analog") : engine));
            if (p) { Catalog::applyValues(*t.inst.params, p->values); t.inst.preset = p->name; }
            t.name = p ? juce::String(p->name) : Catalog::engineName(t.inst.engine);
            break;
        }
        case TrackType::Drum: {
            initInstrument(t.inst, "drums");
            const juce::String kit = preset.isEmpty() ? juce::String("Boom Kit") : preset;
            applyKit(t.inst, kit);
            t.inst.preset = kit;
            t.name = "Drums";
            break;
        }
        case TrackType::Audio: t.name = "Audio " + juce::String(count); t.stereoInput = false; break;
        case TrackType::Bus: t.name = "Bus " + juce::String(count); break;
        case TrackType::Return: {
            static const char* names[] = { "A", "B", "C", "D" };
            t.name = "Return " + juce::String(names[juce::jlimit(0, 3, count - 1)]);
            break;
        }
        default: break;
    }
    // number only genuine duplicates ("Bass Synth", "Bass Synth 2")
    if (type == TrackType::Instrument || type == TrackType::Drum) {
        auto taken = [this](const juce::String& n) { for (auto& o : tracks) if (o.name == n) return true; return false; };
        if (taken(t.name)) { const auto base = t.name; for (int k = 2; taken(t.name); ++k) t.name = base + " " + juce::String(k); }
    }
    if (insertAt < 0 || insertAt > (int)tracks.size()) insertAt = (int)tracks.size();
    tracks.insert(tracks.begin() + insertAt, std::move(t));
    return tracks[(size_t)insertAt];
}

Pattern& Project::addPattern(const juce::String& nm) {
    Pattern p;
    p.id = newId("p");
    p.name = nm.isEmpty() ? juce::String("Pattern ") + juce::String((int)patterns.size() + 1) : nm;
    p.colour = trackColour((int)patterns.size() + 3);
    patterns.push_back(p);
    return patterns.back();
}

Clip Project::makeMidiClip(int start, int length) {
    Clip c;
    c.id = newId("c");
    c.type = ClipType::Midi;
    c.start = start;
    c.length = length;
    c.contentLength = length;
    c.name = "MIDI";
    return c;
}

EffectData Project::makeEffect(const juce::String& type) {
    EffectData e;
    e.id = newId("fx");
    e.type = type;
    e.params = Catalog::makeParams(Catalog::effectParams(type));
    return e;
}

// ------------------------------------------------------------------ time
double Project::bpmAt(int tick) const {
    double b = tempo.front().bpm;
    for (auto& t : tempo) { if (t.tick <= tick) b = t.bpm; else break; }
    return b;
}

double Project::ticksToSeconds(double tick) const {
    double sec = 0.0, prevTick = 0.0, bpm = tempo.front().bpm;
    for (size_t i = 1; i < tempo.size(); ++i) {
        if (tempo[i].tick >= tick) break;
        sec += (tempo[i].tick - prevTick) * 60.0 / (bpm * PPQ);
        prevTick = tempo[i].tick;
        bpm = tempo[i].bpm;
    }
    return sec + (tick - prevTick) * 60.0 / (bpm * PPQ);
}

double Project::secondsToTicks(double target) const {
    double sec = 0.0, prevTick = 0.0, bpm = tempo.front().bpm;
    for (size_t i = 1; i < tempo.size(); ++i) {
        const double segSec = (tempo[i].tick - prevTick) * 60.0 / (bpm * PPQ);
        if (sec + segSec > target) break;
        sec += segSec;
        prevTick = tempo[i].tick;
        bpm = tempo[i].bpm;
    }
    return prevTick + (target - sec) * bpm * PPQ / 60.0;
}

TimeSigPoint Project::timeSigAtBar(int bar) const {
    TimeSigPoint r = timeSig.front();
    for (auto& t : timeSig) { if (t.bar <= bar) r = t; else break; }
    return r;
}

static int ticksPerBar(const TimeSigPoint& t) { return t.num * PPQ * 4 / std::max(1, t.den); }

int Project::barToTick(int bar) const {
    int tick = 0, curBar = 0;
    TimeSigPoint ts = timeSig.front();
    for (size_t i = 1; i < timeSig.size() && timeSig[i].bar < bar; ++i) {
        tick += (timeSig[i].bar - curBar) * ticksPerBar(ts);
        curBar = timeSig[i].bar;
        ts = timeSig[i];
    }
    return tick + (bar - curBar) * ticksPerBar(ts);
}

int Project::tickToBar(int tick) const {
    int t0 = 0, curBar = 0;
    TimeSigPoint ts = timeSig.front();
    for (size_t i = 1; i < timeSig.size(); ++i) {
        const int next = t0 + (timeSig[i].bar - curBar) * ticksPerBar(ts);
        if (next > tick) break;
        t0 = next; curBar = timeSig[i].bar; ts = timeSig[i];
    }
    return curBar + (tick - t0) / std::max(1, ticksPerBar(ts));
}

int Project::ticksPerBarAt(int tick) const { return ticksPerBar(timeSigAtBar(tickToBar(tick))); }

juce::String Project::formatPosition(double tick) const {
    const int t = (int)std::floor(std::max(0.0, tick));
    const int bar = tickToBar(t);
    const auto ts = timeSigAtBar(bar);
    const int beatTicks = PPQ * 4 / std::max(1, ts.den);
    const int inBar = t - barToTick(bar);
    const int beat = inBar / beatTicks;
    const int six = (inBar % beatTicks) / std::max(1, beatTicks / 4);
    return juce::String(bar + 1) + "." + juce::String(beat + 1) + "." + juce::String(six + 1);
}

int Project::songEndTick() const {
    int e = 0;
    for (auto& t : tracks)
        for (auto& c : t.clips) e = std::max(e, c.end());
    return e;
}

// ------------------------------------------------------------------ snap
const std::vector<SnapInfo>& snapTable() {
    static const std::vector<SnapInfo> s = {
        { "Off", 1 }, { "Bar", -1 }, { "1/2 Bar", -2 }, { "1/4 Bar", -4 },   // negative = fraction of current bar
        { "1/4", PPQ }, { "1/8", PPQ / 2 }, { "1/16", PPQ / 4 }, { "1/32", PPQ / 8 },
        { "1/4 T", PPQ * 2 / 3 }, { "1/8 T", PPQ / 3 }, { "1/16 T", PPQ / 6 },
    };
    return s;
}

int snapTicks(const Project& p, int idx, int atTick) {
    const auto& t = snapTable();
    const auto& s = t[(size_t)juce::jlimit(0, (int)t.size() - 1, idx)];
    if (s.ticks < 0) return std::max(1, p.ticksPerBarAt(atTick) / -s.ticks);
    return s.ticks;
}

int snapTick(const Project& p, int tick, int idx, bool round) {
    const int g = snapTicks(p, idx, tick);
    if (g <= 1) return std::max(0, tick);
    // snap relative to the start of the bar so odd meters stay aligned
    const int bar = p.tickToBar(std::max(0, tick));
    const int b0 = p.barToTick(bar);
    const int rel = tick - b0;
    const int q = round ? (int)std::floor((rel + g * 0.5) / g) * g : (int)std::floor((double)rel / g) * g;
    return std::max(0, b0 + q);
}

// ------------------------------------------------------------------ serialization
namespace {
using juce::var;
var obj() { return var(new juce::DynamicObject()); }
void put(var& o, const juce::Identifier& k, const var& v) { o.getDynamicObject()->setProperty(k, v); }
var colourVar(juce::Colour c) { return c.toString(); }
juce::Colour colourFrom(const var& v, juce::Colour d) { return v.isString() ? juce::Colour::fromString(v.toString()) : d; }
int gi(const var& o, const char* k, int d = 0) { return o.hasProperty(k) ? (int)o[k] : d; }
float gf(const var& o, const char* k, float d = 0.f) { return o.hasProperty(k) ? (float)(double)o[k] : d; }
double gd(const var& o, const char* k, double d = 0.0) { return o.hasProperty(k) ? (double)o[k] : d; }
bool gb(const var& o, const char* k, bool d = false) { return o.hasProperty(k) ? (bool)o[k] : d; }
juce::String gs(const var& o, const char* k, const juce::String& d = {}) { return o.hasProperty(k) ? o[k].toString() : d; }

var paramsVar(const ParamSetPtr& p) { return p ? p->toVar() : var(); }

var clipVar(const Clip& c) {
    var o = obj();
    put(o, "id", c.id); put(o, "name", c.name); put(o, "type", (int)c.type);
    put(o, "start", c.start); put(o, "length", c.length);
    put(o, "contentLength", c.contentLength); put(o, "contentOffset", c.contentOffset);
    if (c.muted) put(o, "muted", true);
    if (c.type == ClipType::Midi) {
        juce::Array<var> notes;
        for (auto& n : c.notes) notes.add(juce::Array<var> { n.start, n.length, n.pitch, (double)n.velocity });
        put(o, "notes", notes);
    } else if (c.type == ClipType::Pattern) {
        put(o, "pattern", c.patternId);
    } else {
        var a = obj();
        put(a, "file", c.audio.file);
        juce::Array<var> takes; for (auto& t : c.audio.takes) takes.add(t);
        put(a, "takes", takes);
        put(a, "offset", c.audio.offsetSec); put(a, "gain", c.audio.gainDb);
        put(a, "fadeIn", c.audio.fadeInSec); put(a, "fadeOut", c.audio.fadeOutSec);
        put(a, "reverse", c.audio.reverse); put(a, "loop", c.audio.loop); put(a, "normalize", c.audio.normalize);
        put(a, "pitch", c.audio.pitchSt); put(a, "stretch", c.audio.stretch); put(a, "keepPitch", c.audio.keepPitch);
        put(o, "audio", a);
    }
    return o;
}

Clip clipFrom(const var& o) {
    Clip c;
    c.id = gs(o, "id"); c.name = gs(o, "name");
    c.type = (ClipType)juce::jlimit(0, 2, gi(o, "type"));
    c.start = gi(o, "start"); c.length = std::max(1, gi(o, "length", PPQ * 4));
    c.contentLength = std::max(1, gi(o, "contentLength", c.length)); c.contentOffset = gi(o, "contentOffset");
    c.muted = gb(o, "muted");
    if (auto* arr = o["notes"].getArray())
        for (auto& n : *arr)
            if (n.size() >= 4) c.notes.push_back({ (int)n[0], std::max(1, (int)n[1]), juce::jlimit(0, 127, (int)n[2]), (float)(double)n[3] });
    c.patternId = gs(o, "pattern");
    if (o.hasProperty("audio")) {
        const var a = o["audio"];
        c.audio.file = gs(a, "file");
        if (auto* t = a["takes"].getArray()) for (auto& s : *t) c.audio.takes.add(s.toString());
        c.audio.offsetSec = gd(a, "offset"); c.audio.gainDb = gf(a, "gain");
        c.audio.fadeInSec = gd(a, "fadeIn"); c.audio.fadeOutSec = gd(a, "fadeOut");
        c.audio.reverse = gb(a, "reverse"); c.audio.loop = gb(a, "loop"); c.audio.normalize = gb(a, "normalize");
        c.audio.pitchSt = gf(a, "pitch"); c.audio.stretch = juce::jlimit(0.25f, 4.f, gf(a, "stretch", 1.f));
        c.audio.keepPitch = gb(a, "keepPitch", true);
    }
    return c;
}

var effectsVar(const std::vector<EffectData>& v) {
    juce::Array<var> fx;
    for (auto& e : v) {
        var eo = obj();
        put(eo, "id", e.id); put(eo, "type", e.type); put(eo, "bypass", e.bypass->load());
        if (e.name.isNotEmpty()) put(eo, "name", e.name);
        put(eo, "params", paramsVar(e.params));
        fx.add(eo);
    }
    return fx;
}

var trackVar(const Track& t) {
    var o = obj();
    put(o, "id", t.id); put(o, "name", t.name); put(o, "type", (int)t.type);
    put(o, "colour", colourVar(t.colour)); put(o, "output", t.output);
    put(o, "armed", t.armed); put(o, "collapsed", t.collapsed);
    if (t.input) put(o, "input", paramsVar(t.input));
    put(o, "inputChannel", t.inputChannel); put(o, "stereoInput", t.stereoInput);
    put(o, "channel", paramsVar(t.channel));
    if (t.hasInstrument()) {
        var i = obj();
        put(i, "engine", t.inst.engine); put(i, "preset", t.inst.preset);
        put(i, "params", paramsVar(t.inst.params)); put(i, "arp", paramsVar(t.inst.arp));
        put(i, "sample", t.inst.sample); put(i, "sampleRoot", t.inst.sampleRoot);
        if (t.inst.engine == "drums") {
            juce::Array<var> pads;
            for (auto& p : t.inst.pads) {
                var po = obj();
                put(po, "name", p.name); put(po, "sample", p.sample); put(po, "params", paramsVar(p.params));
                pads.add(po);
            }
            put(i, "pads", pads);
        }
        put(o, "instrument", i);
    }
    put(o, "effects", effectsVar(t.effects));
    if (!t.inputEffects.empty()) put(o, "inputEffects", effectsVar(t.inputEffects));
    juce::Array<var> clips;
    for (auto& c : t.clips) clips.add(clipVar(c));
    put(o, "clips", clips);
    juce::Array<var> lanes;
    for (auto& l : t.automation) {
        var lo = obj();
        put(lo, "target", l.target); put(lo, "enabled", l.enabled); put(lo, "shown", l.shown);
        juce::Array<var> pts;
        for (auto& p : l.points) pts.add(juce::Array<var> { p.tick, (double)p.value, p.curve });
        put(lo, "points", pts);
        lanes.add(lo);
    }
    put(o, "automation", lanes);
    return o;
}
}  // namespace

static bool trackFrom(Project& p, Track& t, const var& o) {
    t.id = gs(o, "id"); t.name = gs(o, "name");
    t.type = (TrackType)juce::jlimit(0, 5, gi(o, "type"));
    t.colour = colourFrom(o["colour"], t.colour);
    t.output = gs(o, "output", "master");
    t.armed = gb(o, "armed"); t.collapsed = gb(o, "collapsed");
    t.inputChannel = gi(o, "inputChannel"); t.stereoInput = gb(o, "stereoInput");
    t.input = Catalog::makeParams(Catalog::inputParams());
    if (o.hasProperty("input")) t.input->fromVar(o["input"]);
    else if (gb(o, "monitor")) t.input->set(1, (float)Track::MonitorThrough);   // files from before the recording booth
    t.channel = Catalog::makeParams(Catalog::channelParams());
    t.channel->fromVar(o["channel"]);
    if (t.hasInstrument() && o.hasProperty("instrument")) {
        const var i = o["instrument"];
        p.initInstrument(t.inst, gs(i, "engine", t.type == TrackType::Drum ? "drums" : "analog"));
        t.inst.preset = gs(i, "preset");
        t.inst.params->fromVar(i["params"]);
        t.inst.arp->fromVar(i["arp"]);
        t.inst.sample = gs(i, "sample");
        t.inst.sampleRoot = gi(i, "sampleRoot", 60);
        if (auto* pads = i["pads"].getArray())
            for (int k = 0; k < juce::jmin(kDrumPads, pads->size()); ++k) {
                auto& pad = t.inst.pads[(size_t)k];
                pad.name = gs((*pads)[k], "name", pad.name);
                pad.sample = gs((*pads)[k], "sample", pad.sample);
                pad.params->fromVar((*pads)[k]["params"]);
            }
    }
    auto readFx = [&p](const var& arr, std::vector<EffectData>& out) {
        if (auto* fx = arr.getArray())
            for (auto& eo : *fx) {
                const auto type = gs(eo, "type");
                if (Catalog::findEffect(type) == nullptr) continue;
                EffectData e = p.makeEffect(type);
                e.id = gs(eo, "id", e.id);
                e.name = gs(eo, "name");
                e.bypass->store(gb(eo, "bypass"));
                e.params->fromVar(eo["params"]);
                out.push_back(std::move(e));
            }
    };
    readFx(o["effects"], t.effects);
    readFx(o["inputEffects"], t.inputEffects);
    if (auto* clips = o["clips"].getArray())
        for (auto& c : *clips) t.clips.push_back(clipFrom(c));
    if (auto* lanes = o["automation"].getArray())
        for (auto& lo : *lanes) {
            AutomationLane l;
            l.target = gs(lo, "target"); l.enabled = gb(lo, "enabled", true); l.shown = gb(lo, "shown", true);
            if (auto* pts = lo["points"].getArray())
                for (auto& pt : *pts)
                    if (pt.size() >= 3) l.points.push_back({ (int)pt[0], juce::jlimit(0.f, 1.f, (float)(double)pt[1]), (int)pt[2] });
            std::sort(l.points.begin(), l.points.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
            t.automation.push_back(std::move(l));
        }
    return true;
}

juce::var Project::toVar() const {
    var o = obj();
    put(o, "format", "ostinato-project");
    put(o, "version", 1);
    put(o, "name", name); put(o, "created", created); put(o, "modified", modified);
    put(o, "sampleRate", sampleRate); put(o, "bufferSize", bufferSize);
    put(o, "lengthBars", lengthBars); put(o, "key", key); put(o, "scale", scale); put(o, "customScale", customScale);
    put(o, "snap", snap); put(o, "metronome", metronome); put(o, "countIn", countInBars);
    put(o, "loopOn", loopOn); put(o, "loopStart", loopStart); put(o, "loopEnd", loopEnd);
    put(o, "idCounter", idCounter);
    juce::Array<var> tp; for (auto& t : tempo) tp.add(juce::Array<var> { t.tick, t.bpm });
    put(o, "tempo", tp);
    juce::Array<var> ts; for (auto& t : timeSig) ts.add(juce::Array<var> { t.bar, t.num, t.den });
    put(o, "timeSig", ts);
    juce::Array<var> mk;
    for (auto& m : markers) { var mo = obj(); put(mo, "tick", m.tick); put(mo, "name", m.name); put(mo, "section", m.section); put(mo, "colour", colourVar(m.colour)); mk.add(mo); }
    put(o, "markers", mk);
    juce::Array<var> tr; for (auto& t : tracks) tr.add(trackVar(t));
    put(o, "tracks", tr);
    put(o, "master", trackVar(master));
    juce::Array<var> pats;
    for (auto& p : patterns) {
        var po = obj();
        put(po, "id", p.id); put(po, "name", p.name); put(po, "steps", p.steps); put(po, "stepTicks", p.stepTicks);
        put(po, "swing", (double)p.swing); put(po, "colour", colourVar(p.colour));
        juce::Array<var> lanes;
        for (int pad = 0; pad < kDrumPads; ++pad) {
            juce::Array<var> steps;
            for (int s = 0; s < kMaxSteps; ++s) {
                const auto& st = p.grid[(size_t)pad][(size_t)s];
                if (st.on) steps.add(juce::Array<var> { s, (double)st.velocity, (double)st.probability, st.pitch, st.ratchet });
            }
            lanes.add(steps);
        }
        put(po, "grid", lanes);
        pats.add(po);
    }
    put(o, "patterns", pats);
    return o;
}

bool Project::fromVar(const juce::var& o, juce::String& error) {
    if (gs(o, "format") != "ostinato-project") { error = "Not an Ostinato project file."; return false; }
    if (gi(o, "version") > 1) { error = "This project was saved by a newer version of Ostinato."; return false; }
    *this = Project();
    name = gs(o, "name", "Untitled"); created = gs(o, "created", created); modified = gs(o, "modified", modified);
    sampleRate = gi(o, "sampleRate", 48000); bufferSize = gi(o, "bufferSize", 256);
    lengthBars = juce::jlimit(1, 999, gi(o, "lengthBars", 32));
    key = juce::jlimit(0, 11, gi(o, "key")); scale = gi(o, "scale"); customScale = gs(o, "customScale", customScale);
    snap = gi(o, "snap", 4); metronome = gb(o, "metronome"); countInBars = gi(o, "countIn", 1);
    loopOn = gb(o, "loopOn"); loopStart = gi(o, "loopStart"); loopEnd = gi(o, "loopEnd", PPQ * 16);
    idCounter = std::max(1, gi(o, "idCounter", 1));
    tempo.clear();
    if (auto* a = o["tempo"].getArray()) for (auto& t : *a) tempo.push_back({ (int)t[0], juce::jlimit(20.0, 300.0, (double)t[1]) });
    if (tempo.empty() || tempo.front().tick != 0) tempo.insert(tempo.begin(), TempoPoint {});
    timeSig.clear();
    if (auto* a = o["timeSig"].getArray()) for (auto& t : *a) timeSig.push_back({ (int)t[0], juce::jlimit(1, 32, (int)t[1]), juce::jlimit(1, 32, (int)t[2]) });
    if (timeSig.empty() || timeSig.front().bar != 0) timeSig.insert(timeSig.begin(), TimeSigPoint {});
    if (auto* a = o["markers"].getArray())
        for (auto& m : *a) markers.push_back({ gi(m, "tick"), gs(m, "name"), gb(m, "section"), colourFrom(m["colour"], juce::Colour(0xff9aa0a6)) });
    if (auto* a = o["patterns"].getArray())
        for (auto& po : *a) {
            Pattern p;
            p.id = gs(po, "id"); p.name = gs(po, "name");
            p.steps = juce::jlimit(1, kMaxSteps, gi(po, "steps", 16));
            p.stepTicks = juce::jlimit(PPQ / 16, PPQ, gi(po, "stepTicks", PPQ / 4));
            p.swing = juce::jlimit(0.f, 0.75f, gf(po, "swing"));
            p.colour = colourFrom(po["colour"], p.colour);
            if (auto* lanes = po["grid"].getArray())
                for (int pad = 0; pad < juce::jmin(kDrumPads, lanes->size()); ++pad)
                    if (auto* steps = (*lanes)[pad].getArray())
                        for (auto& s : *steps) {
                            const int idx = (int)s[0];
                            if (!juce::isPositiveAndBelow(idx, kMaxSteps)) continue;
                            auto& st = p.grid[(size_t)pad][(size_t)idx];
                            st.on = true; st.velocity = (float)(double)s[1]; st.probability = (float)(double)s[2];
                            st.pitch = (int)s[3]; st.ratchet = juce::jlimit(1, 4, (int)s[4]);
                        }
            patterns.push_back(p);
        }
    if (auto* a = o["tracks"].getArray())
        for (auto& to : *a) { Track t; trackFrom(*this, t, to); tracks.push_back(std::move(t)); }
    if (o.hasProperty("master")) { trackFrom(*this, master, o["master"]); master.id = "master"; master.type = TrackType::Master; }
    idCounter = std::max(1, gi(o, "idCounter", idCounter));   // ids created while loading were replaced by saved ids
    return true;
}
