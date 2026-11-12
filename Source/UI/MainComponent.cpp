#include "MainComponent.h"
#include "Screens/Screens.h"
#include "Core/Settings.h"
#include "Core/Theory.h"

using namespace UI;

// =================================================================== transport bar
class TransportBar : public juce::Component, private juce::Timer {
public:
    TransportBar(Session& s, AudioEngine& e, MainComponent& m) : session(s), engine(e), main(m) {
        for (auto* b : { &play, &stop, &rec, &loop, &metro, &undoB, &redoB, &menuB }) addAndMakeVisible(b);
        play.setOnColour(pal().play);
        rec.setOnColour(pal().record);
        play.onClick = [this] { haptic(1); engine.togglePlay(); };
        stop.onClick = [this] {
            haptic(0);
            if (engine.isRecording()) engine.stopRecording();
            if (engine.isPlaying()) engine.stop(); else engine.returnToStart();
            if (!session.project.previewTrack.isEmpty()) { session.project.previewTrack.clear(); engine.rebuild(); }
        };
        stop.setTooltip("Stop (tap again: back to start)");
        rec.onClick = [this] {
            haptic(2);
            if (engine.isRecording()) { engine.stopRecording(); return; }
            bool needsInput = false;
            for (auto& t : session.project.tracks) if (t.armed && t.type == TrackType::Audio) needsInput = true;
            auto start = [this] {
                juce::String err;
                if (!engine.startRecording(err)) { main.toast(err); main.go(ScreenId::Record); }
            };
            if (needsInput && !engine.inputEnabled())
                engine.enableInput([this, start](bool ok) { if (ok) start(); else main.toast("Microphone access is off. Turn it on in system settings to record audio."); });
            else start();
        };
        loop.setClickingTogglesState(false);
        loop.onClick = [this] { session.checkpoint("Toggle loop"); session.project.loopOn = !session.project.loopOn; session.changed(); };
        metro.onClick = [this] { session.project.metronome = !session.project.metronome; engine.setMetronome(session.project.metronome); session.changed(); };
        undoB.onClick = [this] { main.undo(); };
        redoB.onClick = [this] { main.redo(); };
        menuB.onClick = [this] { showMenu(); };
        addAndMakeVisible(posLabel);
        addAndMakeVisible(bpmLabel);
        bpmLabel.onTap = [this] { editTempo(); };
        posLabel.onTap = [this] { timeMode = !timeMode; };
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(pal().surface);
        g.setColour(pal().line);
        g.fillRect(0, getHeight() - 1, getWidth(), 1);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(6, 4);
        const bool narrow = getWidth() < 560;
        const int b = touch;
        play.setBounds(r.removeFromLeft(b + 8));
        stop.setBounds(r.removeFromLeft(b));
        rec.setBounds(r.removeFromLeft(b));
        menuB.setBounds(r.removeFromRight(b));
        if (!narrow) {
            redoB.setBounds(r.removeFromRight(b)); undoB.setBounds(r.removeFromRight(b));
            metro.setBounds(r.removeFromRight(b)); loop.setBounds(r.removeFromRight(b));
        } else {   // phones: loop/metronome/redo move into the menu so the position stays readable
            redoB.setBounds({}); metro.setBounds({}); loop.setBounds({});
            undoB.setBounds(r.removeFromRight(b));
        }
        r.removeFromLeft(6);
        bpmLabel.setBounds(r.removeFromRight(narrow ? 78 : 96));
        posLabel.setBounds(r);
    }

