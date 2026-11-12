#include "Screens.h"
#include "Core/Paths.h"
#include "Core/Settings.h"
#include "Core/Theory.h"
#include "UI/MainComponent.h"
#include <juce_audio_utils/juce_audio_utils.h>

using namespace UI;

namespace {

class Content : public juce::Component, private juce::Timer {
public:
    Content() {
        auto& st = AppSettings::get();
        auto toggle = [this](juce::ToggleButton& b, bool v, std::function<void(bool)> f) {
            addAndMakeVisible(b);
            b.setToggleState(v, juce::dontSendNotification);
            b.onClick = [&b, f] { f(b.getToggleState()); AppSettings::get().save(); };
        };
        toggle(darkT, st.darkTheme, [](bool v) { AppSettings::get().darkTheme = v; if (auto* m = dynamic_cast<MainComponent*>(ctx().root)) m->applyTheme(); });
        toggle(hapticT, st.haptics, [](bool v) { AppSettings::get().haptics = v; });
        toggle(tipT, st.tooltips, [](bool v) { AppSettings::get().tooltips = v; });
        toggle(followT, st.followPlayhead, [](bool v) { AppSettings::get().followPlayhead = v; });
        toggle(lowPowerT, st.lowPowerWhenIdle, [](bool v) { AppSettings::get().lowPowerWhenIdle = v; ctx().engine->setBatterySaver(v); });
        toggle(bgAudioT, st.backgroundAudio, [](bool v) { AppSettings::get().backgroundAudio = v; });
        toggle(clockT, st.midiClockSync, [](bool v) { AppSettings::get().midiClockSync = v; ctx().engine->midiClockSync = v; });
        addAndMakeVisible(scale);
        scale.setLabel("Interface size");
        scale.setItems({ "75%", "90%", "100%", "115%", "130%" }, 2);
        const float scales[] = { 0.75f, 0.9f, 1.f, 1.15f, 1.3f };
        for (int i = 0; i < 5; ++i) if (std::abs(scales[i] - st.uiScale) < 0.01f) scale.setSelected(i);
        scale.onChange = [](int i) {
            const float s[] = { 0.75f, 0.9f, 1.f, 1.15f, 1.3f };
            AppSettings::get().uiScale = s[i];
            AppSettings::get().save();
           #if ! (JUCE_IOS || JUCE_ANDROID)
            juce::Desktop::getInstance().setGlobalScaleFactor(s[i]);
           #endif
        };
        addAndMakeVisible(autosave);
        autosave.setLabel("Autosave every");
        const int secs[] = { 15, 30, 60, 120, 300 };
        autosave.setItems({ "15 s", "30 s", "1 min", "2 min", "5 min" }, 2);
        for (int i = 0; i < 5; ++i) if (secs[i] == st.autosaveSeconds) autosave.setSelected(i);
        autosave.onChange = [](int i) { const int s[] = { 15, 30, 60, 120, 300 }; AppSettings::get().autosaveSeconds = s[i]; AppSettings::get().save(); };
        addAndMakeVisible(templ);
        templ.setLabel("New songs start from");
        templ.setItems(ProjectStore::templateNames(), juce::jmax(0, ProjectStore::templateNames().indexOf(st.defaultTemplate)));
        templ.onChange = [this](int) { AppSettings::get().defaultTemplate = templ.getText(); AppSettings::get().save(); };
        addAndMakeVisible(defBpm);
        defBpm.setLabel("Default tempo");
        juce::StringArray bpms; for (int b = 60; b <= 180; b += 10) bpms.add(juce::String(b) + " BPM");
        defBpm.setItems(bpms, juce::jlimit(0, bpms.size() - 1, (int)(st.defaultBpm - 60) / 10));
        defBpm.onChange = [](int i) { AppSettings::get().defaultBpm = 60 + i * 10; AppSettings::get().save(); };
        // project fields
        for (auto* b : { &projName, &projTempo, &projSig, &projKey, &projLen }) { addAndMakeVisible(b); }
        projName.onClick = [] { auto& s = *ctx().session; askText("Song name", {}, s.project.name, "Rename", [&s](const juce::String& n) { s.checkpoint("Rename song"); s.project.name = n; s.changed(); }); };
        projTempo.onClick = [] { auto& s = *ctx().session; askText("Tempo", "20 to 300 BPM", juce::String(s.project.tempo.front().bpm), "Set", [&s](const juce::String& t) { s.checkpoint("Tempo"); s.project.tempo.front().bpm = juce::jlimit(20.0, 300.0, t.getDoubleValue()); s.changed(); }); };
        projSig.onClick = [] { auto& s = *ctx().session; askText("Time signature", "e.g. 4/4, 3/4, 6/8", juce::String(s.project.timeSig.front().num) + "/" + juce::String(s.project.timeSig.front().den), "Set", [&s](const juce::String& t) {
            const int n = t.upToFirstOccurrenceOf("/", false, false).getIntValue(), d = t.fromFirstOccurrenceOf("/", false, false).getIntValue();
            if (n < 1 || n > 32 || (d != 2 && d != 4 && d != 8 && d != 16)) { ctx().toast("Use a format like 4/4 or 6/8"); return; }
            s.checkpoint("Time signature"); s.project.timeSig.front().num = n; s.project.timeSig.front().den = d; s.changed(true); }); };
        projKey.onClick = [this] {
            juce::PopupMenu km, sm, m;
            auto& s = *ctx().session;
            for (int k = 0; k < 12; ++k) km.addItem(100 + k, Theory::keyNames()[k], true, s.project.key == k);
            for (int i = 0; i < (int)Theory::scales().size(); ++i) sm.addItem(200 + i, Theory::scales()[(size_t)i].name, true, s.project.scale == i);
            m.addSubMenu("Key", km); m.addSubMenu("Scale", sm);
            showMenu(m, &projKey, [&s](int r) { if (r >= 100 && r < 112) { s.checkpoint("Key"); s.project.key = r - 100; s.changed(); } else if (r >= 200) { s.checkpoint("Scale"); s.project.scale = r - 200; s.changed(); } });
        };
        projLen.onClick = [] { auto& s = *ctx().session; askText("Song length", "Planned length in bars (used for the loop and view)", juce::String(s.project.lengthBars), "Set", [&s](const juce::String& t) { s.checkpoint("Length"); s.project.lengthBars = juce::jlimit(1, 999, t.getIntValue()); s.changed(); }); };
        // audio device
        selector = std::make_unique<juce::AudioDeviceSelectorComponent>(ctx().engine->deviceManager(), 0, 2, 2, 2, false, false, true, false);
        addAndMakeVisible(*selector);
        addAndMakeVisible(inputB);
        inputB.onClick = [this] { ctx().engine->enableInput([this](bool ok) { ctx().toast(ok ? "Audio input on" : "Microphone access denied"); resized(); }); };
        // MIDI
        addAndMakeVisible(clearMap);
        clearMap.onClick = [] { ctx().engine->clearMidiMappings(); ctx().toast("MIDI controller mappings cleared"); };
        addAndMakeVisible(btMidi);
        btMidi.setVisible(juce::BluetoothMidiDevicePairingDialogue::isAvailable());
        btMidi.onClick = [] { juce::BluetoothMidiDevicePairingDialogue::open(); };
        addAndMakeVisible(refreshMidi);
        refreshMidi.onClick = [this] { buildMidiToggles(); resized(); };
        buildMidiToggles();
        // storage
        for (auto* b : { &clearCache, &regenPacks, &openFolder, &tutorialB }) addAndMakeVisible(b);
        clearCache.onClick = [] { SampleBank::get().purgeUnused(); ctx().toast("Unused samples released from memory"); };
        regenPacks.onClick = [] {
            Paths::library().getChildFile("Packs/.generated-v1").deleteFile();
            juce::Thread::launch([] { SampleBank::get().ensureBuiltinPacks(); juce::MessageManager::callAsync([] { ctx().toast("Built-in sample packs are ready"); }); });
        };
        openFolder.onClick = [] {
           #if JUCE_IOS || JUCE_ANDROID
            ctx().toast("Open the Files app: On My iPhone > Ostinato");
           #else
            Paths::root().revealToUser();
           #endif
        };
        tutorialB.onClick = [] { if (auto* m = dynamic_cast<MainComponent*>(ctx().root)) m->startTutorial(); };
        startTimerHz(4);
    }
    void buildMidiToggles() {
        midiToggles.clear();
        for (auto& d : ctx().engine->midiInputs()) {
            auto* t = midiToggles.add(new juce::ToggleButton(d.name));
            t->setToggleState(ctx().engine->isMidiInputEnabled(d.identifier), juce::dontSendNotification);
            t->onClick = [t, id = d.identifier] { ctx().engine->setMidiInputEnabled(id, t->getToggleState()); };
            addAndMakeVisible(t);
        }
    }
    void timerCallback() override { repaint(diagArea); }

