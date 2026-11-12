#include "ProjectStore.h"
#include "Paths.h"
#include "Engine/Catalog.h"

// Gives a vocal track a ready-to-sing input chain (heard while recording, recorded clean).
static void addVocalBooth(Project& p, Track& v, const char* chain) {
    for (auto& c : Catalog::chainPresets())
        if (juce::String(c.name) == chain)
            for (auto& [type, values] : c.effects) { auto fx = p.makeEffect(type); Catalog::applyValues(*fx.params, values); v.inputEffects.push_back(fx); }
}

static juce::String folderNameFor(const juce::String& name) {
    auto n = juce::File::createLegalFileName(name.trim());
    if (n.isEmpty()) n = "Untitled";
    return n + ".ostinato";
}

juce::File ProjectStore::uniqueFolder(const juce::String& name) {
    auto base = Paths::projects().getChildFile(folderNameFor(name));
    if (!base.exists()) return base;
    for (int i = 2; i < 1000; ++i) {
        auto f = Paths::projects().getChildFile(folderNameFor(name + " " + juce::String(i)));
        if (!f.exists()) return f;
    }
    return Paths::projects().getNonexistentChildFile("Untitled", ".ostinato");
}

bool ProjectStore::readInfo(const juce::File& folder, ProjectInfo& info) {
    const auto f = projectFile(folder);
    if (!f.existsAsFile()) return false;
    const auto v = juce::JSON::parse(f);
    if (v["format"].toString() != "ostinato-project") return false;
    info.folder = folder;
    info.name = v["name"].toString();
    info.modified = v["modified"].toString();
    info.created = v["created"].toString();
    info.key = (int)v["key"]; info.scale = (int)v["scale"];
    if (auto* t = v["tempo"].getArray(); t && t->size() > 0) info.bpm = (double)(*t)[0][1];
    if (auto* s = v["timeSig"].getArray(); s && s->size() > 0) { info.sigNum = (int)(*s)[0][1]; info.sigDen = (int)(*s)[0][2]; }
    int end = 0;
    if (auto* tr = v["tracks"].getArray()) {
        info.tracks = tr->size();
        for (auto& t : *tr)
            if (auto* clips = t["clips"].getArray())
                for (auto& c : *clips) end = std::max(end, (int)c["start"] + (int)c["length"]);
    }
    const int tpb = info.sigNum * PPQ * 4 / std::max(1, info.sigDen);
    info.bars = (end + tpb - 1) / std::max(1, tpb);
    info.lengthSeconds = end * 60.0 / (info.bpm * PPQ);
    return true;
}

std::vector<ProjectInfo> ProjectStore::list() {
    std::vector<ProjectInfo> v;
    for (auto& d : Paths::projects().findChildFiles(juce::File::findDirectories, false, "*.ostinato")) {
        ProjectInfo i;
        if (readInfo(d, i)) v.push_back(i);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.modified > b.modified; });
    return v;
}

bool ProjectStore::save(const Project& p, const juce::File& folder, juce::String& error) {
    if (!folder.createDirectory()) { error = "Cannot create the project folder."; return false; }
    folder.getChildFile("audio").createDirectory();
    Project copy;
    (void)copy;
    auto v = p.toVar();
    v.getDynamicObject()->setProperty("modified", juce::Time::getCurrentTime().toISO8601(true));
    const auto json = juce::JSON::toString(v, false);
    const auto target = projectFile(folder);
    juce::TemporaryFile tmp(target);
    if (!tmp.getFile().replaceWithText(json, false, false, "\n")) { error = "Disk write failed (storage full?)."; return false; }
    // verify what we wrote parses before replacing the real file
    if (juce::JSON::parse(tmp.getFile())["format"].toString() != "ostinato-project") { error = "Save verification failed."; return false; }
    if (!tmp.overwriteTargetFileWithTemporary()) { error = "Could not replace the project file."; return false; }
    return true;
}

bool ProjectStore::load(Project& p, const juce::File& folder, juce::String& error) {
    const auto f = projectFile(folder);
    if (!f.existsAsFile()) { error = "Project file not found."; return false; }
    juce::var v;
    const auto res = juce::JSON::parse(f.loadFileAsString(), v);
    if (res.failed()) { error = "The project file is damaged: " + res.getErrorMessage(); return false; }
    return p.fromVar(v, error);
}

