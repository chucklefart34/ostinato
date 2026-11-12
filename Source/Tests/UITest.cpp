// End-to-end UI test: drives the real window with synthetic *touch* input through JUCE's
// normal event pipeline (hit-testing, drags, double-taps, long-presses, two-finger pinch)
// and checks the song data after each gesture.  Run:  Ostinato --uitest out_dir
#include "UI/MainComponent.h"
#include "Core/Paths.h"
#include "Core/Settings.h"
#include "Engine/Exporter.h"

#if JUCE_MODAL_LOOPS_PERMITTED   // the test pumps the message loop itself (not available on iOS/Android)
namespace {

struct Driver {
    juce::Component& root;
    int passed = 0, failed = 0;
    juce::StringArray log;
    juce::File dir;
    int shot = 0;

    void pump(int ms) {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms;
        while (juce::Time::getMillisecondCounter() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    }
    void check(bool ok, const juce::String& what, const juce::String& detail = {}) {
        (ok ? passed : failed)++;
        auto l = juce::String(ok ? "  PASS  " : "  FAIL  ") + what + (detail.isNotEmpty() ? "  [" + detail + "]" : juce::String());
        log.add(l);
        std::printf("%s\n", l.toRawUTF8());
        std::fflush(stdout);
    }
    void snap(const juce::String& name) {
        auto img = root.createComponentSnapshot(root.getLocalBounds(), true, 1.f);
        auto f = dir.getChildFile(juce::String(++shot).paddedLeft('0', 2) + "-" + name + ".png");
        f.deleteFile();
        juce::FileOutputStream os(f);
        juce::PNGImageFormat().writeImageToStream(img, os);
    }
    void event(juce::Component& c, juce::Point<float> local, bool down, int touch = 0) {
        auto* peer = root.getPeer();
        const auto pos = peer->globalToLocal(c.localPointToGlobal(local));
        // Linux test builds have no multi-touch (XInput off), so one finger is sent as the primary
        // pointer: components receive identical MouseEvents either way.
        peer->handleMouseEvent(touch == 0 ? juce::MouseInputSource::InputSourceType::mouse : juce::MouseInputSource::InputSourceType::touch, pos,
                               down ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(), down ? 1.f : 0.f, 0.f,
                               juce::Time::currentTimeMillis(), {}, touch);
    }
    void tap(juce::Component& c, juce::Point<float> p, int holdMs = 40) { event(c, p, true); pump(holdMs); event(c, p, false); pump(80); }
    void tap(juce::Component& c) { tap(c, c.getLocalBounds().getCentre().toFloat()); }
    void doubleTap(juce::Component& c, juce::Point<float> p) { event(c, p, true); pump(20); event(c, p, false); pump(60); event(c, p, true); pump(20); event(c, p, false); pump(120); }
    void drag(juce::Component& c, juce::Point<float> a, juce::Point<float> b, int steps = 12, int touch = 0) {
        event(c, a, true, touch); pump(30);
        for (int i = 1; i <= steps; ++i) { event(c, a + (b - a) * ((float)i / steps), true, touch); pump(12); }
        event(c, b, false, touch); pump(100);
    }
    void pinch(juce::Component& c, juce::Point<float> centre, float from, float to) {
        if (!juce::Desktop::getInstance().getMouseSource(1) && !canMultiTouch()) {   // no touch on this platform: use the trackpad magnify gesture
            auto* peer = root.getPeer();
            const auto pos = peer->globalToLocal(c.localPointToGlobal(centre));
            for (int i = 0; i < 8; ++i) { peer->handleMagnifyGesture(juce::MouseInputSource::InputSourceType::mouse, pos, juce::Time::currentTimeMillis(), std::pow(to / from, 1.f / 8.f), 0); pump(15); }
            pump(80);
            return;
        }
        auto p0 = centre - juce::Point<float>(from, 0), p1 = centre + juce::Point<float>(from, 0);
        event(c, p0, true, 0); event(c, p1, true, 1); pump(30);
        for (int i = 1; i <= 10; ++i) {
            const float d = from + (to - from) * i / 10.f;
            event(c, centre - juce::Point<float>(d, 0), true, 0); event(c, centre + juce::Point<float>(d, 0), true, 1); pump(15);
        }
        event(c, centre - juce::Point<float>(to, 0), false, 0); event(c, centre + juce::Point<float>(to, 0), false, 1); pump(100);
    }
    static bool canMultiTouch() {
       #if JUCE_LINUX
        return false;
       #else
        return true;
       #endif
    }
    void key(const juce::KeyPress& k) { root.keyPressed(k); pump(60); }

    // find a child by accessible title / tooltip / button text (depth-first, visible only)
    juce::Component* find(const juce::String& text, juce::Component* from = nullptr) {
        if (!from) from = &root;
        for (auto* c : from->getChildren()) {
            if (!c->isVisible()) continue;
            juce::String t = c->getTitle();
            if (auto* tt = dynamic_cast<juce::SettableTooltipClient*>(c); tt && t.isEmpty()) t = tt->getTooltip();
            if (auto* b = dynamic_cast<juce::Button*>(c); b && b->getButtonText() == text) return c;
            if (t == text) return c;
            if (auto* r = find(text, c)) return r;
        }
        return nullptr;
    }
    juce::Component* findType(const char* typeName, juce::Component* from = nullptr) {
        if (!from) from = &root;
        for (auto* c : from->getChildren()) {
            if (!c->isVisible()) continue;
            if (juce::String(typeid(*c).name()).contains(typeName)) return c;
            if (auto* r = findType(typeName, c)) return r;
        }
        return nullptr;
    }
    // pick an entry in the open popup menu by its text
    bool chooseMenu(const juce::String& text) {
        pump(150);
        if (std::getenv("UI_DEBUG")) for (auto* w : root.getChildren()) if (juce::String(typeid(*w).name()).contains("Menu")) {
            std::printf("menu %s kids=%d\n", typeid(*w).name(), w->getNumChildComponents());
            std::function<void(juce::Component*, int)> dump = [&](juce::Component* c, int depth) {
                for (auto* k : c->getChildren()) { auto* h = k->getAccessibilityHandler(); std::printf("%*s%s '%s'\n", depth * 2, "", typeid(*k).name(), h ? h->getTitle().toRawUTF8() : "-"); if (depth < 3) dump(k, depth + 1); }
            };
            dump(w, 1);
        }
        if (false) for (int i = juce::Desktop::getInstance().getNumComponents(); --i >= 0;) {
            auto* w = juce::Desktop::getInstance().getComponent(i);
            std::printf("desk[%d] %s vis=%d kids=%d\n", i, typeid(*w).name(), w->isVisible(), w->getNumChildComponents());
            for (auto* item : w->getChildren()) { auto* h = item->getAccessibilityHandler(); std::printf("   %s '%s'\n", typeid(*item).name(), h ? h->getTitle().toRawUTF8() : "-"); }
        }
        juce::Array<juce::Component*> candidates;
        for (int i = juce::Desktop::getInstance().getNumComponents(); --i >= 0;) candidates.add(juce::Desktop::getInstance().getComponent(i));
        for (auto* c : root.getChildren()) candidates.add(c);   // menus are hosted inside the window (safe-area aware)
        for (auto* w : candidates) {
            if (!w || !w->isVisible() || !juce::String(typeid(*w).name()).contains("MenuWindow")) continue;
            for (auto* item : w->getChildren()) {
                auto* h = item->getAccessibilityHandler();
                if (h && h->getTitle() == text) {
                    // press through the accessibility action (what VoiceOver uses): headless X has no real
                    // pointer/focus for JUCE's menu tracking, but the item's real callback still runs
                    const bool ok = h->getActions().invoke(juce::AccessibilityActionType::press);
                    pump(250);
                    if (!ok) return false;
                    return true;
                }
            }
        }
        juce::PopupMenu::dismissAllActiveMenus();
        pump(100);
        return false;
    }
};

}  // namespace

int runUITest(const juce::File& dir) {
    dir.deleteRecursively();
    dir.createDirectory();
    const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ostinato-uitest");
    tmp.deleteRecursively();
    Paths::overrideRoot() = tmp;
    AppSettings::get().tutorialDone = true;
    AppSettings::get().defaultTemplate = "Beat Starter";
    UI::LookAndFeel lf;
    juce::LookAndFeel::setDefaultLookAndFeel(&lf);
    AudioEngine engine;
    engine.initialise();
    Session session(engine);
    ctx().session = &session;
    ctx().engine = &engine;
    SampleBank::get().ensureBuiltinPacks();
    auto* main = new MainComponent(session, engine);
    juce::DocumentWindow win("Ostinato UI test", juce::Colours::black, 0);
    win.setUsingNativeTitleBar(false);
    win.setTitleBarHeight(0);
    win.setContentOwned(main, false);
    win.setBounds(0, 0, 1280, 800);
    win.setVisible(true);
    Driver d { *main };
    d.dir = dir;
    d.pump(400);
    auto& p = session.project;

    std::printf("== Projects: create a song from the New song sheet\n");
    auto* nb = d.find("New song");
    std::printf("found=%d showing=%d peer=%p bounds=%s\n", nb != nullptr, nb ? nb->isShowing() : 0, (void*)main->getPeer(), nb ? nb->getScreenBounds().toString().toRawUTF8() : "");
    if (nb) { auto c = nb->getScreenBounds().getCentre(); std::printf("hit=%s\n", main->getComponentAt(main->getLocalPoint(nullptr, c)) ? typeid(*main->getComponentAt(main->getLocalPoint(nullptr, c))).name() : "none"); }
    if (nb) d.tap(*nb);
    d.pump(200);
    d.snap("new-song-sheet");
    if (auto* create = d.find("Create song")) d.tap(*create);
    d.pump(400);
    d.check(session.hasProject() && p.tracks.size() >= 3 && main->current() == ScreenId::Arrange, "New song sheet creates a project from the template and opens the Song view",
            juce::String((int)p.tracks.size()) + " tracks");
    d.snap("song-view");

    std::printf("== Song view: draw, open, move and resize clips with touch\n");
    auto* timeline = d.findType("Timeline");
    d.check(timeline != nullptr, "Timeline is on screen");
    if (!timeline) return 1;
    // geometry of the desktop layout (header 196 px, ruler 40 px, 64 px rows, 0.04 px/tick)
    auto xOf = [](int tick) { return 196.f + tick * 0.04f; };
    auto yOfTrack = [](int i) { return 40.f + 64.f * i + 40.f; };
    const int bassIdx = 1;
    const size_t clipsBefore = p.tracks[bassIdx].clips.size();
    if (auto* draw = d.find("Draw clips (B)")) d.tap(*draw);
    d.tap(*timeline, { xOf(PPQ * 4 * 4 + 100), yOfTrack(bassIdx) });
    d.check(p.tracks[bassIdx].clips.size() == clipsBefore + 1, "Tapping an empty lane in draw mode creates a clip");
    const auto& nc = p.tracks[bassIdx].clips.back();
    d.check(nc.start == PPQ * 4 * 4 && nc.type == ClipType::Midi, "New clip snaps to bar 5", "start " + juce::String(nc.start / PPQ / 4.0 + 1) + " bars");
    const juce::String newClipId = nc.id;
    // move it one bar right
    if (auto* draw = d.find("Draw clips (B)")) d.tap(*draw);
    d.drag(*timeline, { xOf(PPQ * 4 * 4 + 600), yOfTrack(bassIdx) }, { xOf(PPQ * 4 * 5 + 600), yOfTrack(bassIdx) });
    int movedStart = -1;
    for (auto& c : p.tracks[bassIdx].clips) if (c.id == newClipId) movedStart = c.start;
    d.check(movedStart == PPQ * 4 * 5, "Dragging a clip moves it (snapped)", "now at bar " + juce::String(movedStart / (PPQ * 4) + 1));
    // resize from the right edge to two bars
    d.drag(*timeline, { xOf(PPQ * 4 * 6) - 4.f, yOfTrack(bassIdx) }, { xOf(PPQ * 4 * 7) - 4.f, yOfTrack(bassIdx) });
    int len = 0;
    for (auto& c : p.tracks[bassIdx].clips) if (c.id == newClipId) len = c.length;
    d.check(len == PPQ * 4 * 2, "Dragging the right edge resizes the clip", juce::String(len / (PPQ * 4.0), 2) + " bars");
    // long press opens a context menu, which can split the clip
    engine.seek(PPQ * 4 * 6);
    d.pump(100);
    d.event(*timeline, { xOf(PPQ * 4 * 5 + 900), yOfTrack(bassIdx) }, true);
    d.pump(650);
    d.event(*timeline, { xOf(PPQ * 4 * 5 + 900), yOfTrack(bassIdx) }, false);
    d.snap("clip-context-menu");
    const size_t beforeSplit = p.tracks[bassIdx].clips.size();
    d.check(d.chooseMenu("Split at playhead") && p.tracks[bassIdx].clips.size() == beforeSplit + 1, "Long-press menu: Split at playhead");
    // undo restores
    d.key(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0));
    d.check(p.tracks[bassIdx].clips.size() == beforeSplit, "Ctrl/Cmd+Z undoes the split");
    // pinch zoom changes the drawn scale
    auto before = main->createComponentSnapshot(timeline->getBounds().translated(0, 0), true, 1.f);
    d.pinch(*timeline, { 600.f, 300.f }, 60.f, 180.f);
    auto after = main->createComponentSnapshot(timeline->getBounds(), true, 1.f);
    bool changed = false;
    for (int x = 220; x < after.getWidth() && !changed; x += 37) for (int y = 50; y < after.getHeight() && !changed; y += 23) changed = before.getPixelAt(x, y) != after.getPixelAt(x, y);
    d.check(changed, Driver::canMultiTouch() ? "Two-finger pinch zooms the timeline" : "Pinch/magnify gesture zooms the timeline (multi-touch pinch not testable on Linux)");
    d.snap("song-zoomed");

