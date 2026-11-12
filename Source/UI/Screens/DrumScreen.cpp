#include "Screens.h"
#include "Core/Paths.h"
#include "Engine/Catalog.h"

using namespace UI;

namespace {

class StepGrid : public juce::Component {
public:
    explicit StepGrid(Session& s) : session(s) { lp.fire = [this](juce::Point<float> p) { longPress(p); }; setTitle("Step grid"); }
    int mode = 0;   // 0 steps, 1 velocity, 2 probability
    static constexpr int laneH = 44, nameW = 104;
    Track* drums() { auto* t = session.track(); return t && t->type == TrackType::Drum ? t : nullptr; }
    Pattern* pat() { return session.pattern(); }
    int stepW() const { auto* p = const_cast<StepGrid*>(this)->pat(); const int steps = p ? p->steps : 16; return juce::jmax(34, (getParentWidth() - nameW - 8) / juce::jmin(steps, 16)); }
    int preferredWidth() { auto* p = pat(); return nameW + (p ? p->steps : 16) * stepW() + 8; }
    juce::Rectangle<float> cell(int pad, int step) const { return { (float)(nameW + step * stepW()), (float)(pad * laneH), (float)stepW(), (float)laneH }; }

    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        g.fillAll(p.bg);
        auto* t = drums();
        auto* pt = pat();
        if (!t || !pt) return;
        const int playStep = currentStep(*pt);
        for (int pad = 0; pad < kDrumPads; ++pad) {
            auto& pd = t->inst.pads[(size_t)pad];
            const bool muted = pd.params && pd.params->getBool(pd.params->indexOf("mute"));
            auto name = juce::Rectangle<float>(0.f, (float)(pad * laneH), (float)nameW, (float)laneH).reduced(3.f, 2.f);
            g.setColour(pad == flashPad ? p.accent : p.surface);
            g.fillRoundedRectangle(name, rClip);
            g.setColour(pad == flashPad ? p.accentText : (muted ? p.faint : p.text));
            g.setFont(font(12.5f, true));
            g.drawFittedText(pd.name, name.reduced(8.f, 2.f).toNearestInt(), juce::Justification::centredLeft, 2);
            for (int s = 0; s < pt->steps; ++s) {
                auto c = cell(pad, s).reduced(2.f);
                const auto& st = pt->grid[(size_t)pad][(size_t)s];
                const bool beat = (s / 4) % 2 == 0;
                g.setColour(beat ? p.raised : p.surface);
                g.fillRoundedRectangle(c, 5.f);
                if (st.on) {
                    const float v = mode == 2 ? st.probability : st.velocity;
                    auto fill = c.withTop(c.getBottom() - c.getHeight() * (0.35f + 0.65f * v));
                    g.setColour(t->colour.withAlpha(muted ? 0.35f : (0.45f + 0.55f * st.probability)));
                    g.fillRoundedRectangle(fill, 5.f);
                    if (st.probability < 0.999f) { g.setColour(p.text.withAlpha(0.8f)); g.setFont(font(9.5f, true)); g.drawText(juce::String(juce::roundToInt(st.probability * 100)) + "%", c.toNearestInt(), juce::Justification::centredTop); }
                    if (st.ratchet > 1) { g.setColour(p.bg.withAlpha(0.6f)); for (int k = 1; k < st.ratchet; ++k) g.fillRect(c.getX() + c.getWidth() * k / st.ratchet - 1.f, c.getY() + 3.f, 2.f, c.getHeight() - 6.f); }
                    if (st.pitch != 0) { g.setColour(p.text); g.setFont(font(9.5f, true)); g.drawText((st.pitch > 0 ? "+" : "") + juce::String(st.pitch), c.toNearestInt(), juce::Justification::centredBottom); }
                }
                if (s == playStep) { g.setColour(p.accent); g.drawRoundedRectangle(c, 5.f, 2.f); }
            }
        }
    }

    int currentStep(const Pattern& pt) {
        auto& pr = session.project;
        if (!ctx().engine->isPlaying()) return -1;
        const double tick = ctx().engine->playheadTick();
        if (pr.previewTrack.isNotEmpty() && pr.previewPattern == pt.id) return (int)(std::fmod(tick, (double)pt.lengthTicks()) / pt.stepTicks);
        auto* t = drums();
        if (!t) return -1;
        for (auto& c : t->clips) if (c.type == ClipType::Pattern && c.patternId == pt.id && tick >= c.start && tick < c.end()) return (int)(std::fmod(tick - c.start, (double)pt.lengthTicks()) / pt.stepTicks);
        return -1;
    }

    bool hitCell(juce::Point<float> pos, int& pad, int& step) {
        auto* pt = pat();
        if (!pt || pos.x < nameW) return false;
        pad = (int)(pos.y / laneH);
        step = (int)((pos.x - nameW) / stepW());
        return pad >= 0 && pad < kDrumPads && step >= 0 && step < pt->steps;
    }

