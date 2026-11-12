#include "Settings.h"
#include "Paths.h"

AppSettings& AppSettings::get() { static AppSettings s; return s; }

static juce::StringArray arr(const juce::var& v) {
    juce::StringArray s;
    if (auto* a = v.getArray()) for (auto& x : *a) s.add(x.toString());
    return s;
}
static juce::var arrVar(const juce::StringArray& s) {
    juce::Array<juce::var> a;
    for (auto& x : s) a.add(x);
    return a;
}

void AppSettings::load() {
    const auto v = juce::JSON::parse(Paths::settingsFile());
    if (!v.isObject()) return;
    auto b = [&](const char* k, bool d) { return v.hasProperty(k) ? (bool)v[k] : d; };
    auto d = [&](const char* k, double def) { return v.hasProperty(k) ? (double)v[k] : def; };
    darkTheme = b("darkTheme", darkTheme);
    uiScale = (float)juce::jlimit(0.75, 1.5, d("uiScale", uiScale));
    haptics = b("haptics", haptics);
    tooltips = b("tooltips", tooltips);
    tutorialDone = b("tutorialDone", tutorialDone);
    autosaveSeconds = (int)juce::jlimit(15.0, 600.0, d("autosaveSeconds", autosaveSeconds));
    defaultBpm = juce::jlimit(20.0, 300.0, d("defaultBpm", defaultBpm));
    defaultTemplate = v.hasProperty("defaultTemplate") ? v["defaultTemplate"].toString() : defaultTemplate;
    latencyOffsetMs = d("latencyOffsetMs", latencyOffsetMs);
    quantizeMidiRecording = b("quantizeMidiRecording", quantizeMidiRecording);
    followPlayhead = b("followPlayhead", followPlayhead);
    midiClockSync = b("midiClockSync", midiClockSync);
    lowPowerWhenIdle = b("lowPowerWhenIdle", lowPowerWhenIdle);
    backgroundAudio = b("backgroundAudio", backgroundAudio);
    metronomeVolume = (float)d("metronomeVolume", metronomeVolume);
    audioDeviceXml = v["audioDeviceXml"].toString();
    recentProjects = arr(v["recentProjects"]);
    favoriteSamples = arr(v["favoriteSamples"]);
    recentSamples = arr(v["recentSamples"]);
    favoriteInstruments = arr(v["favoriteInstruments"]);
}

void AppSettings::save() const {
    auto* o = new juce::DynamicObject();
    o->setProperty("darkTheme", darkTheme);
    o->setProperty("uiScale", uiScale);
    o->setProperty("haptics", haptics);
    o->setProperty("tooltips", tooltips);
    o->setProperty("tutorialDone", tutorialDone);
    o->setProperty("autosaveSeconds", autosaveSeconds);
    o->setProperty("defaultBpm", defaultBpm);
    o->setProperty("defaultTemplate", defaultTemplate);
    o->setProperty("latencyOffsetMs", latencyOffsetMs);
    o->setProperty("quantizeMidiRecording", quantizeMidiRecording);
    o->setProperty("followPlayhead", followPlayhead);
    o->setProperty("midiClockSync", midiClockSync);
    o->setProperty("lowPowerWhenIdle", lowPowerWhenIdle);
    o->setProperty("backgroundAudio", backgroundAudio);
    o->setProperty("metronomeVolume", metronomeVolume);
    o->setProperty("audioDeviceXml", audioDeviceXml);
    o->setProperty("recentProjects", arrVar(recentProjects));
    o->setProperty("favoriteSamples", arrVar(favoriteSamples));
    o->setProperty("recentSamples", arrVar(recentSamples));
    o->setProperty("favoriteInstruments", arrVar(favoriteInstruments));
    Paths::settingsFile().replaceWithText(juce::JSON::toString(juce::var(o)));
}

void AppSettings::addRecentProject(const juce::String& p) {
    recentProjects.removeString(p);
    recentProjects.insert(0, p);
    while (recentProjects.size() > 12) recentProjects.remove(recentProjects.size() - 1);
}
void AppSettings::addRecentSample(const juce::String& r) {
    recentSamples.removeString(r);
    recentSamples.insert(0, r);
    while (recentSamples.size() > 30) recentSamples.remove(recentSamples.size() - 1);
}