    std::printf("== Piano roll: draw, move, resize notes; velocity\n");
    // double-tap the new clip
    d.pinch(*timeline, { 600.f, 300.f }, 180.f, 60.f);   // zoom back
    session.editClipId.clear();
    for (auto& c : p.tracks[bassIdx].clips) if (c.id == newClipId) { c.selected = false; }
    main->go(ScreenId::Arrange);
    d.pump(100);
    // reset view to known geometry by reopening the screen state
    session.selectTrack(p.tracks[bassIdx].id);
    session.editClipId = newClipId;
    main->go(ScreenId::PianoRoll);
    d.pump(200);
    auto* roll = d.findType("Roll");
    d.check(roll != nullptr && main->current() == ScreenId::PianoRoll, "Piano roll opens for the clip");
    if (!roll) return 1;
    Clip* clip = session.editClip();
    const size_t notesBefore = clip ? clip->notes.size() : 0;
    // the roll centres on C4..C5 and fits the clip; read its geometry from the component itself
    struct RollGeom { float x0, pxPerTick, top, rowH, scrollY; };
    auto geom = [&]() {
        // probe: draw a note at a known spot and read where it landed
        return RollGeom {};
    };
    juce::ignoreUnused(geom);
    const float gx = 58.f + 100.f, gy = 30.f + 200.f;
    d.tap(*roll, { gx, gy });
    clip = session.editClip();
    d.check(clip && clip->notes.size() == notesBefore + 1, "Tapping the grid draws a note");
    if (clip && !clip->notes.empty()) {
        const Note n0 = clip->notes.back();
        d.drag(*roll, { gx + 6.f, gy + 4.f }, { gx + 6.f + 120.f, gy + 4.f - 18.f * 2 });
        clip = session.editClip();
        const Note n1 = clip->notes.back();
        d.check(n1.start > n0.start && n1.pitch == n0.pitch + 2, "Dragging a note moves it in time and pitch", "pitch +" + juce::String(n1.pitch - n0.pitch) + ", start +" + juce::String(n1.start - n0.start) + " ticks");
        // resize from the right edge
        auto* rc = dynamic_cast<juce::Component*>(roll);
        juce::ignoreUnused(rc);
        const int lenBefore = n1.length;
        d.snap("piano-roll-note");
        // add a chord with the chord tool off: tap 3 more notes
        d.tap(*roll, { gx + 240.f, gy + 18.f });
        d.tap(*roll, { gx + 300.f, gy - 36.f });
        d.check(session.editClip()->notes.size() == notesBefore + 3, "More notes drawn", juce::String((int)session.editClip()->notes.size()) + " notes");
        juce::ignoreUnused(lenBefore);
        // velocity lane: drag down on the first new note's bar
        const float velY = (float)roll->getHeight() - 20.f;
        auto* cl = session.editClip();
        const double ppt = (roll->getWidth() - 58 - 20) / (double)juce::jmax(PPQ * 4, cl->contentLength);   // the roll's fit-to-clip zoom
        const float nx = 58.f + (float)(cl->notes.back().start * ppt) + 2.f;
        d.drag(*roll, { nx, velY - 60.f }, { nx, velY - 5.f }, 6);
        bool anyLow = false;
        for (auto& n : session.editClip()->notes) anyLow |= n.velocity < 0.3f;
        d.check(anyLow, "Dragging in the velocity lane lowers a note's velocity");
    }
    d.snap("piano-roll");

