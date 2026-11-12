#include "Session.h"
#include "Settings.h"

Session::Session(AudioEngine& e) : engine(e) { startTimer(1000); }
Session::~Session() { stopTimer(); }

void Session::replaced() {
    undoStack.clear();
    redoStack.clear();
    dirty = false;
    selectedTrack = project.tracks.empty() ? juce::String() : project.tracks.front().id;
    editClipId.clear();
    currentPattern = project.patterns.empty() ? juce::String() : project.patterns.front().id;
    SampleBank::get().setProjectDir(folder);
    engine.setProject(&project);
    engine.setLiveTarget(selectedTrack);
    listeners.call([](Listener& l) { l.projectReplaced(); });
}

void Session::newProject(const juce::String& name, const juce::String& templ, double bpm, int num, int den) {
    project = Project();
    project.name = name.trim().isEmpty() ? juce::String("Untitled") : name.trim();
    ProjectStore::buildTemplate(project, templ);
    if (bpm > 0 && (templ == "Empty" || templ.isEmpty())) project.tempo.front().bpm = juce::jlimit(20.0, 300.0, bpm);
    else if (bpm > 0) project.tempo.front().bpm = juce::jlimit(20.0, 300.0, bpm);
    project.timeSig.front().num = num;
    project.timeSig.front().den = den;
    if (project.tracks.empty()) project.addTrack(TrackType::Instrument, {}, "Grand Piano");
    folder = ProjectStore::uniqueFolder(project.name);
    juce::String err;
    ProjectStore::save(project, folder, err);
    AppSettings::get().addRecentProject(folder.getFullPathName());
    AppSettings::get().save();
    replaced();
    if (makeThumbnail) makeThumbnail(folder);
}

bool Session::open(const juce::File& f, juce::String& error) {
    Project p;
    if (!ProjectStore::load(p, f, error)) return false;
    project = std::move(p);
    folder = f;
    AppSettings::get().addRecentProject(folder.getFullPathName());
    AppSettings::get().save();
    replaced();
    return true;
}

bool Session::save(juce::String& error) {
    if (!hasProject()) { error = "No project is open."; return false; }
    if (!ProjectStore::save(project, folder, error)) return false;
    project.modified = juce::Time::getCurrentTime().toISO8601(true);
    dirty = false;
    lastSave = juce::Time::getCurrentTime();
    ProjectStore::clearRecovery();
    if (makeThumbnail) makeThumbnail(folder);
    return true;
}

bool Session::saveAs(const juce::String& name, juce::String& error) {
    juce::File nf;
    if (!ProjectStore::saveAs(project, folder, name, nf, error)) return false;
    project.name = name;
    folder = nf;
    SampleBank::get().setProjectDir(folder);
    dirty = false;
    AppSettings::get().addRecentProject(folder.getFullPathName());
    AppSettings::get().save();
    if (makeThumbnail) makeThumbnail(folder);
    listeners.call([](Listener& l) { l.projectChanged(true); });
    return true;
}

void Session::checkpoint(const juce::String& description) {
    undoStack.push_back({ description, juce::JSON::toString(project.toVar(), true) });
    redoStack.clear();
    // bound memory: at most 100 steps and ~64 MB of history
    while (undoStack.size() > 100 || (undoStack.size() > 5 && undoBytes() > (size_t)64 << 20)) undoStack.erase(undoStack.begin());
}

size_t Session::undoBytes() const {
    size_t b = 0;
    for (auto& s : undoStack) b += (size_t)s.json.getNumBytesAsUTF8();
    for (auto& s : redoStack) b += (size_t)s.json.getNumBytesAsUTF8();
    return b;
}

void Session::changed(bool structural) {
    dirty = true;
    engine.markDirty();
    listeners.call([structural](Listener& l) { l.projectChanged(structural); });
}

void Session::restore(const juce::String& json) {
    juce::String err;
    Project p;
    if (!p.fromVar(juce::JSON::parse(json), err)) return;
    const auto keepTrack = selectedTrack, keepClip = editClipId, keepPat = currentPattern;
    project = std::move(p);
    dirty = true;
    engine.setProject(&project);
    selectedTrack = project.findTrack(keepTrack) ? keepTrack : (project.tracks.empty() ? juce::String() : project.tracks.front().id);
    editClipId = keepClip;
    currentPattern = project.findPattern(keepPat) ? keepPat : (project.patterns.empty() ? juce::String() : project.patterns.front().id);
    listeners.call([](Listener& l) { l.projectReplaced(); });
}

void Session::undo() {
    if (undoStack.empty()) return;
    auto s = undoStack.back();
    undoStack.pop_back();
    redoStack.push_back({ s.name, juce::JSON::toString(project.toVar(), true) });
    restore(s.json);
}

void Session::redo() {
    if (redoStack.empty()) return;
    auto s = redoStack.back();
    redoStack.pop_back();
    undoStack.push_back({ s.name, juce::JSON::toString(project.toVar(), true) });
    restore(s.json);
}

juce::StringArray Session::history() const {
    juce::StringArray h;
    for (auto& s : undoStack) h.add(s.name);
    return h;
}

void Session::selectTrack(const juce::String& id) {
    selectedTrack = id;
    if (auto* t = track(); t && t->hasInstrument()) engine.setLiveTarget(id);
    listeners.call([](Listener& l) { l.selectionChanged(); });
}

Clip* Session::editClip(Track** owner) {
    for (auto& t : project.tracks)
        for (auto& c : t.clips)
            if (c.id == editClipId) { if (owner) *owner = &t; return &c; }
    return nullptr;
}

Pattern* Session::pattern() { return project.findPattern(currentPattern); }

void Session::timerCallback() {
    if (!hasProject() || !dirty) { secondsSinceAutosave = secondsSinceRecovery = 0; return; }
    ++secondsSinceAutosave;
    ++secondsSinceRecovery;
    if (secondsSinceRecovery >= 15) {
        ProjectStore::writeRecovery(project, folder);
        secondsSinceRecovery = 0;
    }
    if (secondsSinceAutosave >= AppSettings::get().autosaveSeconds && !engine.isRecording()) {
        juce::String err;
        if (save(err)) lastAutosave = juce::Time::getCurrentTime();
        else lastAutosaveError = err;
        secondsSinceAutosave = 0;
    }
}
