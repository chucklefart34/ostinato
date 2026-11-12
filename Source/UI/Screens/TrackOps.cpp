// Track-level actions shared by the arrangement, mixer and instrument screens.
#include "Screens.h"
#include "Core/Paths.h"
#include "Engine/Catalog.h"

using namespace UI;

namespace ScreenUtil {

juce::String addTrack(Session& s, TrackType type, const juce::String& preset) {
    s.checkpoint("Add " + trackTypeName(type).toLowerCase() + " track");
    int insertAt = -1;
    if (auto idx = s.project.trackIndex(s.selectedTrack); idx >= 0) insertAt = idx + 1;
    if (type == TrackType::Return) insertAt = -1;
    if (type == TrackType::Return && (int)s.project.returnTracks().size() >= 4) { ctx().toast("Up to 4 effect returns (sends A to D)"); return {}; }
    auto& t = s.project.addTrack(type, {}, preset, insertAt);
    const auto id = t.id;
    if (type == TrackType::Return) {
        auto fx = s.project.makeEffect("reverb");
        Catalog::applyValues(*fx.params, "mix=100");
        t.effects.push_back(fx);
    }
    if (type == TrackType::Drum && s.project.patterns.empty()) s.currentPattern = s.project.addPattern().id;
    s.changed(true);
    s.selectTrack(id);
    return id;
}

static juce::File trackPresetDir() { auto d = Paths::presets().getChildFile("Tracks"); d.createDirectory(); return d; }

static void saveTrackPreset(Session& s, const juce::String& id) {
    const Track* t = s.project.findTrack(id);
    if (!t) return;
    askText("Save track preset", "Saves the instrument, effects and mixer settings.", t->name, "Save", [&s, id](const juce::String& name) {
        Project tmp;
        juce::String err;
        tmp.fromVar(s.project.toVar(), err);
        Track* src = tmp.findTrack(id);
        if (!src) return;
        Track copy = *src;
        copy.clips.clear();
        copy.automation.clear();
        Project holder;
        holder.tracks.push_back(copy);
        auto v = holder.toVar();
        trackPresetDir().getChildFile(juce::File::createLegalFileName(name) + ".json").replaceWithText(juce::JSON::toString(v["tracks"][0]));
        ctx().toast("Track preset saved");
    });
}

static void loadTrackPreset(Session& s, const juce::File& f) {
    auto tv = juce::JSON::parse(f);
    if (!tv.isObject()) { ctx().toast("Couldn't read the preset"); return; }
    // load through a minimal project document so all validation applies
    auto* doc = new juce::DynamicObject();
    doc->setProperty("format", "ostinato-project");
    doc->setProperty("version", 1);
    doc->setProperty("tracks", juce::Array<juce::var> { tv });
    Project tmp;
    juce::String err;
    if (!tmp.fromVar(juce::var(doc), err) || tmp.tracks.empty()) { ctx().toast(err); return; }
    s.checkpoint("Add track from preset");
    Track t = tmp.tracks.front();
    t.id = s.project.newId("t");
    for (auto& e : t.effects) e.id = s.project.newId("fx");
    t.name = f.getFileNameWithoutExtension();
    s.project.tracks.push_back(t);
    s.changed(true);
    s.selectTrack(t.id);
}

void addTrackMenu(Session& s, juce::Component* target) {
    juce::PopupMenu m, inst, drums, presets;
    std::map<juce::String, juce::PopupMenu> cats;
    int id = 100;
    std::vector<juce::String> names;
    for (auto& p : Catalog::instrumentPresets()) {
        names.push_back(p.name);
        if (juce::String(p.category) == "Drums") drums.addItem(id++, p.name);
        else cats[p.category].addItem(id++, p.name);
    }
    for (auto c : { "Synths", "Keys", "Instruments", "Sampler" }) inst.addSubMenu(c, cats[c]);
    juce::Array<juce::File> presetFiles;
    for (auto& f : trackPresetDir().findChildFiles(juce::File::findFiles, false, "*.json")) { presetFiles.add(f); presets.addItem(1000 + presetFiles.size() - 1, f.getFileNameWithoutExtension()); }
    m.addSubMenu("Instrument track", inst);
    m.addSubMenu("Drum track", drums);
    m.addItem(1, "Audio track (record or import audio)");
    m.addItem(2, "Group bus (submix)");
    m.addItem(3, "Effect return (send target)");
    m.addSubMenu("From track preset", presets, presetFiles.size() > 0);
    showMenu(m, target, [&s, names, presetFiles](int r) {
        if (r == 1) addTrack(s, TrackType::Audio);
        else if (r == 2) addTrack(s, TrackType::Bus);
        else if (r == 3) addTrack(s, TrackType::Return);
        else if (r >= 1000) loadTrackPreset(s, presetFiles[r - 1000]);
        else if (r >= 100) {
            const auto name = names[(size_t)(r - 100)];
            const auto* p = Catalog::findPreset(name);
            addTrack(s, p && juce::String(p->engine) == "drums" ? TrackType::Drum : TrackType::Instrument, name);
        }
    });
}

static juce::StringArray laneTargets(const Track& t, juce::StringArray& labels) {
    juce::StringArray targets;
    auto add = [&](const juce::String& tgt, const juce::String& label) { targets.add(tgt); labels.add(label); };
    add("ch:vol", "Volume"); add("ch:pan", "Pan");
    for (int k = 0; k < 4; ++k) add("ch:send" + juce::String(k + 1), juce::String("Send ") + "ABCD"[k]);
    if (t.inst.params)
        for (int i = 0; i < t.inst.params->size(); ++i) {
            const auto& m = t.inst.params->meta(i);
            if (m.scale == ParamScale::Linear || m.scale == ParamScale::Log) add("inst:" + juce::String(m.id), "Instrument: " + juce::String(m.name));
        }
    for (auto& e : t.effects) {
        const auto* info = Catalog::findEffect(e.type);
        for (int i = 0; i < e.params->size(); ++i) {
            const auto& m = e.params->meta(i);
            if (m.scale == ParamScale::Linear || m.scale == ParamScale::Log) add("fx:" + e.id + ":" + m.id, juce::String(info ? info->name : "Effect") + ": " + m.name);
        }
    }
    return targets;
}

juce::String laneLabel(const Track& t, const juce::String& target) {
    juce::StringArray labels;
    auto targets = laneTargets(t, labels);
    const int i = targets.indexOf(target);
    return i >= 0 ? labels[i] : target;
}

void trackMenu(Session& s, const juce::String& id, juce::Component* target) {
    Track* t = s.project.findTrack(id);
    if (!t) return;
    juce::PopupMenu m, colours, outputs, lanes, input;
    const bool isMaster = t->type == TrackType::Master;
    m.addSectionHeader(t->name + " (" + trackTypeName(t->type) + ")");
    if (!isMaster) m.addItem(1, "Rename...");
    for (int i = 0; i < 12; ++i) { const auto c = Project::trackColour(i); colours.addColouredItem(100 + i, "Colour " + juce::String(i + 1), c, true, t->colour == c); }
    if (!isMaster) m.addSubMenu("Colour", colours);
    if (t->hasInstrument()) m.addItem(2, "Instrument and sound...");
    m.addItem(3, "Effects (" + juce::String((int)t->effects.size()) + ")...");
    juce::StringArray labels;
    auto targets = laneTargets(*t, labels);
    for (int i = 0; i < targets.size(); ++i) lanes.addItem(300 + i, labels[i], t->findLane(targets[i]) == nullptr, t->findLane(targets[i]) != nullptr);
    if (t->type != TrackType::Return || true) m.addSubMenu("Add automation lane", lanes);
    if (!t->automation.empty()) m.addItem(4, t->collapsed ? "Show automation lanes" : "Hide automation lanes");
    if (!isMaster && t->type != TrackType::Return) {
        outputs.addItem(200, "Master", true, t->output == "master");
        int k = 0;
        std::vector<juce::String> busIds;
        for (auto& o : s.project.tracks) if (o.type == TrackType::Bus && o.id != id) { busIds.push_back(o.id); outputs.addItem(201 + k++, o.name, true, t->output == o.id); }
        m.addSubMenu("Output", outputs);
    }
    if (t->type == TrackType::Audio) {
        for (int c = 0; c < 8; ++c) input.addItem(400 + c, "Input " + juce::String(c + 1) + " (mono)", true, !t->stereoInput && t->inputChannel == c);
        for (int c = 0; c < 8; c += 2) input.addItem(420 + c, "Inputs " + juce::String(c + 1) + "+" + juce::String(c + 2) + " (stereo)", true, t->stereoInput && t->inputChannel == c);
        m.addSubMenu("Recording input", input);
    }
    if (!isMaster) {
        m.addSeparator();
        m.addItem(5, "Duplicate track");
        m.addItem(6, "Move up", s.project.trackIndex(id) > 0);
        m.addItem(7, "Move down", s.project.trackIndex(id) < (int)s.project.tracks.size() - 1);
        m.addItem(8, "Save as track preset...");
        m.addSeparator();
        m.addItem(9, "Delete track...");
    }
    showMenu(m, target, [&s, id, targets](int r) {
        Track* t = s.project.findTrack(id);
        if (!t || r == 0) return;
        if (r == 1) askText("Rename track", {}, t->name, "Rename", [&s, id](const juce::String& n) { if (auto* tt = s.project.findTrack(id)) { s.checkpoint("Rename track"); tt->name = n.trim().isEmpty() ? tt->name : n.trim(); s.changed(true); } });
        else if (r >= 100 && r < 112) { s.checkpoint("Track colour"); t->colour = Project::trackColour(r - 100); s.changed(true); }
        else if (r == 2) { s.selectTrack(id); ctx().go(ScreenId::Instruments); }
        else if (r == 3) { s.selectTrack(id); ctx().go(ScreenId::Effects); }
        else if (r == 4) { s.checkpoint("Toggle lanes"); t->collapsed = !t->collapsed; s.changed(true); }
        else if (r >= 300 && r < 400) {
            const auto tgt = targets[r - 300];
            if (t->findLane(tgt)) return;
            s.checkpoint("Add automation lane");
            AutomationLane l; l.target = tgt;
            // seed with the current value so adding a lane changes nothing until edited
            ParamSetPtr ps; juce::String pid = tgt.fromLastOccurrenceOf(":", false, false);
            if (tgt.startsWith("ch:")) ps = t->channel; else if (tgt.startsWith("inst:")) ps = t->inst.params;
            else for (auto& e : t->effects) if (tgt.contains(e.id)) ps = e.params;
            const int pi = ps ? ps->indexOf(pid) : -1;
            l.points = { { 0, pi >= 0 ? ps->toNorm(pi, ps->getBase(pi)) : 0.5f, 0 } };
            t->automation.push_back(l);
            t->collapsed = false;
            s.changed(true);
        }
        else if (r >= 200 && r < 300) {
            s.checkpoint("Change output");
            if (r == 200) t->output = "master";
            else { int k = 0; for (auto& o : s.project.tracks) if (o.type == TrackType::Bus && o.id != id) { if (201 + k == r) t->output = o.id; ++k; } }
            s.changed(true);
        }
        else if (r >= 400 && r < 440) { s.checkpoint("Recording input"); t->stereoInput = r >= 420; t->inputChannel = r >= 420 ? r - 420 : r - 400; s.changed(true); }
        else if (r == 5) {
            s.checkpoint("Duplicate track");
            Project tmp; juce::String err;
            tmp.fromVar(s.project.toVar(), err);   // deep copy (fresh parameter objects)
            Track copy = *tmp.findTrack(id);
            copy.id = s.project.newId("t");
            copy.name << " copy";
            for (auto& c : copy.clips) c.id = s.project.newId("c");
            for (auto& e : copy.effects) {
                const auto newFx = s.project.newId("fx");
                for (auto& l : copy.automation) l.target = l.target.replace("fx:" + e.id + ":", "fx:" + newFx + ":");
                e.id = newFx;
            }
            for (auto& e : copy.inputEffects) e.id = s.project.newId("fx");
            s.project.tracks.insert(s.project.tracks.begin() + s.project.trackIndex(id) + 1, copy);
            s.changed(true);
            s.selectTrack(copy.id);
        }
        else if (r == 6 || r == 7) {
            const int i = s.project.trackIndex(id), j = r == 6 ? i - 1 : i + 1;
            if (j < 0 || j >= (int)s.project.tracks.size()) return;
            s.checkpoint("Move track");
            std::swap(s.project.tracks[(size_t)i], s.project.tracks[(size_t)j]);
            s.changed(true);
        }
        else if (r == 8) saveTrackPreset(s, id);
        else if (r == 9) confirm("Delete \"" + t->name + "\"?", "The track and its clips are removed. You can undo this.", "Delete", [&s, id] {
            s.checkpoint("Delete track");
            const int i = s.project.trackIndex(id);
            if (i < 0) return;
            for (auto& o : s.project.tracks) if (o.output == id) o.output = "master";
            s.project.tracks.erase(s.project.tracks.begin() + i);
            if (s.selectedTrack == id) s.selectedTrack = s.project.tracks.empty() ? juce::String() : s.project.tracks[(size_t)juce::jmin(i, (int)s.project.tracks.size() - 1)].id;
            s.changed(true);
            s.notifySelection();
        }, true);
    });
}

}  // namespace ScreenUtil