    void timerCallback() override {
        play.setToggleState(engine.isPlaying(), juce::dontSendNotification);
        play.setIcon(engine.isPlaying() ? "pause" : "play");
        rec.setToggleState(engine.isRecording(), juce::dontSendNotification);
        loop.setToggleState(session.project.loopOn, juce::dontSendNotification);
        metro.setToggleState(session.project.metronome, juce::dontSendNotification);
        undoB.setEnabled(session.canUndo());
        redoB.setEnabled(session.canRedo());
        const double tick = engine.playheadTick();
        juce::String pos;
        if (timeMode) {
            const double sec = session.project.ticksToSeconds(tick);
            pos = juce::String((int)sec / 60) + ":" + juce::String(std::fmod(sec, 60.0), 2).paddedLeft('0', 5);
        } else pos = session.project.formatPosition(tick);
        if (engine.isCountingIn()) pos = "Count-in";
        posLabel.set(pos, session.project.previewTrack.isNotEmpty() ? "Preview" : (timeMode ? "Time" : "Bar . beat"));
        const auto ts = session.project.timeSigAtBar(session.project.tickToBar((int)tick));
        bpmLabel.set(juce::String(session.project.bpmAt((int)tick), session.project.bpmAt((int)tick) == std::floor(session.project.bpmAt((int)tick)) ? 0 : 1),
                     juce::String(ts.num) + "/" + juce::String(ts.den) + "  BPM");
    }