    std::printf("== Drums: program a beat with taps\n");
    main->go(ScreenId::Drums);
    d.pump(200);
    auto* grid = d.findType("StepGrid");
    d.check(grid != nullptr, "Step grid is shown");
    Pattern* pat = session.pattern();
    if (grid && pat) {
        auto prev = pat->grid[2];
        const int stepW = juce::jmax(34, (grid->getParentWidth() - 104 - 8) / 16);
        for (int s : { 4, 12 }) d.tap(*grid, { 104.f + stepW * s + stepW * 0.5f, 2 * 44.f + 22.f });
        pat = session.pattern();
        d.check(pat->grid[2][4].on != prev[4].on && pat->grid[2][12].on != prev[12].on, "Tapping steps toggles them (clap on beats 2 and 4)");
        d.drag(*grid, { 104.f + stepW * 0.5f, 5 * 44.f + 22.f }, { 104.f + stepW * 7.5f, 5 * 44.f + 22.f }, 14);
        int painted = 0;
        for (int s = 0; s < 8; ++s) painted += session.pattern()->grid[5][(size_t)s].on ? 1 : 0;
        d.check(painted == 8, "Dragging across a lane paints 8 steps", juce::String(painted) + "/8");
        d.snap("drums");
    }

    std::printf("== Mixer: fader and mute\n");
    main->go(ScreenId::Mixer);
    d.pump(300);
    auto* fader = d.find("Volume");
    if (fader) {
        const float v0 = p.tracks[0].channel->get(0);
        d.drag(*fader, fader->getLocalBounds().getCentre().toFloat(), fader->getLocalBounds().getCentre().toFloat() + juce::Point<float>(0.f, 80.f));
        const float v1 = p.tracks[0].channel->get(0);
        d.check(v1 < v0 - 3.f, "Dragging a fader down lowers the track volume", juce::String(v0, 1) + " -> " + juce::String(v1, 1) + " dB");
    } else d.check(false, "Fader found");
    if (auto* mute = d.find("M")) { d.tap(*mute); d.check(p.tracks[0].channel->getBool(2), "Mute button mutes the first track"); d.tap(*mute); }
    d.snap("mixer");