bool ProjectStore::rename(const juce::File& folder, const juce::String& newName, juce::File& newFolder, juce::String& error) {
    Project p;
    if (!load(p, folder, error)) return false;
    p.name = newName.trim().isEmpty() ? juce::String("Untitled") : newName.trim();
    newFolder = folder.getParentDirectory().getChildFile(folderNameFor(p.name));
    if (newFolder != folder) {
        if (newFolder.exists()) newFolder = uniqueFolder(p.name);
        if (!folder.moveFileTo(newFolder)) { error = "Could not rename the project folder."; return false; }
    }
    return save(p, newFolder, error);
}

bool ProjectStore::duplicate(const juce::File& folder, juce::File& copy, juce::String& error) {
    Project p;
    if (!load(p, folder, error)) return false;
    p.name = p.name + " copy";
    copy = uniqueFolder(p.name);
    if (!folder.copyDirectoryTo(copy)) { error = "Could not copy the project."; return false; }
    p.created = juce::Time::getCurrentTime().toISO8601(true);
    return save(p, copy, error);
}

bool ProjectStore::remove(const juce::File& folder) {
   #if JUCE_IOS || JUCE_ANDROID
    return folder.deleteRecursively();
   #else
    return folder.moveToTrash() || folder.deleteRecursively();
   #endif
}

bool ProjectStore::saveAs(const Project& p, const juce::File& oldFolder, const juce::String& newName, juce::File& newFolder, juce::String& error) {
    Project copy;
    if (!copy.fromVar(p.toVar(), error)) return false;
    copy.name = newName.trim().isEmpty() ? juce::String("Untitled") : newName.trim();
    newFolder = uniqueFolder(copy.name);
    newFolder.createDirectory();
    if (oldFolder.getChildFile("audio").isDirectory()) oldFolder.getChildFile("audio").copyDirectoryTo(newFolder.getChildFile("audio"));
    return save(copy, newFolder, error);
}

bool ProjectStore::exportZip(const juce::File& folder, const juce::File& zip, juce::String& error) {
    juce::ZipFile::Builder b;
    for (auto& f : folder.findChildFiles(juce::File::findFiles, true))
        b.addFile(f, 6, folder.getFileName() + "/" + f.getRelativePathFrom(folder).replaceCharacter('\\', '/'));
    zip.deleteFile();
    juce::FileOutputStream os(zip);
    if (!os.openedOk() || !b.writeToStream(os, nullptr)) { error = "Could not write the zip file."; return false; }
    return true;
}

bool ProjectStore::importZip(const juce::File& zipFile, juce::File& folder, juce::String& error) {
    juce::ZipFile zip(zipFile);
    if (zip.getNumEntries() == 0) { error = "Not a valid project archive."; return false; }
    const auto tmp = juce::File::createTempFile("ostinato-import");
    tmp.createDirectory();
    const auto r = zip.uncompressTo(tmp, true);
    if (r.failed()) { error = r.getErrorMessage(); tmp.deleteRecursively(); return false; }
    juce::File found;
    for (auto& f : tmp.findChildFiles(juce::File::findFiles, true, "project.json")) { found = f.getParentDirectory(); break; }
    if (found == juce::File()) { error = "The archive contains no Ostinato project."; tmp.deleteRecursively(); return false; }
    Project p;
    if (!load(p, found, error)) { tmp.deleteRecursively(); return false; }
    folder = uniqueFolder(p.name);
    const bool ok = found.copyDirectoryTo(folder);
    tmp.deleteRecursively();
    if (!ok) error = "Could not copy the imported project.";
    return ok;
}