    void editTempo() {
        juce::PopupMenu m;
        m.addSectionHeader("Tempo " + juce::String(session.project.tempo.front().bpm, 1) + " BPM");
        m.addItem(1, "Set tempo...");
        m.addItem(2, "Tap tempo (tap this item repeatedly)");
        m.addItem(3, "Time signature...");
        m.addItem(4, "Add tempo change at playhead...");
        m.addItem(5, "Key and scale...");
        if (session.project.tempo.size() > 1) m.addItem(6, "Remove all tempo changes");
        UI::showMenu(m, &bpmLabel, [this](int r) {
            if (r == 1) askText("Tempo", "Beats per minute (20 to 300)", juce::String(session.project.tempo.front().bpm), "Set", [this](const juce::String& t) {
                const double v = t.getDoubleValue();
                if (v < 20 || v > 300) { main.toast("Tempo must be between 20 and 300 BPM"); return; }
                session.checkpoint("Change tempo"); session.project.tempo.front().bpm = v; session.changed();
            });
            else if (r == 2) { const double bpm = tap.tap(juce::Time::getMillisecondCounterHiRes()); if (bpm > 0) { session.checkpoint("Tap tempo"); session.project.tempo.front().bpm = std::round(bpm * 10) / 10; session.changed(); main.toast("Tempo " + juce::String(bpm, 1) + " BPM"); } else main.toast("Tap again to the beat"); editTempo(); }
            else if (r == 3) askText("Time signature", "For example 4/4, 3/4, 6/8, 7/8", juce::String(session.project.timeSig.front().num) + "/" + juce::String(session.project.timeSig.front().den), "Set", [this](const juce::String& t) {
                const int n = t.upToFirstOccurrenceOf("/", false, false).getIntValue(), d = t.fromFirstOccurrenceOf("/", false, false).getIntValue();
                if (n < 1 || n > 32 || (d != 2 && d != 4 && d != 8 && d != 16)) { main.toast("Use a format like 4/4 or 6/8"); return; }
                session.checkpoint("Time signature"); session.project.timeSig.front().num = n; session.project.timeSig.front().den = d; session.changed(true);
            });
            else if (r == 4) askText("Tempo change", "New tempo from the bar at the playhead", juce::String(session.project.bpmAt((int)engine.playheadTick())), "Add", [this](const juce::String& t) {
                const double v = juce::jlimit(20.0, 300.0, t.getDoubleValue());
                const int bar = session.project.tickToBar((int)engine.playheadTick());
                const int tick = session.project.barToTick(bar);
                session.checkpoint("Add tempo change");
                auto& tp = session.project.tempo;
                tp.erase(std::remove_if(tp.begin(), tp.end(), [tick](auto& x) { return x.tick == tick && tick != 0; }), tp.end());
                if (tick == 0) tp.front().bpm = v; else tp.push_back({ tick, v });
                std::sort(tp.begin(), tp.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
                session.changed(true);
                main.toast("Tempo " + juce::String(v, 1) + " from bar " + juce::String(bar + 1));
            });
            else if (r == 5) {
                juce::PopupMenu km, sm;
                for (int k = 0; k < 12; ++k) km.addItem(100 + k, Theory::keyNames()[k], true, session.project.key == k);
                for (int s = 0; s < (int)Theory::scales().size(); ++s) sm.addItem(200 + s, Theory::scales()[(size_t)s].name, true, session.project.scale == s);
                juce::PopupMenu m2; m2.addSubMenu("Key", km); m2.addSubMenu("Scale", sm);
                UI::showMenu(m2, &bpmLabel, [this](int v) {
                    if (v >= 100 && v < 112) { session.checkpoint("Key"); session.project.key = v - 100; session.changed(); }
                    if (v >= 200) { session.checkpoint("Scale"); session.project.scale = v - 200; session.changed(); }
                });
            }
            else if (r == 6) { session.checkpoint("Remove tempo changes"); session.project.tempo.resize(1); session.changed(true); }
        });
    }

    void showMenu() {
        juce::PopupMenu m;
        m.addItem(1, "Save", session.hasProject());
        m.addItem(2, "Save as...", session.hasProject());
        m.addItem(3, "Projects");
        m.addSeparator();
        m.addItem(4, "Undo " + session.undoName(), session.canUndo());
        m.addItem(5, "Redo " + session.redoName(), session.canRedo());
        juce::PopupMenu hist;
        auto h = session.history();
        for (int i = h.size(); --i >= juce::jmax(0, h.size() - 15);) hist.addItem(1000 + i, h[i]);
        m.addSubMenu("Undo history", hist, h.size() > 0);
        m.addSeparator();
        if (getWidth() < 560) {
            m.addItem(10, "Loop", true, session.project.loopOn);
            m.addItem(11, "Metronome", true, session.project.metronome);
        }
        m.addItem(6, "Write automation while playing", true, main.automationWrite);
        m.addItem(7, "Count-in before recording", true, session.project.countInBars > 0);
        m.addSeparator();
        m.addItem(8, "Help and tutorial");
        m.addItem(9, "Settings");
        UI::showMenu(m, &menuB, [this, h](int r) {
            if (r == 1) main.save();
            else if (r == 2) askText("Save as", "Name for the copy", session.project.name + " copy", "Save", [this](const juce::String& n) { juce::String e; if (session.saveAs(n, e)) main.toast("Saved as " + n); else main.toast(e); });
            else if (r == 3) main.go(ScreenId::Home);
            else if (r == 4) main.undo();
            else if (r == 5) main.redo();
            else if (r == 6) { main.automationWrite = !main.automationWrite; main.toast(main.automationWrite ? "Automation write on: moving a control while playing records it" : "Automation write off"); }
            else if (r == 7) { session.project.countInBars = session.project.countInBars > 0 ? 0 : 1; session.changed(); }
            else if (r == 8) main.startTutorial();
            else if (r == 9) main.go(ScreenId::Settings);
            else if (r == 10) loop.triggerClick();
            else if (r == 11) metro.triggerClick();
            else if (r >= 1000) { const int steps = h.size() - (r - 1000); for (int i = 0; i < steps; ++i) session.undo(); main.toast("Undid " + juce::String(steps) + " step" + (steps > 1 ? "s" : "")); }
        });
    }

private:
    struct Readout : juce::Component {
        juce::String big, small;
        std::function<void()> onTap;
        void set(const juce::String& b, const juce::String& s) { if (b != big || s != small) { big = b; small = s; repaint(); } }
        void paint(juce::Graphics& g) override {
            auto r = getLocalBounds().reduced(4, 2);
            g.setColour(pal().accent);
            g.setFont(font(r.getHeight() > 40 ? 24.f : 21.f, true));
            g.drawText(big, r.removeFromTop(r.getHeight() * 62 / 100), juce::Justification::bottomLeft, false);
            g.setColour(pal().dim);
            g.setFont(font(11.5f));
            g.drawText(small, r, juce::Justification::topLeft, false);
        }
        void mouseUp(const juce::MouseEvent& e) override { if (getLocalBounds().contains(e.getPosition()) && onTap) onTap(); }
    };
    Session& session;
    AudioEngine& engine;
    MainComponent& main;
    IconButton play { "play", {}, "Play / pause (Space)" }, stop { "stop" }, rec { "record", {}, "Record (R)" }, loop { "loop", {}, "Loop region (L)" },
        metro { "metronome", {}, "Metronome (M)" }, undoB { "undo", {}, "Undo (Ctrl+Z)" }, redoB { "redo", {}, "Redo (Ctrl+Shift+Z)" }, menuB { "more", {}, "More" };
    Readout posLabel, bpmLabel;
    Theory::TapTempo tap;
    bool timeMode = false;
};

// =================================================================== navigation
class NavBar : public juce::Component {
public:
    explicit NavBar(MainComponent& m) : main(m) {
        struct Item { ScreenId id; const char* icon; const char* name; };
        const Item all[] = { { ScreenId::Home, "home", "Projects" }, { ScreenId::Arrange, "song", "Song" }, { ScreenId::PianoRoll, "keys", "Piano roll" },
                             { ScreenId::Drums, "drums", "Drums" }, { ScreenId::Mixer, "mixer", "Mixer" }, { ScreenId::Instruments, "instrument", "Instruments" },
                             { ScreenId::Effects, "fx", "Effects" }, { ScreenId::Samples, "samples", "Samples" }, { ScreenId::Record, "mic", "Record" },
                             { ScreenId::Export, "export", "Export" }, { ScreenId::Settings, "settings", "Settings" } };
        for (auto& it : all) {
            auto* b = buttons.add(new IconButton(it.icon, it.name));
            b->setCaptionBelow(true);
            b->setFilledWhenOn(false);
            b->setQuiet(true);
            b->onClick = [this, id = it.id] { haptic(0); main.go(id); };
            ids.push_back(it.id);
            addAndMakeVisible(b);
        }
        addAndMakeVisible(more);
        more.setCaptionBelow(true);
        more.setQuiet(true);
        more.setFilledWhenOn(false);
        more.onClick = [this] {
            juce::PopupMenu m;
            for (size_t i = 0; i < ids.size(); ++i) if (!isPrimary(ids[i])) m.addItem((int)i + 1, buttons[(int)i]->getTooltip(), true, main.current() == ids[i]);
            UI::showMenu(m, &more, [this](int r) { if (r > 0) main.go(ids[(size_t)r - 1]); });
        };
    }
    static bool isPrimary(ScreenId id) { return id == ScreenId::Arrange || id == ScreenId::PianoRoll || id == ScreenId::Drums || id == ScreenId::Mixer; }
    void setVertical(bool v) { vertical = v; resized(); }
    void setCurrent(ScreenId id) {
        for (size_t i = 0; i < ids.size(); ++i) buttons[(int)i]->setToggleState(ids[i] == id, juce::dontSendNotification);
        more.setToggleState((!vertical || condensed) && !isPrimary(id), juce::dontSendNotification);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(pal().surface);
        g.setColour(pal().line);
        if (vertical) g.fillRect(getWidth() - 1, 0, 1, getHeight()); else g.fillRect(0, 0, getWidth(), 1);
    }
    void resized() override {
        auto r = getLocalBounds();
        if (vertical) {
            r.removeFromTop(4);
            const bool fitsAll = r.getHeight() >= (int)ids.size() * 48;
            condensed = !fitsAll;
            more.setVisible(!fitsAll);
            const int n = fitsAll ? (int)ids.size() : 5;
            const int h = juce::jlimit(44, 64, (r.getHeight() - 4) / n);
            for (size_t i = 0; i < ids.size(); ++i) {
                const bool show = fitsAll || isPrimary(ids[i]);
                buttons[(int)i]->setVisible(show);
                if (show) buttons[(int)i]->setBounds(r.removeFromTop(h).reduced(4, 1));
            }
            if (!fitsAll) more.setBounds(r.removeFromTop(h).reduced(4, 1));
        } else {
            // phone: Song, Piano roll, Drums, Mixer + More
            more.setVisible(true);
            const int n = 5;
            const int w = r.getWidth() / n;
            auto row = r.withHeight(juce::jmin(r.getHeight(), 62));
            int k = 0;
            for (size_t i = 0; i < ids.size(); ++i) {
                const bool show = isPrimary(ids[i]);
                buttons[(int)i]->setVisible(show);
                if (show) buttons[(int)i]->setBounds(row.withX(k++ * w).withWidth(w).reduced(2, 4));
            }
            more.setBounds(row.withX(k * w).withWidth(r.getWidth() - k * w).reduced(2, 4));
        }
    }
private:
    MainComponent& main;
    juce::OwnedArray<IconButton> buttons;
    std::vector<ScreenId> ids;
    IconButton more { "more", "More" };
    bool vertical = true, condensed = false;
};

// =================================================================== tutorial
class Tutorial : public juce::Component {
public:
    explicit Tutorial(MainComponent& m) : main(m) {
        addAndMakeVisible(next); addAndMakeVisible(skip);
        next.onClick = [this] { if (++step >= (int)steps().size()) finish(); else { show(); } };
        skip.onClick = [this] { finish(); };
        setInterceptsMouseClicks(true, true);
    }
    struct Step { ScreenId screen; const char* title; const char* body; };
    static const std::vector<Step>& steps() {
        static const std::vector<Step> s = {
            { ScreenId::Home, "Welcome to Ostinato", "Every song is a project. Tap \"New song\" to start one from a template, or open a recent project. Everything saves automatically." },
            { ScreenId::Arrange, "Arrange your song", "Each row is a track. Tap the pencil, then tap a lane to draw a clip. Drag clips to move them, drag their edges to resize, pinch to zoom. Long-press anything for more options." },
            { ScreenId::Drums, "Build a beat", "Tap steps to turn them on. Long-press a step to set velocity, probability, pitch and ratchets. \"Add to song\" places the pattern at the playhead." },
            { ScreenId::PianoRoll, "Write melodies", "Tap the grid to draw notes, drag to move, drag the right edge to resize. Turn on the scale guide or chord tool to stay in key." },
            { ScreenId::Mixer, "Mix", "Faders, pan, sends and effects for every track. Tap an insert slot to edit effects. Long-press any knob to automate it or map it to a MIDI controller." },
            { ScreenId::Export, "Share your song", "Export a WAV, FLAC or compressed file. Exports include every track, effect and automation, plus reverb tails." },
        };
        return s;
    }
    void start() { step = 0; setVisible(true); toFront(true); show(); }
    void show() { main.go(steps()[(size_t)step].screen); toFront(false); next.setButtonText(step == (int)steps().size() - 1 ? "Start making music" : "Next"); repaint(); resized(); }
    void finish() { setVisible(false); AppSettings::get().tutorialDone = true; AppSettings::get().save(); main.go(ScreenId::Home); }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black.withAlpha(0.55f));
        auto card = cardBounds().toFloat();
        g.setColour(pal().surface);
        g.fillRoundedRectangle(card, rSheet);
        auto r = card.reduced(22.f).toNearestInt();
        g.setColour(pal().dim);
        g.setFont(font(13.f));
        g.drawText(juce::String(step + 1) + " of " + juce::String((int)steps().size()), r.removeFromTop(20), juce::Justification::centredLeft);
        g.setColour(pal().text);
        g.setFont(font(22.f, true));
        g.drawFittedText(steps()[(size_t)step].title, r.removeFromTop(36), juce::Justification::centredLeft, 1);
        g.setFont(font(16.f));
        g.setColour(pal().text.withAlpha(0.85f));
        g.drawFittedText(steps()[(size_t)step].body, r.withTrimmedBottom(60), juce::Justification::topLeft, 6);
    }
    juce::Rectangle<int> cardBounds() const { const int w = juce::jmin(getWidth() - 32, 460); return { (getWidth() - w) / 2, getHeight() - 300, w, 268 }; }
    void resized() override {
        auto c = cardBounds().reduced(22);
        auto row = c.removeFromBottom(touch);
        next.setBounds(row.removeFromRight(juce::jmin(220, row.getWidth() / 2)));
        skip.setBounds(row.removeFromLeft(100));
    }