    void mouseDown(const juce::MouseEvent& e) override {
        auto* pt = pat();
        auto* t = drums();
        if (!pt || !t) return;
        lp.down(e.position);
        paintValue = -1;
        dragged = false;
        if (e.position.x < nameW) {
            const int pad = (int)(e.position.y / laneH);
            if (pad >= 0 && pad < kDrumPads) { ctx().engine->liveNoteOn(t->id, 36 + pad, 0.9f); flashPad = pad; repaint(); }
            downPad = pad;
            return;
        }
        int pad, step;
        if (!hitCell(e.position, pad, step)) return;
        downCell = { pad, step };
        if (mode == 0) {
            // toggling happens on release (tap) or when a paint-drag starts, so a long-press can open the step editor
            paintValue = pat()->grid[(size_t)pad][(size_t)step].on ? 0 : 1;
        } else { session.checkpoint(mode == 1 ? "Step velocity" : "Step probability"); edited = true; setLevel(pad, step, e.position); }
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        lp.moved(e.position);
        if (!dragged && e.getDistanceFromDragStart() > 6) {
            dragged = true;
            if (mode == 0 && paintValue >= 0 && downCell.first >= 0) { session.checkpoint("Paint steps"); edited = true; setStep(downCell.first, downCell.second); }
        }
        if (!dragged) return;
        int pad, step;
        if (!hitCell(e.position, pad, step)) return;
        if (mode == 0 && paintValue >= 0) setStep(pad, step);
        else if (mode != 0) setLevel(pad, step, e.position);
    }
    void mouseUp(const juce::MouseEvent&) override {
        lp.up();
        if (downPad >= 0) { flashPad = -1; downPad = -1; repaint(); }
        if (mode == 0 && !dragged && !lp.didFire() && paintValue >= 0 && downCell.first >= 0) {
            session.checkpoint("Edit steps");
            edited = true;
            setStep(downCell.first, downCell.second);
            if (paintValue == 1) if (auto* t = drums()) ctx().engine->liveNoteOn(t->id, 36 + downCell.first, pat()->grid[(size_t)downCell.first][(size_t)downCell.second].velocity);
            haptic(0);
        }
        if (edited) session.changed();
        edited = false;
        paintValue = -1;
        downCell = { -1, -1 };
    }
    void setStep(int pad, int step) {
        auto& st = pat()->grid[(size_t)pad][(size_t)step];
        const bool on = paintValue == 1;
        if (st.on == on) return;
        st.on = on;
        if (on && st.velocity <= 0.f) st.velocity = 0.8f;
        session.engine.markDirty();
        repaint();
    }
    void setLevel(int pad, int step, juce::Point<float> pos) {
        auto& st = pat()->grid[(size_t)pad][(size_t)step];
        const auto c = cell(pad, step);
        const float v = juce::jlimit(0.05f, 1.f, (c.getBottom() - pos.y) / c.getHeight() * 1.2f);
        st.on = true;
        if (mode == 1) st.velocity = v; else st.probability = v;
        session.engine.markDirty();
        repaint();
    }

    void longPress(juce::Point<float> pos) {
        if (dragged) return;
        haptic(1);
        if (pos.x < nameW) { const int pad = (int)(pos.y / laneH); if (pad >= 0 && pad < kDrumPads && onPadMenu) onPadMenu(pad, this); return; }
        int pad, step;
        if (!hitCell(pos, pad, step)) return;
        paintValue = -1;
        auto& st = pat()->grid[(size_t)pad][(size_t)step];
        juce::PopupMenu m, vel, prob, pitch, rat;
        for (int v : { 25, 50, 75, 90, 100 }) vel.addItem(100 + v, juce::String(v) + "%", true, juce::roundToInt(st.velocity * 100) == v);
        for (int v : { 10, 25, 50, 75, 100 }) prob.addItem(300 + v, juce::String(v) + "%", true, juce::roundToInt(st.probability * 100) == v);
        for (int v = -12; v <= 12; ++v) pitch.addItem(500 + v + 12, (v > 0 ? "+" : "") + juce::String(v) + " st", true, st.pitch == v);
        for (int v = 1; v <= 4; ++v) rat.addItem(600 + v, v == 1 ? juce::String("Off") : "x" + juce::String(v), true, st.ratchet == v);
        m.addSectionHeader("Step " + juce::String(step + 1));
        m.addItem(1, st.on ? "Turn off" : "Turn on");
        m.addSubMenu("Velocity", vel);
        m.addSubMenu("Probability", prob);
        m.addSubMenu("Pitch", pitch);
        m.addSubMenu("Ratchet (repeats)", rat);
        showMenu(m, this, [this, pad, step](int r) {
            auto* pt = pat();
            if (!pt || r == 0) return;
            session.checkpoint("Edit step");
            auto& s2 = pt->grid[(size_t)pad][(size_t)step];
            if (r == 1) s2.on = !s2.on;
            else if (r >= 100 && r < 200) { s2.on = true; s2.velocity = (r - 100) / 100.f; }
            else if (r >= 300 && r < 500) { s2.on = true; s2.probability = (r - 300) / 100.f; }
            else if (r >= 500 && r < 600) { s2.on = true; s2.pitch = r - 512; }
            else if (r >= 600) { s2.on = true; s2.ratchet = r - 600; }
            session.changed();
        });
    }