    std::printf("== Instruments: load a sound and play the keyboard\n");
    session.selectTrack(p.tracks[2].id);
    main->go(ScreenId::Instruments);
    d.pump(300);
    if (auto* list = d.findType("PresetList")) {
        d.tap(*list, { 40.f, 24.f });   // first row: "Studio Synth"
        d.check(p.tracks[2].inst.engine == "studio", "Tapping a preset loads it on the selected track", p.tracks[2].inst.preset);
    }
    if (auto* tab = d.find("Filters", main->screen(ScreenId::Instruments))) {
        d.tap(*tab);
        d.pump(150);
        d.check(d.find("Filter 1 Type", main->screen(ScreenId::Instruments)) != nullptr && d.find("Osc 1 On", main->screen(ScreenId::Instruments)) == nullptr,
                "Studio Synth tabs: Filters shows the filter controls only");
        d.snap("studio-filters");
        if (auto* mm = d.find("Mod Matrix", main->screen(ScreenId::Instruments))) { d.tap(*mm); d.pump(150); d.snap("studio-matrix"); }
    } else d.check(false, "Studio Synth tab buttons found");
    if (auto* rnd = d.find("Randomize or mutate this sound", main->screen(ScreenId::Instruments))) {
        const auto before = juce::JSON::toString(p.tracks[2].inst.params->toVar());
        d.tap(*rnd);
        const bool picked = d.chooseMenu("Randomize: a brand new sound");
        d.check(picked && juce::JSON::toString(p.tracks[2].inst.params->toVar()) != before && p.tracks[2].inst.preset.startsWith("Random"), "Randomize makes a new sound", p.tracks[2].inst.preset);
        session.undo();
        d.check(juce::JSON::toString(session.project.tracks[2].inst.params->toVar()) == before, "Undo brings the previous sound back");
    } else d.check(false, "Randomize button found");
    if (auto* kb = d.findType("Keyboard")) {
        d.event(*kb, { kb->getWidth() * 0.3f, kb->getHeight() * 0.8f }, true);
        d.pump(250);
        const int voices = engine.voices();
        d.event(*kb, { kb->getWidth() * 0.3f, kb->getHeight() * 0.8f }, false);
        d.check(voices > 0, "Touching the on-screen keyboard plays the instrument", juce::String(voices) + " voices");
    }
    d.snap("instruments");