private:
    MainComponent& main;
    juce::TextButton next { "Next" }, skip { "Skip" };
    int step = 0;
};

// =================================================================== main component
MainComponent::MainComponent(Session& s, AudioEngine& e) : session(s), engine(e) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    ctx().root = this;
    ctx().toast = [this](const juce::String& t) { toast(t); };
    ctx().go = [this](ScreenId id) { go(id); };
    ctx().refreshAll = [this] { for (auto& sc : screens) if (sc) sc->refresh(); };
    ctx().beginEdit = [this](const juce::String& what) { if (!editOpen) { session.checkpoint(what); editOpen = true; editName = what; } };
    ctx().endEdit = [this] { if (editOpen) { editOpen = false; session.changed(); } };
    ctx().paramMoved = [this](const ParamSet* ps, int idx, float norm) {
        if (!automationWrite || !engine.isPlaying()) return;
        // find the lane target for this parameter and write a point at the playhead
        const int tick = (int)engine.playheadTick();
        auto findTarget = [&](Track& t) -> juce::String {
            if (t.channel.get() == ps) return "ch:" + juce::String(ps->meta(idx).id);
            if (t.inst.params.get() == ps) return "inst:" + juce::String(ps->meta(idx).id);
            for (auto& fx : t.effects) if (fx.params.get() == ps) return "fx:" + fx.id + ":" + ps->meta(idx).id;
            return {};
        };
        auto write = [&](Track& t) {
            const auto target = findTarget(t);
            if (target.isEmpty()) return false;
            auto* lane = t.findLane(target);
            if (!lane) { t.automation.push_back({ target, {}, true, true }); lane = &t.automation.back(); }
            auto& pts = lane->points;
            // replace points written over in the last ~1/16 so the lane follows the gesture
            pts.erase(std::remove_if(pts.begin(), pts.end(), [tick](auto& p) { return p.tick > tick - PPQ / 8 && p.tick <= tick; }), pts.end());
            pts.push_back({ tick, norm, 0 });
            std::sort(pts.begin(), pts.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
            session.changed();
            return true;
        };
        for (auto& t : session.project.tracks) if (write(t)) return;
        write(session.project.master);
    };
    ctx().automate = [this](ParamSetPtr ps, int idx, const juce::String&, const juce::String&) {
        auto add = [&](Track& t, const juce::String& target) {
            if (!t.findLane(target)) {
                session.checkpoint("Add automation lane");
                AutomationLane l;
                l.target = target;
                const float v = ps->toNorm(idx, ps->getBase(idx));
                l.points = { { 0, v, 0 } };
                t.automation.push_back(l);
                t.collapsed = false;
                session.changed(true);
            }
            session.selectTrack(t.id);
            go(ScreenId::Arrange);
            toast("Automation lane added. Tap the lane to add points, drag them to shape the change.");
        };
        auto scan = [&](Track& t) {
            if (t.channel == ps) { add(t, "ch:" + juce::String(ps->meta(idx).id)); return true; }
            if (t.inst.params == ps) { add(t, "inst:" + juce::String(ps->meta(idx).id)); return true; }
            for (auto& fx : t.effects) if (fx.params == ps) { add(t, "fx:" + fx.id + ":" + ps->meta(idx).id); return true; }
            return false;
        };
        for (auto& t : session.project.tracks) if (scan(t)) return;
        if (scan(session.project.master)) return;
        toast("This control can't be automated.");
    };
    session.addListener(this);
    session.makeThumbnail = [this](const juce::File& f) { makeThumbnail(f); };

    transport = std::make_unique<TransportBar>(session, engine, *this);
    nav = std::make_unique<NavBar>(*this);
    addAndMakeVisible(*transport);
    addAndMakeVisible(*nav);
    screens[(size_t)ScreenId::Home] = makeHomeScreen();
    screens[(size_t)ScreenId::Arrange] = makeArrangeScreen();
    screens[(size_t)ScreenId::PianoRoll] = makePianoRollScreen();
    screens[(size_t)ScreenId::Drums] = makeDrumScreen();
    screens[(size_t)ScreenId::Mixer] = makeMixerScreen();
    screens[(size_t)ScreenId::Instruments] = makeInstrumentScreen();
    screens[(size_t)ScreenId::Effects] = makeEffectsScreen();
    screens[(size_t)ScreenId::Samples] = makeSampleScreen();
    screens[(size_t)ScreenId::Record] = makeRecordScreen();
    screens[(size_t)ScreenId::Export] = makeExportScreen();
    screens[(size_t)ScreenId::Settings] = makeSettingsScreen();
    for (auto& sc : screens) addChildComponent(*sc);
    tutorial = std::make_unique<Tutorial>(*this);
    addChildComponent(*tutorial);
    if (AppSettings::get().tooltips) tooltips = std::make_unique<juce::TooltipWindow>(this, 900);
    go(ScreenId::Home);
    startTimerHz(10);
}

