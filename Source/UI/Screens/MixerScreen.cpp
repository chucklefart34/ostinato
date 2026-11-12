#include "Screens.h"
#include "Engine/Catalog.h"

using namespace UI;

namespace {

class Strip : public juce::Component, private juce::Timer {
public:
    Strip(Session& s, const juce::String& trackId) : session(s), id(trackId) {
        Track* t = track();
        if (!t) return;
        const bool master = t->type == TrackType::Master;
        fader = std::make_unique<Fader>(t->channel, 0, t->colour);
        addAndMakeVisible(*fader);
        pan = std::make_unique<Knob>(t->channel, 1, "ch:pan", id);
        pan->setCompact(true);
        pan->setColour(t->colour);
        addAndMakeVisible(*pan);
        if (!master && t->type != TrackType::Return) {
            const int returns = (int)s.project.returnTracks().size();
            for (int k = 0; k < returns; ++k) {
                auto* kn = sends.add(new Knob(t->channel, 6 + k, "ch:send" + juce::String(k + 1), id));
                kn->setCompact(true);
                kn->setColour(s.project.returnTracks()[(size_t)k]->colour);
                addAndMakeVisible(kn);
            }
        }
        meter.setSource(s.engine.channelFor(id));
        addAndMakeVisible(meter);
        for (auto* b : { &muteB, &soloB, &phaseB, &monoB }) { addAndMakeVisible(b); b->setClickingTogglesState(false); }
        muteB.onClick = [this] { toggle(2); };
        soloB.onClick = [this] { toggle(3); };
        phaseB.onClick = [this] { toggle(4); };
        monoB.onClick = [this] { toggle(5); };
        soloB.setVisible(!master);
        if (!master && t->type != TrackType::Return) {
            addAndMakeVisible(outB);
            outB.onClick = [this] { routeMenu(); };
        }
        startTimerHz(10);
        lp.fire = [this](juce::Point<float>) { haptic(1); ScreenUtil::trackMenu(session, id, this); };
        setTitle(t->name);
    }
    Track* track() { return session.project.findTrack(id); }
    void toggle(int idx) {
        Track* t = track();
        if (!t) return;
        haptic(0);
        session.checkpoint(juce::String(t->channel->meta(idx).name));
        t->channel->set(idx, t->channel->getBool(idx) ? 0.f : 1.f);
        session.changed();
        timerCallback();
    }
    void timerCallback() override {
        Track* t = track();
        if (!t) return;
        auto st = [](juce::TextButton& b, bool on, juce::Colour c) { b.setToggleState(on, juce::dontSendNotification); b.setColour(juce::TextButton::buttonOnColourId, c); };
        st(muteB, t->channel->getBool(2), pal().warn);
        st(soloB, t->channel->getBool(3), pal().accent);
        st(phaseB, t->channel->getBool(4), pal().play);
        st(monoB, t->channel->getBool(5), pal().play);
        if (meterSrcMissing && session.engine.channelFor(id)) { meter.setSource(session.engine.channelFor(id)); meterSrcMissing = false; }
        juce::String out = "Master";
        if (auto* o = session.project.findTrack(t->output); o && o->type == TrackType::Bus) out = o->name;
        outB.setButtonText(out);
        repaint(0, 0, getWidth(), 70);
    }
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        Track* t = track();
        if (!t) return;
        auto r = getLocalBounds().toFloat().reduced(3.f, 0.f);
        const bool sel = session.selectedTrack == id;
        g.setColour(sel ? p.raised : p.surface);
        g.fillRoundedRectangle(r, rControl);
        g.setColour(t->colour);
        g.fillRoundedRectangle(r.removeFromTop(6.f), 3.f);
        auto name = r.removeFromTop(34.f).reduced(6.f, 0.f);
        g.setColour(p.text);
        g.setFont(font(13.f, true));
        g.drawFittedText(t->name, name.toNearestInt(), juce::Justification::centred, 2);
        // inserts
        auto ins = insertsArea().toFloat();
        g.setFont(font(11.f));
        for (int i = 0; i < 3; ++i) {
            auto slot = ins.removeFromTop(22.f).reduced(0.f, 1.5f);
            g.setColour(p.bg);
            g.fillRoundedRectangle(slot, 5.f);
            if (i < (int)t->effects.size()) {
                const auto* info = Catalog::findEffect(t->effects[(size_t)i].type);
                const bool byp = t->effects[(size_t)i].bypass->load();
                g.setColour(byp ? p.faint : p.text);
                g.drawFittedText(info ? info->name : "?", slot.reduced(6.f, 0.f).toNearestInt(), juce::Justification::centredLeft, 1);
            } else if (i == (int)t->effects.size()) {
                g.setColour(p.dim);
                g.drawFittedText("+ Effect", slot.reduced(6.f, 0.f).toNearestInt(), juce::Justification::centredLeft, 1);
            }
        }
        if (t->effects.size() > 3) { g.setColour(p.dim); g.drawText("+" + juce::String((int)t->effects.size() - 3) + " more", ins.removeFromTop(14.f).toNearestInt(), juce::Justification::centred); }
        // fader value
        g.setColour(p.dim);
        g.setFont(font(11.5f, true));
        const float db = t->channel->get(0);
        g.drawText(db <= -59.9f ? juce::String("-inf") : juce::String(db, 1), fader->getBounds().withHeight(16).translated(0, -16), juce::Justification::centred);
    }
    juce::Rectangle<int> insertsArea() const { return { 9, 44, getWidth() - 18, 80 }; }
    void resized() override {
        auto r = getLocalBounds().reduced(9, 0);
        r.removeFromTop(44 + 82);
        if (!sends.isEmpty()) {
            auto sr = r.removeFromTop(sends.size() > 2 ? 104 : 52);
            const int w = sr.getWidth() / 2;
            for (int i = 0; i < sends.size(); ++i) sends[i]->setBounds(sr.getX() + (i % 2) * w, sr.getY() + (i / 2) * 52, w, 52);
        }
        pan->setBounds(r.removeFromTop(58).withSizeKeepingCentre(58, 58));
        auto b1 = r.removeFromTop(34);
        muteB.setBounds(b1.removeFromLeft(b1.getWidth() / 2).reduced(1));
        soloB.setBounds(b1.reduced(1));
        auto b2 = r.removeFromTop(34);
        phaseB.setBounds(b2.removeFromLeft(b2.getWidth() / 2).reduced(1));
        monoB.setBounds(b2.reduced(1));
        if (outB.isVisible()) outB.setBounds(r.removeFromBottom(36).reduced(0, 3));
        r.removeFromTop(18);
        r.removeFromBottom(6);
        meter.setBounds(r.removeFromRight(16));
        fader->setBounds(r);
    }
    void mouseDown(const juce::MouseEvent& e) override { if (e.mods.isPopupMenu()) { ScreenUtil::trackMenu(session, id, this); return; } lp.down(e.position); }
    void mouseDrag(const juce::MouseEvent& e) override { lp.moved(e.position); }
    void mouseUp(const juce::MouseEvent& e) override {
        lp.up();
        if (lp.didFire() || e.mods.isPopupMenu()) return;
        session.selectTrack(id);
        if (insertsArea().contains(e.getPosition())) ctx().go(ScreenId::Effects);
        getParentComponent()->repaint();
    }
    void routeMenu() {
        juce::PopupMenu m;
        m.addItem(1, "Master", true, track()->output == "master");
        std::vector<juce::String> ids;
        for (auto& o : session.project.tracks) if (o.type == TrackType::Bus && o.id != id) { ids.push_back(o.id); m.addItem((int)ids.size() + 1, o.name, true, track()->output == o.id); }
        m.addSeparator();
        m.addItem(100, "New group bus...");
        showMenu(m, &outB, [this, ids](int r) {
            Track* t = track();
            if (!t || r == 0) return;
            if (r == 100) {
                const auto busId = ScreenUtil::addTrack(session, TrackType::Bus);
                if (auto* tt = session.project.findTrack(id)) { tt->output = busId; session.changed(true); }
                return;
            }
            session.checkpoint("Change output");
            t->output = r == 1 ? juce::String("master") : ids[(size_t)r - 2];
            session.changed(true);
        });
    }
    bool meterSrcMissing = true;
private:
    Session& session;
    juce::String id;
    std::unique_ptr<Fader> fader;
    std::unique_ptr<Knob> pan;
    juce::OwnedArray<Knob> sends;
    Meter meter;
    juce::TextButton muteB { "M" }, soloB { "S" }, phaseB { juce::CharPointer_UTF8("\xc3\x98") }, monoB { "Mono" }, outB;
    LongPress lp;
};