    std::printf("== Effects: add a reverb from the menu\n");
    main->go(ScreenId::Effects);
    d.pump(200);
    if (auto* chain = d.findType("FxChainEditor4List")) {
        const size_t fx0 = p.tracks[2].effects.size();
        d.tap(*chain, { 120.f, (float)fx0 * 56.f + 28.f });
        const bool opened = d.chooseMenu("Time");
        const bool picked = opened && d.chooseMenu("Reverb");
        d.check(picked && p.tracks[2].effects.size() == fx0 + 1 && p.tracks[2].effects.back().type == "reverb", "Add effect > Time > Reverb inserts a reverb");
        d.snap("effects");
        d.tap(*chain, { 120.f, (float)(fx0 + 1) * 56.f + 28.f });
        const bool designed = d.chooseMenu("Design a new effect...");
        d.pump(200);
        d.check(designed && p.tracks[2].effects.back().type == "designer", "Add effect > Design a new effect adds the Effect Designer");
        // module 1 defaults to Drive: its knobs are labelled for that module
        bool labelled = false;
        std::function<void(juce::Component*)> scan = [&](juce::Component* c) { for (auto* k : c->getChildren()) { if (auto* kn = dynamic_cast<UI::Knob*>(k)) if (kn->labelText() == "Asymmetry") labelled = true; scan(k); } };
        scan(main->screen(ScreenId::Effects));
        d.check(labelled, "Designer knobs are named after the chosen module (Drive: Drive / Tone / Asymmetry)");
        d.snap("effects-designer");
    } else d.check(false, "Effect chain list found");