MainComponent::~MainComponent() {
    session.removeListener(this);
    session.makeThumbnail = nullptr;
    ctx() = AppContext { ctx().session, ctx().engine };
}

void MainComponent::parentHierarchyChanged() { grabKeyboardFocus(); }

void MainComponent::applyTheme() {
    UI::setDark(AppSettings::get().darkTheme);
    if (auto* lf = dynamic_cast<UI::LookAndFeel*>(&getLookAndFeel())) lf->refreshColours();
    sendLookAndFeelChange();
    for (auto& sc : screens) if (sc) sc->refresh();
    repaint();
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(pal().bg);
    if (!toastText.isEmpty() && juce::Time::getMillisecondCounter() < toastUntil) {}  // drawn in paintOverChildren-like timer repaint below
}

void MainComponent::resized() {
    auto r = getLocalBounds();
    if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds())) {
        safe = d->safeAreaInsets;
        // only apply insets that overlap this window (full screen on phones)
        if (!getScreenBounds().contains(d->totalArea.reduced(2))) safe = {};
    }
    r = safe.subtractedFrom(r);
    const bool wide = r.getWidth() >= 700 || r.getWidth() > r.getHeight() * 1.25f;
    compact = !wide;
    transport->setBounds(r.removeFromTop(compact ? 58 : 64));
    if (wide) { nav->setVertical(true); nav->setBounds(r.removeFromLeft(86)); }
    else { nav->setVertical(false); nav->setBounds(r.removeFromBottom(64)); }
    for (auto& sc : screens) sc->setBounds(r);
    tutorial->setBounds(getLocalBounds());
}