    struct Section { juce::String title; int y; };
    std::vector<Section> sections;
    juce::Rectangle<int> diagArea, storageArea, aboutArea, keysArea;

    int layout(int w) {
        sections.clear();
        int y = 0;
        auto sec = [&](const juce::String& t) { sections.push_back({ t, y }); y += 40; };
        auto row = [&](juce::Component& c, int h = 46) { c.setBounds(0, y, w, h); y += h + 4; };
        auto pair = [&](juce::Component& a, juce::Component& b) { a.setBounds(0, y, w / 2 - 3, 46); b.setBounds(w / 2 + 3, y, w / 2 - 3, 46); y += 50; };
        sec("This song");
        pair(projName, projTempo);
        pair(projSig, projKey);
        row(projLen);
        sec("Audio");
        const int selH = juce::jmax(220, selector->getHeight());
        selector->setBounds(0, y, w, selH); y += selH + 4;
        inputB.setVisible(!ctx().engine->inputEnabled());
        if (inputB.isVisible()) row(inputB);
        row(bgAudioT, 40);
        sec("MIDI");
        for (auto* t : midiToggles) row(*t, 40);
        noMidiY = midiToggles.isEmpty() ? y : -1;
        if (midiToggles.isEmpty()) y += 30;
        row(clockT, 40);
        pair(clearMap, refreshMidi);
        if (btMidi.isVisible()) row(btMidi);
        sec("Interface");
        row(darkT, 40); row(hapticT, 40); row(tipT, 40); row(followT, 40); row(lowPowerT, 40);
        row(scale);
        row(tutorialB);
        sec("New songs and saving");
        row(templ); row(defBpm); row(autosave);
        sec("Storage");
        storageArea = { 0, y, w, 110 }; y += 114;
        pair(clearCache, regenPacks);
        row(openFolder);
        sec("Diagnostics");
        diagArea = { 0, y, w, 210 }; y += 214;
        sec("Keyboard shortcuts");
        keysArea = { 0, y, w, 220 }; y += 224;
        sec("About");
        aboutArea = { 0, y, w, 150 }; y += 160;
        return y;
    }
    int noMidiY = -1;
    void resized() override { layout(getWidth()); }
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        for (auto& s : sections) {
            heading(g, s.title, { 0, s.y + 8, getWidth(), 26 }, 18.f);
            g.setColour(p.line); g.fillRect(0, s.y + 36, getWidth(), 1);
        }
        auto& s = *ctx().session;
        auto& e = *ctx().engine;
        projName.setButtonText("Name: " + s.project.name);
        projTempo.setButtonText("Tempo: " + juce::String(s.project.tempo.front().bpm, 1) + " BPM");
        projSig.setButtonText("Time signature: " + juce::String(s.project.timeSig.front().num) + "/" + juce::String(s.project.timeSig.front().den));
        projKey.setButtonText("Key: " + Theory::keyNames()[s.project.key] + " " + Theory::scales()[(size_t)juce::jlimit(0, (int)Theory::scales().size() - 1, s.project.scale)].name);
        projLen.setButtonText("Length: " + juce::String(s.project.lengthBars) + " bars");
        g.setFont(font(14.f));
        if (noMidiY >= 0) { g.setColour(p.dim); g.drawText("No MIDI devices found. Connect a keyboard by USB or Bluetooth, then tap Rescan.", 0, noMidiY, getWidth(), 26, juce::Justification::centredLeft); }
        auto lines = [&](juce::Rectangle<int> r, const juce::StringArray& ls) {
            g.setColour(p.text.withAlpha(0.85f));
            for (auto& l : ls) { g.drawFittedText(l, r.removeFromTop(22), juce::Justification::centredLeft, 1); }
        };
        lines(storageArea, { "Songs: " + Paths::projects().getFullPathName(), "Samples and imports: " + Paths::library().getFullPathName(), "Exports: " + Paths::exports().getFullPathName(),
                             "Samples in memory: " + juce::File::descriptionOfSizeInBytes((juce::int64)SampleBank::get().memoryBytes()) + ", undo history: " + juce::File::descriptionOfSizeInBytes((juce::int64)s.undoBytes()) });
        lines(diagArea, { "Audio device: " + e.deviceName(),
                          "Sample rate " + juce::String(e.sampleRate(), 0) + " Hz, buffer " + juce::String(e.blockSize()) + " samples (" + juce::String(e.blockSize() * 1000.0 / juce::jmax(1.0, e.sampleRate()), 1) + " ms)",
                          "Reported latency " + juce::String(e.reportedLatencyMs(), 1) + " ms, recording offset " + juce::String(AppSettings::get().latencyOffsetMs, 0) + " ms",
                          "Audio CPU " + juce::String(e.cpuUsage() * 100.0, 1) + "%, dropouts (xruns) " + juce::String(juce::jmax(0, e.xruns())),
                          "Voices playing " + juce::String(e.voices()) + ", engine snapshots built " + juce::String(e.snapshotsBuilt()) + (e.isIdle() ? ", engine idle (battery saver)" : ""),
                          "MIDI: " + juce::String(e.midiActivity.load()) + " messages received, " + juce::String(e.numMidiMappings()) + " controller mappings" + (e.midiClockBpm() > 0 ? ", clock " + juce::String(e.midiClockBpm(), 1) + " BPM" : juce::String()),
                          "Last autosave: " + (s.lastAutosave.toMilliseconds() > 0 ? s.lastAutosave.formatted("%H:%M:%S") : juce::String("not yet")) + (s.lastAutosaveError.isNotEmpty() ? " (" + s.lastAutosaveError + ")" : juce::String()),
                          "Unsaved changes: " + juce::String(s.isDirty() ? "yes" : "no") });
        lines(keysArea, { "Space: play / pause      Enter: back to start      R: record      L: loop      M: metronome",
                          "Ctrl/Cmd+Z: undo      Ctrl/Cmd+Shift+Z or Ctrl/Cmd+Y: redo      Ctrl/Cmd+S: save",
                          "Ctrl/Cmd+1 to 0: switch screens      F1: tutorial",
                          "Song: Delete, Ctrl/Cmd+C / V / D, S split at playhead, B draw mode, arrows nudge, + / - zoom",
                          "Piano roll: arrows move notes (Shift = octave), Q quantize, Ctrl/Cmd+A select all",
                          "Instruments: A W S E D F T G Y H U J K play notes, Z / X change octave",
                          "Mouse: wheel scrolls, Shift+wheel scrolls sideways, Ctrl+wheel zooms, right-click = long-press",
                          "Touch: pinch to zoom, long-press for options, double-tap to open or reset" });
        lines(aboutArea, { "Ostinato 1.0. Built with JUCE 9 (AGPLv3 or commercial JUCE licence).",
                           "Inter typeface by Rasmus Andersson (SIL Open Font License 1.1).",
                           "All instruments, drum sounds and sample packs are synthesized by Ostinato itself,",
                           "so everything you make with them is yours to release.",
                           "Not available yet: MP3 export, AU/VST plug-in hosting." });
    }
    ParamSetPtr dummy;
    juce::ToggleButton darkT { "Dark theme" }, hapticT { "Haptic feedback" }, tipT { "Tooltips (desktop)" }, followT { "Follow the playhead" },
        lowPowerT { "Save battery when idle" }, bgAudioT { "Keep playing in the background" }, clockT { "Follow external MIDI clock (start/stop/tempo)" };
    Picker scale, autosave, templ, defBpm;
    juce::TextButton projName, projTempo, projSig, projKey, projLen;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
    juce::TextButton inputB { "Turn on audio input" }, clearMap { "Clear MIDI mappings" }, btMidi { "Pair Bluetooth MIDI device" }, refreshMidi { "Rescan MIDI devices" };
    juce::TextButton clearCache { "Release sample memory" }, regenPacks { "Rebuild built-in packs" }, openFolder { "Open Ostinato folder" }, tutorialB { "Show the tutorial again" };
    juce::OwnedArray<juce::ToggleButton> midiToggles;
};

class SettingsScreen : public Screen {
public:
    SettingsScreen() {
        addAndMakeVisible(view);
        view.setScrollBarsShown(true, false);
        view.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
    }
    juce::String title() const override { return "Settings"; }
    void onShow() override {
        if (!content) { content = std::make_unique<Content>(); view.setViewedComponent(content.get(), false); }
        resized();
        content->repaint();
    }
    void refresh() override { if (content) content->repaint(); }
    void paint(juce::Graphics& g) override { g.fillAll(pal().bg); }
    void resized() override {
        auto r = getLocalBounds().reduced(14, 8);
        view.setBounds(r);
        if (content) {
            const int w = juce::jmin(720, r.getWidth() - 12);
            content->setSize(w, content->layout(w) + 40);
        }
    }
private:
    juce::Viewport view;
    std::unique_ptr<Content> content;
};

}  // namespace

std::unique_ptr<Screen> makeSettingsScreen() { return std::make_unique<SettingsScreen>(); }