// ------------------------------------------------------------------ recovery
void ProjectStore::writeRecovery(const Project& p, const juce::File& folder) {
    auto v = p.toVar();
    v.getDynamicObject()->setProperty("recoveryFolder", folder.getFullPathName());
    v.getDynamicObject()->setProperty("recoveryTime", juce::Time::getCurrentTime().toISO8601(true));
    const auto f = Paths::recovery().getChildFile("autosave.json");
    juce::TemporaryFile tmp(f);
    if (tmp.getFile().replaceWithText(juce::JSON::toString(v, true))) tmp.overwriteTargetFileWithTemporary();
}
void ProjectStore::clearRecovery() { Paths::recovery().getChildFile("autosave.json").deleteFile(); }
bool ProjectStore::recoveryAvailable(juce::String& name, juce::File& folder, juce::Time& when) {
    const auto f = Paths::recovery().getChildFile("autosave.json");
    if (!f.existsAsFile()) return false;
    const auto v = juce::JSON::parse(f);
    if (v["format"].toString() != "ostinato-project") return false;
    name = v["name"].toString();
    folder = juce::File(v["recoveryFolder"].toString());
    when = juce::Time::fromISO8601(v["recoveryTime"].toString());
    // only offer if the autosave is newer than the last real save
    const auto saved = projectFile(folder);
    return !saved.existsAsFile() || saved.getLastModificationTime() < when;
}
bool ProjectStore::loadRecovery(Project& p, juce::File& folder, juce::String& error) {
    const auto v = juce::JSON::parse(Paths::recovery().getChildFile("autosave.json"));
    folder = juce::File(v["recoveryFolder"].toString());
    return p.fromVar(v, error);
}
void ProjectStore::markSessionOpen() { Paths::support().getChildFile("session.lock").replaceWithText(juce::Time::getCurrentTime().toISO8601(true)); }
void ProjectStore::markSessionClosed() { Paths::support().getChildFile("session.lock").deleteFile(); }
bool ProjectStore::previousSessionCrashed() { return Paths::support().getChildFile("session.lock").existsAsFile(); }

// ------------------------------------------------------------------ templates
juce::StringArray ProjectStore::templateNames() {
    juce::StringArray s { "Empty", "Beat Starter", "Hip-Hop", "Electronic", "Songwriter", "Band" };
    for (auto& f : Paths::templates().findChildFiles(juce::File::findFiles, false, "*.json")) s.add(f.getFileNameWithoutExtension());
    return s;
}

static void addDrumPattern(Project& p, Track& drums, const juce::String& name, const char* kick, const char* snare, const char* hat, int bars, float swing = 0.f) {
    auto& pat = p.addPattern(name);
    pat.swing = swing;
    auto setRow = [&](int pad, const char* row) {
        for (int i = 0; i < 16 && row[i]; ++i)
            if (row[i] != '.') { auto& s = pat.grid[(size_t)pad][(size_t)i]; s.on = true; s.velocity = row[i] == 'X' ? 1.f : 0.65f; }
    };
    setRow(0, kick); setRow(1, snare); setRow(3, hat);
    Clip c;
    c.id = p.newId("c");
    c.type = ClipType::Pattern;
    c.patternId = pat.id;
    c.name = name;
    c.start = 0;
    c.length = p.ticksPerBarAt(0) * bars;
    drums.clips.push_back(c);
}