void MainComponent::go(ScreenId id) {
    if (!session.hasProject() && id != ScreenId::Home && id != ScreenId::Settings) {
        toast("Open or create a project first");
        id = ScreenId::Home;
    }
    if (screens[(size_t)currentId] && screens[(size_t)currentId]->isVisible()) screens[(size_t)currentId]->onHide();
    for (size_t i = 0; i < screens.size(); ++i) screens[i]->setVisible(i == (size_t)id);
    currentId = id;
    nav->setCurrent(id);
    screens[(size_t)id]->onShow();
    grabKeyboardFocus();
}

void MainComponent::toast(const juce::String& text) {
    toastText = text;
    toastUntil = juce::Time::getMillisecondCounter() + (juce::uint32)juce::jlimit(1800, 5000, text.length() * 60);
    struct ToastView : juce::Component {
        juce::String text;
        std::function<void(ToastView&)> place;
        void parentSizeChanged() override { if (place) place(*this); }   // stays centred when the window turns
        void paint(juce::Graphics& g) override {
            g.setColour(pal().text);
            g.fillRoundedRectangle(getLocalBounds().toFloat(), 12.f);
            g.setColour(pal().bg);
            g.setFont(font(15.f, true));
            g.drawFittedText(text, getLocalBounds().reduced(16, 6), juce::Justification::centred, 3);
        }
    };
    static juce::Component::SafePointer<juce::Component> view;
    if (view) delete view.getComponent();
    auto* v = new ToastView();
    v->text = text;
    v->setInterceptsMouseClicks(false, false);
    v->place = [this](ToastView& tv) {
        const int w = juce::jmin(getWidth() - 32, juce::GlyphArrangement::getStringWidthInt(font(15.f, true), tv.text) + 48);
        const int h = w >= getWidth() - 32 ? 66 : 46;
        tv.setBounds((getWidth() - w) / 2, getHeight() - safe.getBottom() - (compact ? 140 : 90), w, h);
    };
    v->place(*v);
    addAndMakeVisible(v);
    view = v;
    juce::Timer::callAfterDelay((int)(toastUntil - juce::Time::getMillisecondCounter()), [sp = juce::Component::SafePointer<juce::Component>(v)] { if (sp) delete sp.getComponent(); });
}