    std::function<void(int, juce::Component*)> onPadMenu;
    int flashPad = -1;
private:
    Session& session;
    LongPress lp;
    int paintValue = -1, downPad = -1;
    std::pair<int, int> downCell { -1, -1 };
    bool dragged = false, edited = false;
};

class PadGrid : public juce::Component {
public:
    explicit PadGrid(Session& s) : session(s) { setTitle("Drum pads"); }
    bool recordToPattern = false;
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        auto* t = session.track();
        for (int i = 0; i < 16; ++i) {
            auto r = padRect(i).reduced(4.f);
            const bool on = down.count(i) > 0;
            g.setColour(on ? (t ? t->colour : p.accent) : p.raised);
            g.fillRoundedRectangle(r, rControl);
            if (t && t->type == TrackType::Drum) {
                g.setColour(on ? juce::Colour(0xff17181b) : p.text);
                g.setFont(font(13.f, true));
                g.drawFittedText(t->inst.pads[(size_t)i].name, r.reduced(8.f).toNearestInt(), juce::Justification::bottomLeft, 2);
            }
        }
    }
    juce::Rectangle<float> padRect(int i) const {
        const float w = getWidth() / 4.f, h = getHeight() / 4.f;
        return { (i % 4) * w, (3 - i / 4) * h, w, h };   // pad 1 bottom-left like hardware
    }
    int padAt(juce::Point<float> p) const { for (int i = 0; i < 16; ++i) if (padRect(i).contains(p)) return i; return -1; }
    void mouseDown(const juce::MouseEvent& e) override {
        const int pad = padAt(e.position);
        auto* t = session.track();
        if (pad < 0 || !t || t->type != TrackType::Drum) return;
        // velocity from vertical position inside the pad
        const auto r = padRect(pad);
        const float vel = juce::jlimit(0.3f, 1.f, 0.4f + (r.getBottom() - e.position.y) / r.getHeight() * 0.7f);
        ctx().engine->liveNoteOn(t->id, 36 + pad, vel);
        down.insert(pad);
        touch[e.source.getIndex()] = pad;
        haptic(0);
        if (recordToPattern && ctx().engine->isPlaying()) if (auto* pt = session.pattern()) {
            double tick = ctx().engine->playheadTick();
            auto& pr = session.project;
            if (!(pr.previewTrack.isNotEmpty() && pr.previewPattern == pt->id))
                for (auto& c : t->clips) if (c.type == ClipType::Pattern && c.patternId == pt->id && tick >= c.start && tick < c.end()) tick -= c.start;
            const int step = (int)std::lround(std::fmod(tick, (double)pt->lengthTicks()) / pt->stepTicks) % pt->steps;
            session.checkpoint("Record pad");
            auto& st = pt->grid[(size_t)pad][(size_t)step];
            st.on = true; st.velocity = vel;
            session.changed();
        }
        repaint();
    }
    void mouseUp(const juce::MouseEvent& e) override {
        auto it = touch.find(e.source.getIndex());
        if (it != touch.end()) { down.erase(it->second); touch.erase(it); }
        repaint();
    }
private:
    Session& session;
    std::set<int> down;
    std::map<int, int> touch;
};