void ProjectStore::buildTemplate(Project& p, const juce::String& name) {
    const auto userFile = Paths::templates().getChildFile(name + ".json");
    if (userFile.existsAsFile()) {
        juce::String err;
        Project t;
        if (t.fromVar(juce::JSON::parse(userFile), err)) { const auto keepName = p.name; p = t; p.name = keepName; return; }
    }
    auto addReturn = [&](const char* fx, const char* preset) {
        auto& r = p.addTrack(TrackType::Return);
        auto e = p.makeEffect(fx);
        for (auto& pr : Catalog::effectPresets()) if (juce::String(pr.effect) == fx && juce::String(pr.name) == preset) Catalog::applyValues(*e.params, pr.values);
        r.effects.push_back(e);
        return r.id;
    };
    if (name == "Empty") return;
    if (name == "Beat Starter" || name == "Hip-Hop") {
        const bool hh = name == "Hip-Hop";
        p.tempo.front().bpm = hh ? 88.0 : 100.0;
        auto& d = p.addTrack(TrackType::Drum, {}, hh ? "Trap Kit" : "Boom Kit");
        addDrumPattern(p, d, "Groove", "X......x..X.....", "....X.......X...", "x.x.x.x.x.x.x.x.", 4, hh ? 0.15f : 0.f);
        auto& b = p.addTrack(TrackType::Instrument, {}, hh ? "FM Bass" : "Bass Synth");
        Clip bc = p.makeMidiClip(0, p.ticksPerBarAt(0) * 4);
        const int roots[4] = { 36, 36, 41, 43 };
        for (int bar = 0; bar < 4; ++bar) { bc.notes.push_back({ bar * PPQ * 4, PPQ * 2, roots[bar], 0.9f }); bc.notes.push_back({ bar * PPQ * 4 + PPQ * 5 / 2, PPQ, roots[bar], 0.7f }); }
        bc.name = "Bassline";
        b.clips.push_back(bc);
        auto& k = p.addTrack(TrackType::Instrument, {}, hh ? "Rhodes-style Keys" : "Electric Piano");
        Clip kc = p.makeMidiClip(0, p.ticksPerBarAt(0) * 4);
        const int chords[4][3] = { { 60, 63, 67 }, { 60, 63, 67 }, { 60, 65, 68 }, { 62, 67, 70 } };
        for (int bar = 0; bar < 4; ++bar) for (int n : chords[bar]) kc.notes.push_back({ bar * PPQ * 4, PPQ * 3, n, 0.6f });
        kc.name = "Chords";
        k.clips.push_back(kc);
        p.key = 0; p.scale = 1;
        k.channel->set(k.channel->indexOf("send1"), -12.f);   // before addReturn: adding a track can move `k`
        addReturn("reverb", "Send (100% wet)");
    } else if (name == "Electronic") {
        p.tempo.front().bpm = 124.0;
        auto& d = p.addTrack(TrackType::Drum, {}, "Electro Kit");
        addDrumPattern(p, d, "Four on the floor", "X...X...X...X...", "....X.......X...", "..x...x...x...x.", 8);
        p.addTrack(TrackType::Instrument, {}, "Bass Synth");
        p.addTrack(TrackType::Instrument, {}, "Supersaw");
        p.addTrack(TrackType::Instrument, {}, "Pad Synth");
        addReturn("reverb", "Large Hall");
        addReturn("delay", "Send (100% wet)");
        p.master.effects.push_back(p.makeEffect("limiter"));
    } else if (name == "Songwriter") {
        p.tempo.front().bpm = 92.0;
        p.addTrack(TrackType::Instrument, {}, "Grand Piano");
        p.addTrack(TrackType::Instrument, {}, "Acoustic Guitar");
        p.addTrack(TrackType::Instrument, {}, "Strings");
        auto& v = p.addTrack(TrackType::Audio);
        v.name = "Vocals";
        v.effects.push_back(p.makeEffect("compressor"));
        addVocalBooth(p, v, "Clean Vocal");
        addReturn("reverb", "Plate");
    } else if (name == "Band") {
        p.tempo.front().bpm = 110.0;
        auto& d = p.addTrack(TrackType::Drum, {}, "Studio Kit");
        addDrumPattern(p, d, "Rock beat", "X.....X.X.......", "....X.......X...", "x.x.x.x.x.x.x.x.", 4);
        p.addTrack(TrackType::Instrument, {}, "Bass Guitar");
        p.addTrack(TrackType::Instrument, {}, "Electric Guitar");
        p.addTrack(TrackType::Instrument, {}, "Organ");
        auto& v = p.addTrack(TrackType::Audio);
        v.name = "Vocals";
        addVocalBooth(p, v, "Pop Vocal");
        addReturn("reverb", "Small Room");
    }
}

bool ProjectStore::saveUserTemplate(const Project& p, const juce::String& name, juce::String& error) {
    Project t;
    if (!t.fromVar(p.toVar(), error)) return false;
    for (auto& tr : t.tracks) tr.clips.erase(std::remove_if(tr.clips.begin(), tr.clips.end(), [](auto& c) { return c.type == ClipType::Audio; }), tr.clips.end());
    const auto f = Paths::templates().getChildFile(juce::File::createLegalFileName(name) + ".json");
    if (!f.replaceWithText(juce::JSON::toString(t.toVar(), false))) { error = "Could not save the template."; return false; }
    return true;
}