void MainComponent::timerCallback() {
    juce::ignoreUnused(lastLowPower);
}

void MainComponent::save() {
    juce::String err;
    if (session.save(err)) { toast("Saved"); haptic(1); }
    else toast("Couldn't save: " + err);
}
void MainComponent::undo() { if (session.canUndo()) { const auto n = session.undoName(); session.undo(); toast("Undo: " + n); haptic(0); } }
void MainComponent::redo() { if (session.canRedo()) { const auto n = session.redoName(); session.redo(); toast("Redo: " + n); haptic(0); } }

void MainComponent::startTutorial() { tutorial->start(); }

void MainComponent::projectReplaced() { engine.setMetronome(session.project.metronome); }
void MainComponent::projectChanged(bool) {}

bool MainComponent::keyPressed(const juce::KeyPress& k) {
    if (screens[(size_t)currentId]->handleKey(k)) return true;
    const auto mods = k.getModifiers();
    const bool cmd = mods.isCommandDown();
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::spaceKey) { engine.togglePlay(); return true; }
    if (cmd && (code == 'Z' || code == 'z')) { mods.isShiftDown() ? redo() : undo(); return true; }
    if (cmd && (code == 'Y' || code == 'y')) { redo(); return true; }
    if (cmd && (code == 'S' || code == 's')) { save(); return true; }
    if (!cmd && (code == 'R' || code == 'r')) { juce::String err; if (engine.isRecording()) engine.stopRecording(); else if (!engine.startRecording(err)) toast(err); return true; }
    if (!cmd && (code == 'L' || code == 'l')) { session.project.loopOn = !session.project.loopOn; session.changed(); return true; }
    if (!cmd && (code == 'M' || code == 'm')) { session.project.metronome = !session.project.metronome; engine.setMetronome(session.project.metronome); session.changed(); return true; }
    if (code == juce::KeyPress::homeKey || code == juce::KeyPress::returnKey) { engine.returnToStart(); return true; }
    if (cmd && code >= '1' && code <= '9') { const int i = code - '1' + 1; if (i < (int)ScreenId::Count) go((ScreenId)i); return true; }
    if (cmd && code == '0') { go(ScreenId::Home); return true; }
    if (code == juce::KeyPress::F1Key) { startTutorial(); return true; }
    return false;
}