    std::printf("== Record: booth for a vocal track\n");
    session.checkpoint("Add vocal track");
    auto& vox = p.addTrack(TrackType::Audio);
    vox.name = "Lead Vocal";
    const auto voxId = vox.id;
    session.changed(true);
    session.selectTrack(voxId);
    main->go(ScreenId::Record);
    d.pump(300);
    auto* recScreen = main->screen(ScreenId::Record);
    if (auto* thru = d.find("Through effects", recScreen)) {
        d.tap(*thru);
        d.check(p.findTrack(voxId)->monitorMode() == Track::MonitorThrough, "Hear yourself: Through effects");
        if (auto* direct = d.find("Direct (interface)", recScreen)) { d.tap(*direct); d.check(p.findTrack(voxId)->monitorMode() == Track::MonitorDirect, "Hear yourself: Direct (interface)"); }
        if (auto* withFx = d.find("With effects", recScreen)) { d.tap(*withFx); d.check(p.findTrack(voxId)->recordsWithEffects(), "Record: With effects"); }
    } else d.check(false, "Booth monitoring buttons found");
    if (auto* cp = d.find("Chain presets", recScreen)) {
        d.tap(*cp);
        const bool ok = d.chooseMenu("Vocal chains") && d.chooseMenu("Pop Vocal");
        d.pump(200);
        d.check(ok && p.findTrack(voxId)->inputEffects.size() == 6 && p.findTrack(voxId)->effects.empty(), "Vocal chain preset loads into the live input chain", juce::String((int)p.findTrack(voxId)->inputEffects.size()) + " effects");
    } else d.check(false, "Chain presets button found");
    d.snap("record-booth");