class DrumScreen : public Screen {
public:
    DrumScreen() : grid(session()), pads(session()) {
        addAndMakeVisible(patterns);
        addAndMakeVisible(scroller);
        scroller.setScrollBarsShown(true, true);
        scroller.setViewedComponent(&grid, false);
        addChildComponent(pads);
        for (auto* b : std::initializer_list<juce::Component*> { &playB, &addB, &chainB, &patMenuB, &stepsB, &swingB, &kitB, &padsB }) addAndMakeVisible(b);
        for (auto* c : { &modeSteps, &modeVel, &modeProb, &recPads }) addAndMakeVisible(c);
        modeSteps.setRadioGroupId(7); modeVel.setRadioGroupId(7); modeProb.setRadioGroupId(7);
        modeSteps.setToggleState(true, juce::dontSendNotification);
        modeSteps.onClick = [this] { grid.mode = 0; grid.repaint(); };
        modeVel.onClick = [this] { grid.mode = 1; grid.repaint(); };
        modeProb.onClick = [this] { grid.mode = 2; grid.repaint(); };
        recPads.onClick = [this] { pads.recordToPattern = recPads.getToggleState(); if (pads.recordToPattern) ctx().toast("Pad hits are written into the pattern while it plays"); };
        playB.onClick = [this] { togglePreview(); };
        addB.onClick = [this] { addToSong(); };
        chainB.onClick = [this] { chainMenu(); };
        patMenuB.onClick = [this] { patternMenu(); };
        stepsB.onClick = [this] { stepsMenu(); };
        swingB.onClick = [this] { swingMenu(); };
        kitB.onClick = [this] { kitMenu(); };
        padsB.setClickingTogglesState(true);
        padsB.onClick = [this] { pads.setVisible(padsB.getToggleState()); resized(); };
        grid.onPadMenu = [this](int pad, juce::Component* c) { padMenu(pad, c); };
        startTimerHz(20);
    }
    juce::String title() const override { return "Drums"; }
    void onShow() override {
        // focus a drum track
        auto* t = session().track();
        if (!t || t->type != TrackType::Drum)
            for (auto& tr : proj().tracks) if (tr.type == TrackType::Drum) { session().selectedTrack = tr.id; session().engine.setLiveTarget(tr.id); break; }
        if (!session().pattern() && !proj().patterns.empty()) session().currentPattern = proj().patterns.front().id;
        refresh();
    }
    void onHide() override { if (proj().previewTrack.isNotEmpty()) togglePreview(); }
    void refresh() override {
        patterns.clear();
        auto* t = session().track();
        const bool ok = t && t->type == TrackType::Drum;
        for (auto& p : proj().patterns) patterns.addPattern(p.id, p.name, p.colour, p.id == session().currentPattern);
        patterns.onSelect = [this](const juce::String& id) { session().currentPattern = id; if (proj().previewTrack.isNotEmpty()) { proj().previewPattern = id; engine().rebuild(); } refresh(); };
        patterns.onAdd = [this] { newPattern(); };
        patterns.resized();
        auto* pt = session().pattern();
        stepsB.setButtonText(pt ? juce::String(pt->steps) + " steps, " + resolutionName(pt->stepTicks) : juce::String("Steps"));
        swingB.setButtonText(pt ? "Swing " + juce::String(juce::roundToInt(pt->swing * 100)) + "%" : juce::String("Swing"));
        kitB.setButtonText(ok ? t->inst.preset : juce::String("Kit"));
        playB.setToggleState(proj().previewTrack.isNotEmpty(), juce::dontSendNotification);
        for (auto* c : std::initializer_list<juce::Component*> { &scroller, &addB, &chainB, &patMenuB, &stepsB, &swingB, &kitB, &playB, &modeSteps, &modeVel, &modeProb, &padsB })
            c->setEnabled(ok && pt != nullptr);
        resized();
        grid.repaint();
        pads.repaint();
        repaint();
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(pal().surface);
        auto* t = session().track();
        if (!t || t->type != TrackType::Drum || !session().pattern()) {
            g.setColour(pal().dim);
            g.setFont(font(16.f));
            g.drawFittedText(!t || t->type != TrackType::Drum ? "Add a drum track to build beats. Tap + below." : "Tap + Pattern to create your first beat.",
                             getLocalBounds().reduced(30).withTrimmedTop(160), juce::Justification::centredTop, 3);
        }
    }
    void resized() override {
        auto r = getLocalBounds().reduced(6, 4);
        const bool narrow = r.getWidth() < 600;
        auto row1 = r.removeFromTop(48);
        playB.setBounds(row1.removeFromLeft(touch));
        patMenuB.setBounds(row1.removeFromRight(touch));
        chainB.setBounds(row1.removeFromRight(touch));
        addB.setBounds(row1.removeFromRight(narrow ? touch : 160));
        addB.setCaption(narrow ? juce::String() : "Add to song");
        patterns.setBounds(row1.reduced(4, 0));
        r.removeFromTop(4);
        auto row2 = r.removeFromTop(44);
        if (narrow) {
            auto row3 = r.removeFromTop(48).withTrimmedTop(4);
            const int w3 = row3.getWidth() / 4;
            modeSteps.setBounds(row3.removeFromLeft(w3)); modeVel.setBounds(row3.removeFromLeft(w3)); modeProb.setBounds(row3.removeFromLeft(w3));
            padsB.setBounds(row3);
        }
        const int w = row2.getWidth() / (narrow ? 3 : 7);
        kitB.setBounds(row2.removeFromLeft(w).reduced(2, 0));
        stepsB.setBounds(row2.removeFromLeft(w).reduced(2, 0));
        swingB.setBounds(row2.removeFromLeft(w).reduced(2, 0));
        if (!narrow) {
            modeSteps.setBounds(row2.removeFromLeft(w)); modeVel.setBounds(row2.removeFromLeft(w)); modeProb.setBounds(row2.removeFromLeft(w));
            padsB.setBounds(row2);
        }
        r.removeFromTop(6);
        if (pads.isVisible()) {
            auto pr = r.removeFromBottom(juce::jmin(r.getHeight() / 2, r.getWidth()));
            recPads.setVisible(true);
            recPads.setBounds(pr.removeFromTop(40).removeFromRight(200).reduced(0, 4));
            pads.setBounds(pr);
        } else recPads.setVisible(false);
        scroller.setBounds(r);
        grid.setSize(juce::jmax(r.getWidth() - 8, grid.preferredWidth()), StepGrid::laneH * kDrumPads + 8);
    }
private:
    static juce::String resolutionName(int t) { return t == PPQ / 2 ? "1/8" : t == PPQ / 4 ? "1/16" : t == PPQ / 8 ? "1/32" : t == PPQ / 6 ? "1/16 T" : t == PPQ / 3 ? "1/8 T" : "custom"; }
    void timerCallback() { if (engine().isPlaying()) grid.repaint(); }
    struct Ticker : juce::Timer { std::function<void()> f; void timerCallback() override { f(); } } ticker;
    void startTimerHz(int hz) { ticker.f = [this] { timerCallback(); }; ticker.startTimerHz(hz); }