juce::Image MainComponent::snapshot(ScreenId id, int w, int h) {
    setSize(w, h);
    go(id);
    return createComponentSnapshot(getLocalBounds(), true, 2.f);
}

void MainComponent::makeThumbnail(const juce::File& folder) {
    // small arrangement picture: one coloured bar per clip
    const auto& p = session.project;
    juce::Image img(juce::Image::RGB, 320, 180, true);
    juce::Graphics g(img);
    g.fillAll(juce::Colour(0xff1f2125));
    const int end = juce::jmax(p.songEndTick(), PPQ * 16);
    const int n = juce::jmax(1, (int)p.tracks.size());
    const float rowH = juce::jmin(28.f, 164.f / n);
    for (int i = 0; i < (int)p.tracks.size(); ++i) {
        const auto& t = p.tracks[(size_t)i];
        for (auto& c : t.clips) {
            const float x = 8.f + 304.f * c.start / end, w = juce::jmax(3.f, 304.f * c.length / end);
            g.setColour(t.colour.withAlpha(c.muted ? 0.35f : 0.9f));
            g.fillRoundedRectangle(x, 8.f + i * rowH, w, rowH - 3.f, 3.f);
        }
    }
    juce::PNGImageFormat png;
    auto f = folder.getChildFile("thumbnail.png");
    f.deleteFile();
    juce::FileOutputStream os(f);
    if (os.openedOk()) png.writeImageToStream(img, os);
}