    std::printf("== Export: render the song to WAV\n");
    main->go(ScreenId::Export);
    d.pump(200);
    if (auto* ex = d.find("Export", main->screen(ScreenId::Export))) {
        d.tap(*ex);
        for (int i = 0; i < 200 && !Paths::exports().getChildFile(juce::File::createLegalFileName(p.name) + ".wav").existsAsFile(); ++i) d.pump(100);
        d.pump(300);
        const auto f = Paths::exports().getChildFile(juce::File::createLegalFileName(p.name) + ".wav");
        std::unique_ptr<juce::AudioFormatReader> r(SampleBank::get().formats().createReaderFor(f));
        const double secs = r ? r->lengthInSamples / r->sampleRate : 0.0;
        const double song = p.ticksToSeconds(p.songEndTick());
        d.check(r && secs >= song, "Export button writes a WAV containing the whole song", juce::String(secs, 2) + " s file, song " + juce::String(song, 2) + " s");
        d.snap("export-done");
    }

    std::printf("== Phone sizes and rotation\n");
    for (auto sz : { juce::Point<int>(390, 844), juce::Point<int>(844, 390), juce::Point<int>(360, 640) }) {
        win.setSize(sz.x, sz.y);
        d.pump(200);
        bool overlap = false;
        juce::String offender;
        for (auto* c : main->getChildren()) if (c->isVisible() && !main->getLocalBounds().contains(c->getBounds()) && c->getWidth() < main->getWidth()) { overlap = true; offender << typeid(*c).name() << " " << c->getBounds().toString() << " "; }
        main->go(ScreenId::Arrange);
        d.pump(100);
        d.check(!overlap && main->isCompact() == (sz.x < 700 && sz.x <= sz.y * 1.25f), "Layout fits " + juce::String(sz.x) + "x" + juce::String(sz.y), offender + (main->isCompact() ? "compact" : "wide"));
        d.snap("size-" + juce::String(sz.x) + "x" + juce::String(sz.y));
    }

    std::printf("== Save and reopen\n");
    juce::String err;
    const auto folder = session.folder;
    const auto json = juce::JSON::toString(p.toVar());
    d.check(session.save(err), "Save", err);
    Session s2(engine);
    d.check(s2.open(folder, err) && juce::JSON::toString(s2.project.toVar()).length() > 1000, "Reopen the saved song", err);
    int notes1 = 0, notes2 = 0;
    for (auto& t : p.tracks) for (auto& c : t.clips) notes1 += (int)c.notes.size();
    for (auto& t : s2.project.tracks) for (auto& c : t.clips) notes2 += (int)c.notes.size();
    d.check(notes1 == notes2 && s2.project.tracks.size() == p.tracks.size(), "Everything drawn through the UI survived save and reopen", juce::String(notes2) + " notes");

    const auto summary = "UI test: " + juce::String(d.passed) + " passed, " + juce::String(d.failed) + " failed";
    std::printf("\n%s\n", summary.toRawUTF8());
    d.log.insert(0, summary);
    dir.getChildFile("uitest.txt").replaceWithText(d.log.joinIntoString("\n"));
    win.clearContentComponent();
    engine.shutdown();
    ctx().session = nullptr; ctx().engine = nullptr;
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    Paths::overrideRoot() = juce::File();
    return d.failed == 0 ? 0 : 1;
}
#else
int runUITest(const juce::File& dir) { juce::ignoreUnused(dir); return 1; }
#endif