class MixerScreen : public Screen {
public:
    MixerScreen() {
        addAndMakeVisible(scroller);
        scroller.setScrollBarsShown(false, true);
        scroller.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
        scroller.setViewedComponent(&strips, false);
        addAndMakeVisible(addB);
        addB.onClick = [this] { ScreenUtil::addTrackMenu(session(), &addB); };
    }
    juce::String title() const override { return "Mixer"; }
    void refresh() override {
        // rebuild only when the track list changed (keeps controls stable while dragging)
        juce::StringArray ids;
        for (auto& t : proj().tracks) ids.add(t.id + t.name + juce::String((int)t.effects.size()) + juce::String((int)proj().returnTracks().size()) + t.colour.toString() + t.channel->meta(0).id);
        ids.add(juce::String((juce::pointer_sized_int)proj().master.channel.get()));
        for (auto& t : proj().tracks) ids.add(juce::String((juce::pointer_sized_int)t.channel.get()));
        if (ids != lastIds) {
            lastIds = ids;
            stripList.clear();
            strips.removeAllChildren();
            for (auto& t : proj().tracks) { auto* s = stripList.add(new Strip(session(), t.id)); strips.addAndMakeVisible(s); }
            master = std::make_unique<Strip>(session(), "master");
            addAndMakeVisible(*master);
            resized();
        }
        for (auto* s : stripList) s->repaint();
        if (master) master->repaint();
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(pal().bg);
        if (proj().tracks.empty()) { g.setColour(pal().dim); g.setFont(font(16.f)); g.drawFittedText("No tracks to mix yet.", getLocalBounds().reduced(30), juce::Justification::centred, 2); }
    }
    void resized() override {
        auto r = getLocalBounds().reduced(4, 6);
        const int w = r.getWidth() < 520 ? 92 : 104;
        auto top = r.removeFromTop(48);
        addB.setBounds(top.removeFromLeft(130).reduced(2, 2));
        r.removeFromTop(4);
        if (master) master->setBounds(r.removeFromRight(w + 6).withTrimmedLeft(6));
        scroller.setBounds(r);
        strips.setSize(juce::jmax(r.getWidth(), stripList.size() * w), r.getHeight() - (stripList.size() * w > r.getWidth() ? 8 : 0));
        for (int i = 0; i < stripList.size(); ++i) stripList[i]->setBounds(i * w, 0, w, strips.getHeight());
    }
private:
    juce::Viewport scroller;
    juce::Component strips;
    juce::OwnedArray<Strip> stripList;
    std::unique_ptr<Strip> master;
    juce::StringArray lastIds;
    IconButton addB { "plus", "Track", "Add a track, bus or effect return" };
};

}  // namespace

std::unique_ptr<Screen> makeMixerScreen() { return std::make_unique<MixerScreen>(); }