    void newPattern() {
        auto* t = session().track();
        if (!t || t->type != TrackType::Drum) { ScreenUtil::addTrack(session(), TrackType::Drum); }
        session().checkpoint("New pattern");
        auto& p = proj().addPattern(juce::String::charToString((juce::juce_wchar)('A' + (int)proj().patterns.size() % 26)));
        session().currentPattern = p.id;
        session().changed(true);
    }
    void togglePreview() {
        auto& p = proj();
        if (p.previewTrack.isNotEmpty()) { p.previewTrack.clear(); p.previewPattern.clear(); engine().stop(); engine().rebuild(); engine().seek(0); refresh(); return; }
        auto* t = session().track();
        auto* pt = session().pattern();
        if (!t || !pt) return;
        p.previewTrack = t->id; p.previewPattern = pt->id; p.previewClip.clear();
        engine().rebuild();
        engine().seek(0);
        engine().play();
        refresh();
    }
    void addToSong() {
        auto* t = session().track();
        auto* pt = session().pattern();
        if (!t || !pt) return;
        session().checkpoint("Add pattern to song");
        const int start = proj().barToTick(proj().tickToBar((int)engine().playheadTick()));
        // place after any clip already covering the playhead
        int at = start;
        for (auto& c : t->clips) if (at >= c.start && at < c.end()) at = c.end();
        Clip c; c.id = proj().newId("c"); c.type = ClipType::Pattern; c.patternId = pt->id; c.name = pt->name;
        c.start = at; c.length = juce::jmax(pt->lengthTicks(), proj().ticksPerBarAt(at));
        t->clips.push_back(c);
        session().changed(true);
        engine().seek(c.end());
        ctx().toast("Pattern " + pt->name + " added at bar " + juce::String(proj().tickToBar(at) + 1));
    }
    void chainMenu() {
        juce::PopupMenu m;
        m.addSectionHeader("Chain patterns into the song");
        juce::String all;
        for (auto& p : proj().patterns) all << p.name;
        m.addItem(1, "Append all patterns in order (" + all + ")");
        m.addItem(2, "Append current pattern x4");
        m.addItem(3, "Type a chain (e.g. AABA)...");
        showMenu(m, &chainB, [this](int r) {
            if (r == 1) { juce::String s; for (auto& p : proj().patterns) s << p.name.substring(0, 1); appendChain(s); }
            else if (r == 2) { if (auto* pt = session().pattern()) appendChain(juce::String::repeatedString(pt->name.substring(0, 1), 4)); }
            else if (r == 3) askText("Pattern chain", "Pattern letters in the order they should play", "AABA", "Append", [this](const juce::String& s) { appendChain(s); });
        });
    }
    void appendChain(const juce::String& chain) {
        auto* t = session().track();
        if (!t) return;
        session().checkpoint("Chain patterns");
        int at = 0;
        for (auto& c : t->clips) at = juce::jmax(at, c.end());
        int added = 0;
        for (auto ch : chain) {
            const Pattern* found = nullptr;
            for (auto& p : proj().patterns) if (p.name.substring(0, 1).equalsIgnoreCase(juce::String::charToString(ch))) { found = &p; break; }
            if (!found) continue;
            Clip c; c.id = proj().newId("c"); c.type = ClipType::Pattern; c.patternId = found->id; c.name = found->name; c.start = at; c.length = juce::jmax(found->lengthTicks(), PPQ);
            t->clips.push_back(c);
            at += c.length;
            ++added;
        }
        session().changed(true);
        ctx().toast(added ? "Added " + juce::String(added) + " patterns to the song" : "No matching pattern letters");
    }
    void patternMenu() {
        auto* pt = session().pattern();
        if (!pt) return;
        juce::PopupMenu m;
        m.addSectionHeader("Pattern " + pt->name);
        m.addItem(1, "New pattern");
        m.addItem(2, "Duplicate");
        m.addItem(3, "Create variation (fills and ghost notes)");
        m.addItem(4, "Rename...");
        m.addItem(5, "Clear all steps");
        m.addItem(6, "Shift right one step");
        m.addItem(7, "Shift left one step");
        m.addSeparator();
        m.addItem(8, "Delete pattern...", proj().patterns.size() > 1);
        showMenu(m, &patMenuB, [this](int r) {
            auto* p = session().pattern();
            if (!p || r == 0) return;
            if (r == 1) { newPattern(); return; }
            if (r == 4) { askText("Rename pattern", {}, p->name, "Rename", [this](const juce::String& n) { if (auto* pp = session().pattern()) { session().checkpoint("Rename pattern"); pp->name = n; for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.patternId == pp->id) c.name = n; session().changed(true); } }); return; }
            if (r == 8) { confirm("Delete pattern " + p->name + "?", "Clips using it are removed from the song too.", "Delete", [this] {
                auto* pp = session().pattern(); if (!pp) return;
                session().checkpoint("Delete pattern");
                const auto id = pp->id;
                for (auto& t : proj().tracks) t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&id](auto& c) { return c.patternId == id; }), t.clips.end());
                proj().patterns.erase(std::remove_if(proj().patterns.begin(), proj().patterns.end(), [&id](auto& x) { return x.id == id; }), proj().patterns.end());
                session().currentPattern = proj().patterns.empty() ? juce::String() : proj().patterns.front().id;
                session().changed(true);
            }, true); return; }
            session().checkpoint("Edit pattern");
            if (r == 2 || r == 3) {
                Pattern copy = *p;
                copy.id = proj().newId("p");
                copy.name = juce::String::charToString((juce::juce_wchar)('A' + (int)proj().patterns.size() % 26));
                copy.colour = Project::trackColour((int)proj().patterns.size() + 3);
                if (r == 3) {
                    juce::Random rnd;
                    // ghost notes on snare/hats, an extra kick, and a fill in the last beat
                    for (int s = 0; s < copy.steps; ++s) {
                        if (!copy.grid[1][(size_t)s].on && rnd.nextFloat() < 0.15f) copy.grid[1][(size_t)s] = { true, 0.3f, 0.7f, 0, 1 };
                        if (!copy.grid[3][(size_t)s].on && rnd.nextFloat() < 0.25f) copy.grid[3][(size_t)s] = { true, 0.45f, 0.8f, 0, 1 };
                        if (copy.grid[3][(size_t)s].on && rnd.nextFloat() < 0.15f) copy.grid[3][(size_t)s].ratchet = 2;
                    }
                    const int fill = juce::jmax(0, copy.steps - 4);
                    for (int s = fill; s < copy.steps; ++s) if (rnd.nextFloat() < 0.6f) copy.grid[(size_t)(5 + rnd.nextInt(3))][(size_t)s] = { true, 0.6f + 0.1f * (s - fill), 1.f, 0, 1 };
                }
                proj().patterns.push_back(copy);
                session().currentPattern = copy.id;
            } else if (r == 5) p->grid = {};
            else if (r == 6 || r == 7) {
                for (auto& lane : p->grid) {
                    std::array<DrumStep, kMaxSteps> rot {};
                    for (int s = 0; s < p->steps; ++s) rot[(size_t)((s + (r == 6 ? 1 : p->steps - 1)) % p->steps)] = lane[(size_t)s];
                    for (int s = 0; s < p->steps; ++s) lane[(size_t)s] = rot[(size_t)s];
                }
            }
            session().changed(true);
        });
    }
    void stepsMenu() {
        auto* pt = session().pattern();
        if (!pt) return;
        juce::PopupMenu m, res;
        for (int s : { 8, 12, 16, 24, 32, 48, 64 }) m.addItem(s, juce::String(s) + " steps", true, pt->steps == s);
        const std::pair<int, const char*> r[] = { { PPQ / 2, "1/8" }, { PPQ / 4, "1/16" }, { PPQ / 8, "1/32" }, { PPQ / 3, "1/8 triplets" }, { PPQ / 6, "1/16 triplets" } };
        for (auto& x : r) res.addItem(1000 + x.first, x.second, true, pt->stepTicks == x.first);
        m.addSeparator();
        m.addSubMenu("Step length", res);
        showMenu(m, &stepsB, [this](int v) {
            auto* p = session().pattern();
            if (!p || v == 0) return;
            session().checkpoint("Pattern length");
            if (v >= 1000) p->stepTicks = v - 1000;
            else {
                // growing a pattern repeats its existing content (expandable patterns)
                const int old = p->steps;
                p->steps = v;
                if (v > old) for (auto& lane : p->grid) for (int s = old; s < v; ++s) lane[(size_t)s] = lane[(size_t)(s % old)];
            }
            for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.patternId == p->id) c.length = juce::jmax(c.length, p->lengthTicks());
            session().changed(true);
        });
    }
    void swingMenu() {
        juce::PopupMenu m;
        auto* pt = session().pattern();
        if (!pt) return;
        for (int s : { 0, 10, 20, 30, 40, 50, 60, 75 }) m.addItem(s + 1, juce::String(s) + "%", true, juce::roundToInt(pt->swing * 100) == s);
        showMenu(m, &swingB, [this](int r) { if (r > 0) if (auto* p = session().pattern()) { session().checkpoint("Swing"); p->swing = (r - 1) / 100.f; session().changed(); } });
    }
    void kitMenu() {
        auto* t = session().track();
        if (!t || t->type != TrackType::Drum) return;
        juce::PopupMenu m, saved;
        int k = 1;
        for (auto& [name, sounds] : Catalog::drumKits()) m.addItem(k++, name, true, t->inst.preset == name);
        juce::Array<juce::File> kits;
        const auto dir = Paths::presets().getChildFile("Kits");
        for (auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.json")) { kits.add(f); saved.addItem(100 + kits.size() - 1, f.getFileNameWithoutExtension()); }
        m.addSubMenu("My kits", saved, kits.size() > 0);
        m.addSeparator();
        m.addItem(50, "Save this kit...");
        m.addItem(51, "Kit level and settings...");
        showMenu(m, &kitB, [this, kits](int r) {
            auto* tr = session().track();
            if (!tr || r == 0) return;
            if (r == 50) {
                askText("Save kit", "Saves every pad's sound and settings.", tr->inst.preset, "Save", [this](const juce::String& n) {
                    auto* t2 = session().track(); if (!t2) return;
                    juce::Array<juce::var> padsV;
                    for (auto& p : t2->inst.pads) { auto* o = new juce::DynamicObject(); o->setProperty("name", p.name); o->setProperty("sample", p.sample); o->setProperty("params", p.params->toVar()); padsV.add(juce::var(o)); }
                    const auto dir2 = Paths::presets().getChildFile("Kits"); dir2.createDirectory();
                    dir2.getChildFile(juce::File::createLegalFileName(n) + ".json").replaceWithText(juce::JSON::toString(juce::var(padsV)));
                    ctx().toast("Kit saved");
                });
                return;
            }
            if (r == 51) { session().selectTrack(tr->id); ctx().go(ScreenId::Instruments); return; }
            session().checkpoint("Change kit");
            if (r >= 100) {
                auto v = juce::JSON::parse(kits[r - 100]);
                if (auto* arr = v.getArray())
                    for (int i = 0; i < juce::jmin(16, arr->size()); ++i) {
                        auto& p = tr->inst.pads[(size_t)i];
                        p.name = (*arr)[i]["name"].toString(); p.sample = (*arr)[i]["sample"].toString(); p.params->fromVar((*arr)[i]["params"]);
                    }
                tr->inst.preset = kits[r - 100].getFileNameWithoutExtension();
            } else {
                const auto& kit = Catalog::drumKits()[(size_t)r - 1];
                for (int i = 0; i < 16; ++i) {
                    auto& p = tr->inst.pads[(size_t)i];
                    p.sample = "builtin:" + kit.second[i];
                    for (auto& d : Catalog::drumSounds()) if (kit.second[i] == d.id) p.name = d.name;
                }
                tr->inst.preset = kit.first;
            }
            session().changed(true);
        });
    }
    void padMenu(int pad, juce::Component* target) {
        auto* t = session().track();
        if (!t) return;
        auto& pd = t->inst.pads[(size_t)pad];
        juce::PopupMenu m, sounds;
        std::map<juce::String, juce::PopupMenu> cats;
        int i = 0;
        for (auto& d : Catalog::drumSounds()) cats[d.category].addItem(1000 + i++, d.name, true, pd.sample == "builtin:" + juce::String(d.id));
        for (auto& [name, menu] : cats) sounds.addSubMenu(name, menu);
        m.addSectionHeader("Pad " + juce::String(pad + 1) + ": " + pd.name);
        m.addSubMenu("Built-in sound", sounds);
        m.addItem(1, "Use a sample from the browser...");
        m.addItem(2, "Import an audio file...");
        m.addItem(5, "Shape this sound (tune, length, filter, pitch drop...)");
        m.addItem(3, pd.params->getBool(pd.params->indexOf("mute")) ? "Unmute pad" : "Mute pad");
        m.addItem(4, "Clear this pad's steps");
        showMenu(m, target, [this, pad](int r) {
            auto* tr = session().track();
            if (!tr || r == 0) return;
            auto& p = tr->inst.pads[(size_t)pad];
            if (r >= 1000 && r < 2000) {
                const auto& d = Catalog::drumSounds()[(size_t)r - 1000];
                session().checkpoint("Pad sound"); p.sample = "builtin:" + juce::String(d.id); p.name = d.name; session().changed(true);
                engine().liveNoteOn(tr->id, 36 + pad, 0.9f);
            } else if (r == 5) {
                const auto tid = tr->id;
                showParamSheet("Pad " + juce::String(pad + 1) + ": " + p.name, p.params, &grid, tid, [tid, pad] {
                    ctx().engine->liveNoteOn(tid, 36 + pad, 0.9f);
                    juce::Timer::callAfterDelay(300, [tid, pad] { ctx().engine->liveNoteOff(tid, 36 + pad); });
                });
            } else if (r == 1) { padTarget = pad; ctx().toast("Choose a sample, then tap \"Use on drum pad\""); browsePadSample(pad); }
            else if (r == 2) importPadSample(pad);
            else if (r == 3) { session().checkpoint("Mute pad"); const int mi = p.params->indexOf("mute"); p.params->set(mi, p.params->getBool(mi) ? 0.f : 1.f); session().changed(); }
            else if (r == 4) if (auto* pt = session().pattern()) { session().checkpoint("Clear pad steps"); for (auto& s : pt->grid[(size_t)pad]) s = {}; session().changed(); }
        });
    }
    void browsePadSample(int pad) {
        // remember which pad wants a sample; the Samples screen offers "Use on drum pad"
        ctx().session->project.previewTrack.clear();
        juce::ignoreUnused(pad);
        ctx().go(ScreenId::Samples);
    }
    void importPadSample(int pad) {
        chooser = std::make_unique<juce::FileChooser>("Import a drum sample", Paths::root(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, pad](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            auto* t = session().track();
            if (f == juce::File() || !t) return;
            juce::String err;
            if (!SampleBank::decodeFile(f, err)) { ctx().toast(err); return; }
            const auto dest = Paths::imports().getChildFile(f.getFileName());
            Paths::imports().createDirectory();
            if (!dest.existsAsFile()) f.copyFileTo(dest);
            session().checkpoint("Pad sample");
            t->inst.pads[(size_t)pad].sample = "lib:Imported/" + f.getFileName();
            t->inst.pads[(size_t)pad].name = f.getFileNameWithoutExtension();
            session().changed(true);
        });
    }

    struct PatternChips : juce::Component {
        struct Item { juce::String id, name; juce::Colour c; bool sel; };
        std::vector<Item> items;
        std::function<void(const juce::String&)> onSelect;
        std::function<void()> onAdd;
        void clear() { items.clear(); }
        void addPattern(const juce::String& id, const juce::String& n, juce::Colour c, bool s) { items.push_back({ id, n, c, s }); }
        juce::Rectangle<int> chip(int i) const { return { i * 52, 0, 48, getHeight() }; }
        void paint(juce::Graphics& g) override {
            for (int i = 0; i < (int)items.size(); ++i) {
                auto r = chip(i).toFloat().reduced(2.f, 4.f);
                g.setColour(items[(size_t)i].sel ? items[(size_t)i].c : pal().raised);
                g.fillRoundedRectangle(r, rControl);
                g.setColour(items[(size_t)i].sel ? juce::Colour(0xff17181b) : pal().text);
                g.setFont(font(15.f, true));
                g.drawFittedText(items[(size_t)i].name, r.toNearestInt(), juce::Justification::centred, 1);
            }
            auto r = chip((int)items.size()).toFloat().reduced(2.f, 4.f);
            g.setColour(pal().line);
            g.drawRoundedRectangle(r, rControl, 1.5f);
            drawIcon(g, "plus", r.withSizeKeepingCentre(18.f, 18.f), pal().text);
        }
        void resized() override { setSize(juce::jmax(getParentWidth() > 0 ? 0 : 0, getWidth()), getHeight()); repaint(); }
        void mouseUp(const juce::MouseEvent& e) override {
            const int i = e.x / 52;
            if (i < (int)items.size()) { if (onSelect) onSelect(items[(size_t)i].id); }
            else if (i == (int)items.size() && onAdd) onAdd();
        }
    };

    StepGrid grid;
    PadGrid pads;
    PatternChips patterns;
    juce::Viewport scroller;
    IconButton playB { "play", {}, "Preview this pattern" }, addB { "plus", "Add to song", "Place this pattern in the song at the playhead" }, chainB { "copy", {}, "Chain patterns into the song" },
        patMenuB { "more", {}, "Pattern options" }, padsB { "drums", "Pads", "Finger drumming pads" };
    juce::TextButton stepsB, swingB, kitB;
    Chip modeSteps { "Steps" }, modeVel { "Velocity" }, modeProb { "Chance" }, recPads { "Record pads" };
    std::unique_ptr<juce::FileChooser> chooser;
    int padTarget = -1;
};

}  // namespace

std::unique_ptr<Screen> makeDrumScreen() { return std::make_unique<DrumScreen>(); }
