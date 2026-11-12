#include "Screens.h"
#include "Core/MidiIO.h"
#include "Core/Paths.h"
#include "Core/Settings.h"
#include "Core/Theory.h"
#include <climits>

using namespace UI;

namespace {

// ------------------------------------------------------------------ audio clip editor
class AudioClipSheet : public juce::Component, private juce::Timer {
public:
    AudioClipSheet(Session& s, juce::String clipId) : session(s), id(std::move(clipId)) {
        auto addKnob = [this](ParamSetPtr ps, int i) { auto* k = knobs.add(new Knob(ps, i)); k->onChange = [this] { apply(); }; addAndMakeVisible(k); };
        params = std::make_shared<ParamSet>(kInfos, 6);
        if (auto* c = clip()) {
            params->set(0, c->audio.gainDb); params->set(1, (float)c->audio.fadeInSec); params->set(2, (float)c->audio.fadeOutSec);
            params->set(3, c->audio.pitchSt); params->set(4, c->audio.stretch * 100.f); params->set(5, (float)c->audio.offsetSec);
        }
        for (int i = 0; i < 6; ++i) addKnob(params, i);
        for (auto* b : { &rev, &norm, &loop, &keep }) { addAndMakeVisible(b); b->onClick = [this] { apply(); }; }
        if (auto* c = clip()) {
            rev.setToggleState(c->audio.reverse, juce::dontSendNotification); norm.setToggleState(c->audio.normalize, juce::dontSendNotification);
            loop.setToggleState(c->audio.loop, juce::dontSendNotification); keep.setToggleState(c->audio.keepPitch, juce::dontSendNotification);
        }
        addAndMakeVisible(done);
        done.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible(takes);
        takes.setLabel("Take");
        if (auto* c = clip()) {
            juce::StringArray names;
            int sel = 0;
            for (int i = 0; i < c->audio.takes.size(); ++i) {
                names.add("Take " + juce::String(i + 1));
                if (std::abs(c->audio.takes[i].fromLastOccurrenceOf("|", false, false).getDoubleValue() - c->audio.offsetSec) < 1e-4) sel = i;
            }
            takes.setItems(names.isEmpty() ? juce::StringArray { "Take 1" } : names, sel);
        }
        takes.onChange = [this](int i) {
            auto* c = clip();
            if (!c || i >= c->audio.takes.size()) return;
            session.checkpoint("Choose take");
            c->audio.file = c->audio.takes[i].upToLastOccurrenceOf("|", false, false);
            c->audio.offsetSec = c->audio.takes[i].fromLastOccurrenceOf("|", false, false).getDoubleValue();
            params->set(5, (float)c->audio.offsetSec);
            session.changed();
            repaint();
        };
        addAndMakeVisible(bpmB);
        bpmB.onClick = [this] {
            auto* c = clip();
            if (!c) return;
            auto smp = SampleBank::get().loadNow(c->audio.file);
            const double bpm = smp ? Theory::detectBpm(*smp) : 0.0;
            if (bpm <= 0.0) { ctx().toast("No steady tempo found in this audio"); return; }
            const double proj = session.project.bpmAt(c->start);
            confirm("Detected " + juce::String(bpm, 1) + " BPM", "Stretch the clip to the song tempo (" + juce::String(proj, 1) + " BPM), keeping its pitch?", "Stretch", [this, bpm, proj] {
                params->set(4, (float)(bpm / proj * 100.0));
                keep.setToggleState(true, juce::dontSendNotification);
                apply();
            });
        };
        startTimerHz(10);
    }
    std::function<void()> onClose;
    Clip* clip() { for (auto& t : session.project.tracks) for (auto& c : t.clips) if (c.id == id) return &c; return nullptr; }
    void apply() {
        auto* c = clip();
        if (!c) return;
        if (!editing) { session.checkpoint("Edit audio clip"); editing = true; }
        c->audio.gainDb = params->get(0); c->audio.fadeInSec = params->get(1); c->audio.fadeOutSec = params->get(2);
        c->audio.pitchSt = params->get(3); c->audio.stretch = params->get(4) * 0.01f; c->audio.offsetSec = params->get(5);
        c->audio.reverse = rev.getToggleState(); c->audio.normalize = norm.getToggleState(); c->audio.loop = loop.getToggleState(); c->audio.keepPitch = keep.getToggleState();
        session.changed();
        repaint();
    }
    void timerCallback() override { editing = false; }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black.withAlpha(0.5f));
        auto card = cardBounds();
        g.setColour(pal().surface);
        g.fillRoundedRectangle(card.toFloat(), rSheet);
        auto r = card.reduced(18);
        auto* c = clip();
        heading(g, c ? c->name : juce::String("Clip"), r.removeFromTop(28), 19.f);
        auto wave = r.removeFromTop(110).toFloat();
        g.setColour(pal().raised);
        g.fillRoundedRectangle(wave, rClip);
        if (c) if (auto smp = SampleBank::get().request(c->audio.file)) {
            const double dur = session.project.ticksToSeconds(c->end()) - session.project.ticksToSeconds(c->start);
            const double speed = c->audio.keepPitch ? 1.0 / c->audio.stretch : std::pow(2.0, c->audio.pitchSt / 12.0) / c->audio.stretch;
            drawWaveform(g, *smp, wave.reduced(4.f), pal().accent, c->audio.offsetSec, dur * speed, c->audio.reverse);
            g.setColour(pal().dim); g.setFont(font(12.f));
            g.drawText(juce::String(smp->sampleRate / 1000.0, 1) + " kHz, " + formatDuration(smp->seconds()) + " source", wave.reduced(8.f).toNearestInt(), juce::Justification::bottomRight);
        }
    }
    juce::Rectangle<int> cardBounds() const { const int w = juce::jmin(getWidth() - 20, 620); return { (getWidth() - w) / 2, juce::jmax(10, getHeight() / 2 - 300), w, juce::jmin(getHeight() - 20, 600) }; }
    void resized() override {
        auto r = cardBounds().reduced(18);
        r.removeFromTop(28 + 110 + 10);
        auto row = r.removeFromTop(100);
        const int kw = row.getWidth() / 6;
        for (auto* k : knobs) k->setBounds(row.removeFromLeft(kw));
        r.removeFromTop(8);
        auto toggles = r.removeFromTop(46);
        const int tw = toggles.getWidth() / 4;
        for (auto* b : { &rev, &norm, &loop, &keep }) b->setBounds(toggles.removeFromLeft(tw).reduced(3));
        r.removeFromTop(8);
        auto row2 = r.removeFromTop(46);
        takes.setBounds(row2.removeFromLeft(row2.getWidth() / 2).reduced(3));
        bpmB.setBounds(row2.reduced(3));
        done.setBounds(r.removeFromBottom(touch).removeFromRight(140));
    }
    void mouseUp(const juce::MouseEvent& e) override { if (!cardBounds().contains(e.getPosition()) && onClose) onClose(); }
private:
    static constexpr ParamInfo kInfos[6] = {
        { "gain", "Gain", -36.f, 24.f, 0.f, "dB", ParamScale::Linear, nullptr, "" },
        { "fadein", "Fade in", 0.f, 10.f, 0.f, "s", ParamScale::Linear, nullptr, "" },
        { "fadeout", "Fade out", 0.f, 10.f, 0.f, "s", ParamScale::Linear, nullptr, "" },
        { "pitch", "Pitch", -24.f, 24.f, 0.f, "st", ParamScale::Linear, nullptr, "" },
        { "stretch", "Length", 25.f, 400.f, 100.f, "%", ParamScale::Log, nullptr, "" },
        { "offset", "Start", 0.f, 600.f, 0.f, "s", ParamScale::Linear, nullptr, "" },
    };
    Session& session;
    juce::String id;
    ParamSetPtr params;
    juce::OwnedArray<Knob> knobs;
    Chip rev { "Reverse" }, norm { "Normalize" }, loop { "Loop" }, keep { "Keep pitch" };
    Picker takes;
    juce::TextButton done { "Done" }, bpmB { "Detect tempo" };
    bool editing = false;
};
constexpr ParamInfo AudioClipSheet::kInfos[6];

// ------------------------------------------------------------------ timeline
class Timeline : public juce::Component, public juce::DragAndDropTarget, private juce::Timer {
public:
    explicit Timeline(Session& s) : session(s) {
        setWantsKeyboardFocus(false);
        lp.fire = [this](juce::Point<float> p) { longPress(p); };
        startTimerHz(30);
        setTitle("Arrangement");
    }

    // view state
    double pxPerTick = 0.04;      // ~38 px per beat
    double scrollTick = 0.0;
    float scrollY = 0.f, vZoom = 1.f;
    bool drawMode = false;
    std::function<void(const juce::String&)> openAudioClip;

    int headerW() const { return getWidth() < 520 ? 112 : 196; }
    static constexpr int rulerH = 40;
    float trackH() const { return 64.f * vZoom; }
    static constexpr float laneH = 52.f;

    struct Row { int track; int lane; float y, h; };
    std::vector<Row> rows;

    void layoutRows() {
        rows.clear();
        float y = rulerH - scrollY;
        for (int i = 0; i < (int)proj().tracks.size(); ++i) {
            auto& t = proj().tracks[(size_t)i];
            rows.push_back({ i, -1, y, trackH() });
            y += trackH();
            if (!t.collapsed)
                for (int l = 0; l < (int)t.automation.size(); ++l) if (t.automation[(size_t)l].shown) { rows.push_back({ i, l, y, laneH }); y += laneH; }
        }
        contentH = y + scrollY - rulerH + 120.f;
    }
    float contentH = 0.f;

    Project& proj() { return session.project; }
    float tickToX(double t) const { return (float)(headerW() + (t - scrollTick) * pxPerTick); }
    double xToTick(float x) const { return scrollTick + (x - headerW()) / pxPerTick; }
    int snap(double tick, bool round = true) { return snapTick(proj(), (int)std::max(0.0, tick), proj().snap, round); }
    const Row* rowAt(float y) const { for (auto& r : rows) if (y >= r.y && y < r.y + r.h) return &r; return nullptr; }

    // ---------------------------------------------------------------- painting
    void paint(juce::Graphics& g) override {
        layoutRows();
        const auto& p = pal();
        g.fillAll(p.bg);
        const int hw = headerW();
        auto lanes = getLocalBounds().withTrimmedLeft(hw).withTrimmedTop(rulerH);
        // bar grid
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(lanes);
            drawGrid(g, lanes);
            for (auto& r : rows) drawRow(g, r);
            if (proj().loopOn) {
                g.setColour(p.accent.withAlpha(0.06f));
                g.fillRect(juce::Rectangle<float>(tickToX(proj().loopStart), (float)rulerH, tickToX(proj().loopEnd) - tickToX(proj().loopStart), (float)getHeight()));
            }
            if (rubber.has_value()) { g.setColour(p.accent.withAlpha(0.15f)); g.fillRect(*rubber); g.setColour(p.accent); g.drawRect(*rubber, 1.f); }
            if (dropHighlight.has_value()) { g.setColour(p.accent.withAlpha(0.3f)); g.fillRoundedRectangle(*dropHighlight, rClip); }
        }
        drawRuler(g);
        drawHeaders(g);
        // playhead
        const float px = tickToX(ctx().engine->playheadTick());
        if (px >= hw && px < getWidth()) {
            g.setColour(p.accent);
            g.fillRect(px - 1.f, (float)rulerH - 10.f, 2.f, (float)getHeight());
            juce::Path tri; tri.addTriangle(px - 7.f, (float)rulerH - 14.f, px + 7.f, (float)rulerH - 14.f, px, (float)rulerH - 4.f);
            g.fillPath(tri);
        }
        if (proj().tracks.empty()) {
            g.setColour(p.dim);
            g.setFont(font(16.f));
            g.drawFittedText("This song has no tracks yet. Tap + to add an instrument, drums or an audio track.", lanes.reduced(30), juce::Justification::centred, 3);
        }
    }

    void drawGrid(juce::Graphics& g, juce::Rectangle<int> area) {
        const auto& p = pal();
        const double t0 = xToTick((float)area.getX()), t1 = xToTick((float)area.getRight());
        int bar = proj().tickToBar((int)std::max(0.0, t0));
        const int beatTicks = PPQ * 4 / proj().timeSigAtBar(bar).den;
        const bool showBeats = beatTicks * pxPerTick > 14.0;
        const bool showSixteenths = beatTicks / 4 * pxPerTick > 12.0;
        for (int guard = 0; guard < 2000; ++guard) {
            const int bt = proj().barToTick(bar);
            if (bt > t1) break;
            const int next = proj().barToTick(bar + 1);
            const float x = tickToX(bt);
            if ((bar % 2) == 1) { g.setColour(p.surface.withAlpha(0.5f)); g.fillRect(juce::Rectangle<float>(x, (float)area.getY(), tickToX(next) - x, (float)area.getHeight())); }
            g.setColour(p.line);
            g.fillRect(x, (float)area.getY(), 1.f, (float)area.getHeight());
            if (showBeats) {
                const int bTicks = PPQ * 4 / proj().timeSigAtBar(bar).den;
                const int sub = showSixteenths ? bTicks / 4 : bTicks;
                for (int t = bt + sub; t < next; t += sub) {
                    g.setColour(p.line.withAlpha((t - bt) % bTicks == 0 ? 0.45f : 0.18f));
                    g.fillRect(tickToX(t), (float)area.getY(), 1.f, (float)area.getHeight());
                }
            }
            ++bar;
        }
    }

    void drawRuler(juce::Graphics& g) {
        const auto& p = pal();
        auto r = juce::Rectangle<int>(headerW(), 0, getWidth() - headerW(), rulerH);
        g.setColour(p.surface);
        g.fillRect(r);
        g.setColour(p.line);
        g.fillRect(r.getX(), rulerH - 1, r.getWidth(), 1);
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(r);
        // loop band
        const float lx0 = tickToX(proj().loopStart), lx1 = tickToX(proj().loopEnd);
        g.setColour(proj().loopOn ? p.accent.withAlpha(0.85f) : p.line);
        g.fillRoundedRectangle(lx0, 2.f, juce::jmax(4.f, lx1 - lx0), 8.f, 3.f);
        // bar numbers
        const double t0 = xToTick((float)r.getX()), t1 = xToTick((float)r.getRight());
        int bar = proj().tickToBar((int)std::max(0.0, t0));
        const float barPx = (float)(proj().ticksPerBarAt(proj().barToTick(bar)) * pxPerTick);
        const int every = barPx > 36.f ? 1 : (barPx > 14.f ? 4 : 8);
        g.setFont(font(12.f, true));
        for (int guard = 0; guard < 2000; ++guard) {
            const int bt = proj().barToTick(bar);
            if (bt > t1) break;
            if (bar % every == 0) {
                const float x = tickToX(bt);
                g.setColour(p.dim);
                g.fillRect(x, 24.f, 1.f, (float)rulerH - 24.f);
                g.setColour(p.text.withAlpha(0.8f));
                g.drawText(juce::String(bar + 1), (int)x + 4, 22, 40, 16, juce::Justification::centredLeft);
            }
            ++bar;
        }
        // markers
        for (auto& m : proj().markers) {
            const float x = tickToX(m.tick);
            g.setColour(m.colour);
            juce::Path flag; flag.addTriangle(x, 12.f, x + 10.f, 17.f, x, 22.f);
            g.fillPath(flag);
            g.fillRect(x, 12.f, 1.5f, (float)rulerH - 12.f);
            g.setFont(font(12.f, true));
            g.drawText(m.name, (int)x + 12, 11, 120, 13, juce::Justification::centredLeft);
        }
        // tempo changes
        for (size_t i = 1; i < proj().tempo.size(); ++i) {
            const float x = tickToX(proj().tempo[i].tick);
            g.setColour(p.warn);
            g.setFont(font(11.f));
            g.drawText(juce::String(proj().tempo[i].bpm, 0), (int)x + 2, 26, 40, 12, juce::Justification::centredLeft);
        }
    }

    void drawHeaders(juce::Graphics& g) {
        const auto& p = pal();
        const int hw = headerW();
        g.setColour(p.surface);
        g.fillRect(0, 0, hw, getHeight());
        g.setColour(p.line);
        g.fillRect(hw - 1, 0, 1, getHeight());
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(0, rulerH, hw, getHeight() - rulerH);
        for (auto& r : rows) {
            auto& t = proj().tracks[(size_t)r.track];
            auto rr = juce::Rectangle<float>(0.f, r.y, (float)hw - 1.f, r.h);
            if (r.lane >= 0) {
                g.setColour(p.bg);
                g.fillRect(rr);
                g.setColour(p.dim);
                g.setFont(font(12.f));
                g.drawFittedText(ScreenUtil::laneLabel(t, t.automation[(size_t)r.lane].target), rr.reduced(14.f, 4.f).withTrimmedRight(30.f).toNearestInt(), juce::Justification::centredLeft, 2);
                drawIcon(g, "close", { rr.getRight() - 30.f, rr.getCentreY() - 9.f, 18.f, 18.f }, p.dim);
                g.setColour(p.line); g.fillRect(rr.withHeight(1.f).withY(rr.getBottom() - 1.f));
                continue;
            }
            const bool sel = t.id == session.selectedTrack;
            g.setColour(sel ? p.raised : p.surface);
            g.fillRect(rr);
            g.setColour(t.colour);
            g.fillRect(rr.withWidth(5.f));
            g.setColour(p.line);
            g.fillRect(rr.withHeight(1.f).withY(rr.getBottom() - 1.f));
            auto inner = rr.reduced(12.f, 6.f).withTrimmedLeft(2.f);
            const bool small = r.h < 56.f || hw < 150;
            auto top = inner.removeFromTop(small ? inner.getHeight() * 0.5f : 22.f);
            drawIcon(g, trackIcon(t.type), top.removeFromLeft(18.f).withSizeKeepingCentre(16.f, 16.f), p.dim);
            top.removeFromLeft(6.f);
            g.setColour(p.text);
            g.setFont(font(14.f, true));
            g.drawFittedText(t.name, top.toNearestInt(), juce::Justification::centredLeft, 1);
            // M S R buttons
            auto btns = inner.withTrimmedTop(small ? 0.f : 4.f).removeFromTop(28.f);
            const auto& ch = *t.channel;
            auto btn = [&](const char* lbl, bool on, juce::Colour c) {
                auto b = btns.removeFromLeft(30.f).reduced(2.f, 1.f);
                g.setColour(on ? c : p.line);
                g.fillRoundedRectangle(b, 6.f);
                g.setColour(on ? juce::Colour(0xff17181b) : p.dim);
                g.setFont(font(12.f, true));
                g.drawText(lbl, b.toNearestInt(), juce::Justification::centred);
            };
            btn("M", ch.getBool(2), p.warn);
            btn("S", ch.getBool(3), p.accent);
            if (t.hasClips()) btn("R", t.armed, p.record);
            if (!t.automation.empty() && hw >= 150) drawIcon(g, t.collapsed ? "chevron" : "down", btns.removeFromLeft(26.f).withSizeKeepingCentre(16.f, 16.f), p.dim);
        }
        // corner (above headers)
        g.setColour(p.surface);
        g.fillRect(0, 0, hw, rulerH);
        g.setColour(p.line);
        g.fillRect(0, rulerH - 1, hw, 1);
        g.setColour(p.dim);
        g.setFont(font(12.f));
        g.drawText(snapTable()[(size_t)proj().snap].name + juce::String(" grid"), 12, 0, hw - 16, rulerH, juce::Justification::centredLeft);
    }

    juce::Rectangle<float> clipRect(const Row& r, const Clip& c) const {
        return { tickToX(c.start), r.y + 3.f, (float)(c.length * pxPerTick), r.h - 6.f };
    }

    void drawRow(juce::Graphics& g, const Row& r) {
        const auto& p = pal();
        auto& t = proj().tracks[(size_t)r.track];
        if (r.y > getHeight() || r.y + r.h < rulerH) return;
        if (r.lane >= 0) { drawLane(g, r, t, t.automation[(size_t)r.lane]); return; }
        g.setColour(p.line.withAlpha(0.5f));
        g.fillRect((float)headerW(), r.y + r.h - 1.f, (float)getWidth(), 1.f);
        for (auto& c : t.clips) {
            auto cr = clipRect(r, c);
            if (cr.getRight() < headerW() || cr.getX() > getWidth()) continue;
            const auto base = t.colour;
            g.setColour(base.withAlpha(c.muted ? 0.18f : 0.32f));
            g.fillRoundedRectangle(cr, rClip);
            g.setColour(base.withAlpha(c.muted ? 0.4f : 1.f));
            g.fillRoundedRectangle(cr.withHeight(16.f), rClip);
            g.fillRect(cr.withHeight(8.f).withY(cr.getY() + 8.f));
            if (c.selected) { g.setColour(p.text); g.drawRoundedRectangle(cr.reduced(0.5f), rClip, 2.f); }
            g.setColour(base.getPerceivedBrightness() > 0.6f ? juce::Colour(0xff17181b) : juce::Colours::white);
            g.setFont(font(11.5f, true));
            g.drawText(c.name + (c.muted ? " (muted)" : ""), cr.withHeight(16.f).reduced(6.f, 0.f).toNearestInt(), juce::Justification::centredLeft, true);
            auto body = cr.withTrimmedTop(17.f).reduced(2.f, 2.f);
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(body.toNearestInt());
            const auto ink = base.brighter(0.4f).withAlpha(c.muted ? 0.4f : 0.95f);
            if (c.type == ClipType::Midi && !c.notes.empty()) {
                int lo = 127, hi = 0;
                for (auto& n : c.notes) { lo = juce::jmin(lo, n.pitch); hi = juce::jmax(hi, n.pitch); }
                const float span = (float)juce::jmax(12, hi - lo + 1);
                const int loopLen = juce::jmax(1, c.contentLength - c.contentOffset);
                g.setColour(ink);
                for (int rep = 0; rep * loopLen < c.length; ++rep)
                    for (auto& n : c.notes) {
                        if (n.start < c.contentOffset || n.start >= c.contentLength) continue;
                        const int pos = n.start - c.contentOffset + rep * loopLen;
                        if (pos >= c.length) continue;
                        const float x = cr.getX() + (float)(pos * pxPerTick);
                        const float y = body.getBottom() - (n.pitch - lo + 1) / span * body.getHeight();
                        g.fillRect(x, y, juce::jmax(2.f, (float)(n.length * pxPerTick)), juce::jmax(2.f, body.getHeight() / span));
                    }
                if (c.length > loopLen) for (int rep = 1; rep * loopLen < c.length; ++rep) { g.setColour(base.withAlpha(0.6f)); g.fillRect(cr.getX() + (float)(rep * loopLen * pxPerTick), body.getY(), 1.f, body.getHeight()); }
            } else if (c.type == ClipType::Pattern) {
                if (const Pattern* pat = proj().findPattern(c.patternId)) {
                    const int plen = juce::jmax(1, pat->lengthTicks());
                    g.setColour(ink);
                    int lanesUsed = 0;
                    std::array<int, kDrumPads> laneIdx {}; laneIdx.fill(-1);
                    for (int pad = 0; pad < kDrumPads; ++pad) for (int st = 0; st < pat->steps; ++st) if (pat->grid[(size_t)pad][(size_t)st].on) { if (laneIdx[(size_t)pad] < 0) laneIdx[(size_t)pad] = lanesUsed++; break; }
                    const float lh = body.getHeight() / juce::jmax(4, lanesUsed);
                    for (int rep = 0; rep * plen < c.length; ++rep)
                        for (int pad = 0; pad < kDrumPads; ++pad)
                            for (int st = 0; st < pat->steps; ++st)
                                if (pat->grid[(size_t)pad][(size_t)st].on) {
                                    const int pos = rep * plen + st * pat->stepTicks;
                                    if (pos >= c.length) continue;
                                    g.fillRect(cr.getX() + (float)(pos * pxPerTick), body.getY() + laneIdx[(size_t)pad] * lh, juce::jmax(2.f, (float)(pat->stepTicks * pxPerTick) - 1.f), juce::jmax(1.5f, lh - 1.f));
                                }
                    g.setColour(base.withAlpha(0.7f));
                    for (int rep = 1; rep * plen < c.length; ++rep) g.fillRect(cr.getX() + (float)(rep * plen * pxPerTick), body.getY(), 1.f, body.getHeight());
                }
            } else if (c.type == ClipType::Audio) {
                if (auto smp = SampleBank::get().request(c.audio.file)) {
                    const double dur = proj().ticksToSeconds(c.end()) - proj().ticksToSeconds(c.start);
                    const double speed = c.audio.keepPitch ? 1.0 / c.audio.stretch : std::pow(2.0, c.audio.pitchSt / 12.0) / c.audio.stretch;
                    drawWaveform(g, *smp, body, ink, c.audio.offsetSec, dur * speed, c.audio.reverse);
                    if (c.audio.fadeInSec > 0) { const float fx = (float)(proj().secondsToTicks(proj().ticksToSeconds(c.start) + c.audio.fadeInSec) - c.start) * (float)pxPerTick; g.setColour(p.text.withAlpha(0.6f)); g.drawLine(cr.getX(), body.getBottom(), cr.getX() + fx, body.getY(), 1.5f); }
                    if (c.audio.fadeOutSec > 0) { const float fx = (float)(c.end() - proj().secondsToTicks(proj().ticksToSeconds(c.end()) - c.audio.fadeOutSec)) * (float)pxPerTick; g.setColour(p.text.withAlpha(0.6f)); g.drawLine(cr.getRight() - fx, body.getY(), cr.getRight(), body.getBottom(), 1.5f); }
                } else {
                    g.setColour(p.dim); g.setFont(font(11.f));
                    g.drawText(SampleBank::get().resolve(c.audio.file).existsAsFile() ? "Loading..." : "Missing audio file", body.toNearestInt(), juce::Justification::centredLeft);
                }
            }
            // resize handles hint on selected clips
            if (c.selected) { g.setColour(p.text.withAlpha(0.7f)); g.fillRoundedRectangle(cr.getRight() - 6.f, cr.getCentreY() - 10.f, 3.f, 20.f, 1.5f); g.fillRoundedRectangle(cr.getX() + 3.f, cr.getCentreY() - 10.f, 3.f, 20.f, 1.5f); }
        }
    }

    void drawLane(juce::Graphics& g, const Row& r, const Track& t, const AutomationLane& l) {
        const auto& p = pal();
        auto area = juce::Rectangle<float>((float)headerW(), r.y, (float)(getWidth() - headerW()), r.h);
        g.setColour(p.surface.withAlpha(0.7f));
        g.fillRect(area);
        g.setColour(p.line);
        g.fillRect(area.withHeight(1.f).withY(area.getBottom() - 1.f));
        if (l.points.empty()) return;
        auto yOf = [&](float v) { return area.getBottom() - 6.f - v * (area.getHeight() - 12.f); };
        juce::Path path;
        const float x0 = (float)headerW();
        path.startNewSubPath(x0, yOf(l.valueAt((int)std::max(0.0, xToTick(x0)))));
        for (float x = x0; x < getWidth(); x += 3.f) path.lineTo(x, yOf(l.valueAt((int)std::max(0.0, xToTick(x)))));
        g.setColour(t.colour.withAlpha(l.enabled ? 1.f : 0.4f));
        g.strokePath(path, juce::PathStrokeType(2.f));
        for (size_t i = 0; i < l.points.size(); ++i) {
            const auto& pt = l.points[i];
            const float x = tickToX(pt.tick);
            if (x < x0 - 10 || x > getWidth() + 10) continue;
            g.setColour((int)i == dragPoint && dragLane == &l ? p.accent : p.text);
            g.fillEllipse(x - 6.f, yOf(pt.value) - 6.f, 12.f, 12.f);
            g.setColour(t.colour);
            g.drawEllipse(x - 6.f, yOf(pt.value) - 6.f, 12.f, 12.f, 2.f);
        }
    }

    // ---------------------------------------------------------------- hit testing
    struct Hit { enum Kind { None, Ruler, LoopStart, LoopEnd, LoopBody, Marker, Header, HeaderBtn, LaneClose, Clip, ClipLeft, ClipRight, Lane, LanePoint, Empty } kind = None;
                 int track = -1, lane = -1, index = -1; char btn = 0; };

    Hit hitTest(juce::Point<float> p) {
        layoutRows();
        Hit h;
        const int hw = headerW();
        if (p.y < rulerH) {
            if (p.x < hw) return h;
            const float lx0 = tickToX(proj().loopStart), lx1 = tickToX(proj().loopEnd);
            if (p.y < 14.f) {
                if (std::abs(p.x - lx0) < 14.f) { h.kind = Hit::LoopStart; return h; }
                if (std::abs(p.x - lx1) < 14.f) { h.kind = Hit::LoopEnd; return h; }
                if (p.x > lx0 && p.x < lx1) { h.kind = Hit::LoopBody; return h; }
            }
            for (int i = 0; i < (int)proj().markers.size(); ++i)
                if (p.y >= 10.f && p.y < 26.f && p.x >= tickToX(proj().markers[(size_t)i].tick) - 4.f && p.x < tickToX(proj().markers[(size_t)i].tick) + 70.f) { h.kind = Hit::Marker; h.index = i; return h; }
            h.kind = Hit::Ruler;
            return h;
        }
        const Row* r = rowAt(p.y);
        if (!r) { h.kind = p.x < hw ? Hit::None : Hit::Empty; return h; }
        h.track = r->track; h.lane = r->lane;
        auto& t = proj().tracks[(size_t)r->track];
        if (p.x < hw) {
            if (r->lane >= 0) { h.kind = p.x > hw - 40 ? Hit::LaneClose : Hit::Header; return h; }
            const bool small = r->h < 56.f || hw < 150;
            const float by = small ? r->y + 6.f + (r->h - 12.f) * 0.5f : r->y + 6.f + 26.f;
            if (p.y >= by - 2.f && p.y < by + 30.f) {
                const float bx = p.x - 14.f;
                if (bx >= 0 && bx < 30) { h.kind = Hit::HeaderBtn; h.btn = 'M'; return h; }
                if (bx >= 30 && bx < 60) { h.kind = Hit::HeaderBtn; h.btn = 'S'; return h; }
                if (bx >= 60 && bx < 90 && t.hasClips()) { h.kind = Hit::HeaderBtn; h.btn = 'R'; return h; }
                if (bx >= 90 && bx < 116 && !t.automation.empty()) { h.kind = Hit::HeaderBtn; h.btn = 'A'; return h; }
            }
            h.kind = Hit::Header;
            return h;
        }
        if (r->lane >= 0) {
            auto& l = t.automation[(size_t)r->lane];
            auto yOf = [&](float v) { return r->y + r->h - 6.f - v * (r->h - 12.f); };
            for (int i = 0; i < (int)l.points.size(); ++i)
                if (juce::Point<float>(tickToX(l.points[(size_t)i].tick), yOf(l.points[(size_t)i].value)).getDistanceFrom(p) < 16.f) { h.kind = Hit::LanePoint; h.index = i; return h; }
            h.kind = Hit::Lane;
            return h;
        }
        for (int i = (int)t.clips.size() - 1; i >= 0; --i) {
            auto cr = clipRect(*r, t.clips[(size_t)i]);
            if (!cr.expanded(2.f, 0.f).contains(p)) continue;
            h.index = i;
            const float edge = juce::jlimit(8.f, 18.f, cr.getWidth() * 0.25f);
            if (p.x > cr.getRight() - edge) h.kind = Hit::ClipRight;
            else if (p.x < cr.getX() + edge) h.kind = Hit::ClipLeft;
            else h.kind = Hit::Clip;
            return h;
        }
        h.kind = Hit::Empty;
        return h;
    }

    // ---------------------------------------------------------------- gestures
    void mouseDown(const juce::MouseEvent& e) override {
        pinch.down(e);
        if (pinch.active()) { lp.cancel(); cancelDrag(); pinchStartPx = pxPerTick; pinchStartV = vZoom; pinchStartScroll = scrollTick; return; }
        downHit = hitTest(e.position);
        downPos = e.position;
        downScrollTick = scrollTick; downScrollY = scrollY;
        dragging = false;
        if (e.mods.isPopupMenu()) { longPress(e.position); downHit = {}; return; }
        lp.down(e.position);
        if (downHit.kind == Hit::Clip || downHit.kind == Hit::ClipLeft || downHit.kind == Hit::ClipRight) {
            auto& t = proj().tracks[(size_t)downHit.track];
            auto& c = t.clips[(size_t)downHit.index];
            if (!c.selected && !e.mods.isShiftDown()) clearSelection();
            c.selected = true;
            dragOrig.clear();
            for (auto& tt : proj().tracks) for (auto& cc : tt.clips) if (cc.selected) dragOrig[cc.id] = { cc.start, cc.length, cc.contentOffset, cc.audio.offsetSec };
            session.selectTrack(t.id);
            repaint();
        } else if (downHit.kind == Hit::LanePoint) {
            dragLane = &proj().tracks[(size_t)downHit.track].automation[(size_t)downHit.lane];
            dragPoint = downHit.index;
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        float sx, sy; juce::Point<float> c;
        if (pinch.drag(e, sx, sy, c)) {
            const double anchor = pinchStartScroll + (c.x - headerW()) / pinchStartPx;
            pxPerTick = juce::jlimit(0.002, 0.6, pinchStartPx * sx);
            vZoom = juce::jlimit(0.6f, 2.5f, pinchStartV * sy);
            scrollTick = std::max(0.0, anchor - (c.x - headerW()) / pxPerTick);
            repaint();
            return;
        }
        lp.moved(e.position);
        const auto d = e.position - downPos;
        if (!dragging && d.getDistanceFromOrigin() < 6.f) return;
        if (!dragging) { dragging = true; lp.cancel(); beginDrag(); }
        continueDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override {
        const bool wasPinch = pinch.active();
        pinch.up(e);
        lp.up();
        if (wasPinch) return;
        if (lp.didFire()) { dragging = false; return; }
        if (dragging) { endDrag(); dragging = false; return; }
        tap(e);
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override {
        auto h = hitTest(e.position);
        if (h.kind == Hit::Clip || h.kind == Hit::ClipLeft || h.kind == Hit::ClipRight) openClip(h.track, h.index);
        else if (h.kind == Hit::LanePoint) {
            session.checkpoint("Delete automation point");
            auto& l = proj().tracks[(size_t)h.track].automation[(size_t)h.lane];
            if (l.points.size() > 1) l.points.erase(l.points.begin() + h.index);
            session.changed();
        } else if (h.kind == Hit::Marker) markerMenu(h.index);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
            const double anchor = xToTick(e.position.x);
            pxPerTick = juce::jlimit(0.002, 0.6, pxPerTick * (w.deltaY > 0 ? 1.15 : 1.0 / 1.15));
            scrollTick = std::max(0.0, anchor - (e.position.x - headerW()) / pxPerTick);
        } else if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY)) {
            scrollTick = std::max(0.0, scrollTick - (w.deltaX != 0.f ? w.deltaX : w.deltaY) * 400.0 / pxPerTick * 0.25);
        } else {
            scrollY = juce::jlimit(0.f, juce::jmax(0.f, contentH - (getHeight() - rulerH)), scrollY - w.deltaY * 200.f);
        }
        repaint();
    }

    void mouseMagnify(const juce::MouseEvent& e, float scale) override {
        const double anchor = xToTick(e.position.x);
        pxPerTick = juce::jlimit(0.002, 0.6, pxPerTick * scale);
        scrollTick = std::max(0.0, anchor - (e.position.x - headerW()) / pxPerTick);
        repaint();
    }

    void tap(const juce::MouseEvent& e) {
        auto& h = downHit;
        switch (h.kind) {
            case Hit::Ruler: case Hit::LoopBody: {
                ctx().engine->seek(std::max(0, snap(xToTick(e.position.x))));
                break;
            }
            case Hit::Marker: ctx().engine->seek(proj().markers[(size_t)h.index].tick); break;
            case Hit::Header: session.selectTrack(proj().tracks[(size_t)h.track].id); break;
            case Hit::LaneClose: {
                session.checkpoint("Remove automation lane");
                auto& t = proj().tracks[(size_t)h.track];
                t.automation.erase(t.automation.begin() + h.lane);
                session.changed(true);
                break;
            }
            case Hit::HeaderBtn: {
                auto& t = proj().tracks[(size_t)h.track];
                haptic(0);
                if (h.btn == 'M') { t.channel->set(2, t.channel->getBool(2) ? 0.f : 1.f); session.changed(); }
                else if (h.btn == 'S') { t.channel->set(3, t.channel->getBool(3) ? 0.f : 1.f); session.changed(); }
                else if (h.btn == 'R') {
                    session.checkpoint("Arm track");
                    t.armed = !t.armed;
                    if (t.armed && t.type == TrackType::Audio && !ctx().engine->inputEnabled()) ctx().engine->enableInput([](bool ok) { if (!ok) ctx().toast("Microphone access is off"); });
                    session.changed(true);
                }
                else if (h.btn == 'A') { t.collapsed = !t.collapsed; session.changed(true); }
                break;
            }
            case Hit::Clip: case Hit::ClipLeft: case Hit::ClipRight: {
                auto& c = proj().tracks[(size_t)h.track].clips[(size_t)h.index];
                if (lastTapClip == c.id && juce::Time::getMillisecondCounter() - lastTapTime < 450) { openClip(h.track, h.index); lastTapClip.clear(); break; }
                lastTapClip = c.id; lastTapTime = juce::Time::getMillisecondCounter();
                if (e.mods.isShiftDown()) { c.selected = !c.selected; }
                break;
            }
            case Hit::Lane: {
                auto& t = proj().tracks[(size_t)h.track];
                auto& l = t.automation[(size_t)h.lane];
                const Row* r = rowAt(e.position.y);
                if (!r) break;
                const float v = juce::jlimit(0.f, 1.f, (r->y + r->h - 6.f - e.position.y) / (r->h - 12.f));
                session.checkpoint("Add automation point");
                l.points.push_back({ snap(xToTick(e.position.x)), v, l.points.empty() ? 0 : l.points.back().curve });
                std::sort(l.points.begin(), l.points.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
                session.changed();
                haptic(0);
                break;
            }
            case Hit::Empty: {
                clearSelection();
                if (h.track >= 0 && drawMode) createClipAt(h.track, xToTick(e.position.x));
                else {
                    if (h.track >= 0) session.selectTrack(proj().tracks[(size_t)h.track].id);
                    ctx().engine->seek(snap(xToTick(e.position.x)));
                }
                break;
            }
            default: break;
        }
        repaint();
    }

    void beginDrag() {
        auto& h = downHit;
        if (h.kind == Hit::Clip || h.kind == Hit::ClipLeft || h.kind == Hit::ClipRight) session.checkpoint(h.kind == Hit::Clip ? "Move clip" : "Resize clip");
        else if (h.kind == Hit::LanePoint) session.checkpoint("Move automation point");
        else if (h.kind == Hit::LoopStart || h.kind == Hit::LoopEnd || h.kind == Hit::LoopBody) { session.checkpoint("Loop region"); loopOrig = { proj().loopStart, proj().loopEnd }; }
        else if (h.kind == Hit::Empty && !drawMode && downHit.track >= 0 && false) {}
    }

    void continueDrag(const juce::MouseEvent& e) {
        auto& h = downHit;
        const auto d = e.position - downPos;
        const double dt = d.x / pxPerTick;
        switch (h.kind) {
            case Hit::Clip: {
                // move all selected clips; vertical drag moves between compatible tracks
                const int snapped = snap(dragOrig[proj().tracks[(size_t)h.track].clips[(size_t)h.index].id].start + dt) - dragOrig[proj().tracks[(size_t)h.track].clips[(size_t)h.index].id].start;
                for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.selected) c.start = juce::jmax(0, dragOrig[c.id].start + snapped);
                const Row* r = rowAt(e.position.y);
                if (r && r->lane < 0 && r->track != h.track) {
                    auto& from = proj().tracks[(size_t)h.track];
                    auto& to = proj().tracks[(size_t)r->track];
                    auto& c = from.clips[(size_t)h.index];
                    const bool compatible = (c.type == ClipType::Audio && to.type == TrackType::Audio) || (c.type == ClipType::Midi && to.hasInstrument())
                                            || (c.type == ClipType::Pattern && to.type == TrackType::Drum);
                    if (compatible) {
                        to.clips.push_back(c);
                        from.clips.erase(from.clips.begin() + h.index);
                        h.track = r->track;
                        h.index = (int)to.clips.size() - 1;
                        session.selectedTrack = to.id;
                    }
                }
                break;
            }
            case Hit::ClipRight: {
                for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.selected) {
                    const int end = snap(dragOrig[c.id].start + dragOrig[c.id].length + dt);
                    c.length = juce::jmax(snapTicks(proj(), proj().snap, c.start) > 1 ? snapTicks(proj(), proj().snap, c.start) : PPQ / 4, end - c.start);
                }
                break;
            }
            case Hit::ClipLeft: {
                for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.selected) {
                    const auto o = dragOrig[c.id];
                    const int end = o.start + o.length;
                    int ns = juce::jlimit(0, end - PPQ / 8, snap(o.start + dt));
                    if (c.type == ClipType::Midi) ns = juce::jmax(ns, o.start - o.contentOffset);   // can't trim before content start
                    const int delta = ns - o.start;
                    c.start = ns;
                    c.length = end - ns;
                    if (c.type == ClipType::Midi) c.contentOffset = juce::jlimit(0, c.contentLength - 1, o.contentOffset + delta);
                    if (c.type == ClipType::Audio) c.audio.offsetSec = juce::jmax(0.0, o.offsetSec + (proj().ticksToSeconds(ns) - proj().ticksToSeconds(o.start)) * (c.audio.keepPitch ? 1.0 / c.audio.stretch : std::pow(2.0, c.audio.pitchSt / 12.0) / c.audio.stretch));
                }
                break;
            }
            case Hit::LanePoint: {
                if (!dragLane) break;
                const Row* r = nullptr;
                for (auto& rr : rows) if (rr.track == h.track && rr.lane == h.lane) r = &rr;
                if (!r) break;
                auto& pt = dragLane->points[(size_t)dragPoint];
                pt.tick = snap(xToTick(e.position.x));
                pt.value = juce::jlimit(0.f, 1.f, (r->y + r->h - 6.f - e.position.y) / (r->h - 12.f));
                // keep ordering stable while dragging
                const int lo = dragPoint > 0 ? dragLane->points[(size_t)dragPoint - 1].tick : 0;
                const int hi = dragPoint + 1 < (int)dragLane->points.size() ? dragLane->points[(size_t)dragPoint + 1].tick : INT_MAX;
                pt.tick = juce::jlimit(lo, hi, pt.tick);
                break;
            }
            case Hit::LoopStart: proj().loopStart = juce::jlimit(0, loopOrig.second - PPQ / 4, snap(loopOrig.first + dt)); proj().loopOn = true; break;
            case Hit::LoopEnd: proj().loopEnd = juce::jmax(loopOrig.first + PPQ / 4, snap(loopOrig.second + dt)); proj().loopOn = true; break;
            case Hit::LoopBody: { const int len = loopOrig.second - loopOrig.first; proj().loopStart = juce::jmax(0, snap(loopOrig.first + dt)); proj().loopEnd = proj().loopStart + len; break; }
            case Hit::Ruler: ctx().engine->seek(std::max(0.0, xToTick(e.position.x))); break;
            case Hit::Empty: case Hit::Lane: case Hit::Header: case Hit::None: case Hit::Marker: {
                if (h.kind == Hit::Empty && drawMode) {   // rubber-band select in draw-off? in draw mode: draw a clip of the dragged length
                    rubber = juce::Rectangle<float>(downPos, e.position);
                    break;
                }
                if (h.kind == Hit::Empty && e.mods.isShiftDown()) { rubber = juce::Rectangle<float>(downPos, e.position); break; }
                scrollTick = std::max(0.0, downScrollTick - d.x / pxPerTick);
                scrollY = juce::jlimit(0.f, juce::jmax(0.f, contentH - (getHeight() - rulerH)), downScrollY - d.y);
                break;
            }
            default: break;
        }
        repaint();
    }

    void endDrag() {
        auto& h = downHit;
        if (rubber.has_value()) {
            if (drawMode && h.track >= 0) {
                const int s0 = snap(xToTick(rubber->getX()), false), s1 = snap(xToTick(rubber->getRight()));
                createClipAt(h.track, s0, juce::jmax(PPQ, s1 - s0));
            } else {
                for (auto& r : rows) {
                    if (r.lane >= 0) continue;
                    for (auto& c : proj().tracks[(size_t)r.track].clips) c.selected = clipRect(r, c).intersects(*rubber);
                }
            }
            rubber.reset();
        }
        if (h.kind == Hit::Clip || h.kind == Hit::ClipLeft || h.kind == Hit::ClipRight || h.kind == Hit::LanePoint || h.kind == Hit::LoopStart || h.kind == Hit::LoopEnd || h.kind == Hit::LoopBody)
            session.changed(true);
        dragLane = nullptr; dragPoint = -1;
        repaint();
    }
    void cancelDrag() { dragging = false; rubber.reset(); }

    // ---------------------------------------------------------------- actions
    void clearSelection() { for (auto& t : proj().tracks) for (auto& c : t.clips) c.selected = false; }

    void createClipAt(int trackIdx, double tick, int length = -1) {
        auto& t = proj().tracks[(size_t)trackIdx];
        const int start = snapTick(proj(), (int)std::max(0.0, tick), proj().snap, false);
        const int bar = proj().ticksPerBarAt(start);
        if (t.type == TrackType::Audio) { ctx().toast("Record or add a sample to put audio here"); return; }
        if (!t.hasInstrument()) return;
        session.checkpoint("Create clip");
        Clip c;
        if (t.type == TrackType::Drum) {
            Pattern* pat = session.pattern();
            if (!pat) { pat = &proj().addPattern(); session.currentPattern = pat->id; }
            c.id = proj().newId("c"); c.type = ClipType::Pattern; c.patternId = pat->id; c.name = pat->name;
            c.start = start; c.length = length > 0 ? length : juce::jmax(pat->lengthTicks(), bar);
        } else {
            c = proj().makeMidiClip(start, length > 0 ? length : bar);
            c.name = t.name;
        }
        c.selected = true;
        clearSelection();
        t.clips.push_back(c);
        session.selectTrack(t.id);
        session.changed(true);
        haptic(0);
    }

    void openClip(int trackIdx, int clipIdx) {
        auto& t = proj().tracks[(size_t)trackIdx];
        auto& c = t.clips[(size_t)clipIdx];
        session.selectTrack(t.id);
        if (c.type == ClipType::Midi) { session.editClipId = c.id; ctx().go(ScreenId::PianoRoll); }
        else if (c.type == ClipType::Pattern) { session.currentPattern = c.patternId; ctx().go(ScreenId::Drums); }
        else if (openAudioClip) openAudioClip(c.id);
    }

    void longPress(juce::Point<float> p) {
        haptic(1);
        auto h = hitTest(p);
        cancelDrag();
        if (h.kind == Hit::Header || h.kind == Hit::HeaderBtn) { ScreenUtil::trackMenu(session, proj().tracks[(size_t)h.track].id, this); return; }
        if (h.kind == Hit::Clip || h.kind == Hit::ClipLeft || h.kind == Hit::ClipRight) { clipMenu(h.track, h.index); return; }
        if (h.kind == Hit::LanePoint) { pointMenu(h.track, h.lane, h.index); return; }
        if (h.kind == Hit::Marker) { markerMenu(h.index); return; }
        if (h.kind == Hit::Ruler || h.kind == Hit::LoopBody || h.kind == Hit::LoopStart || h.kind == Hit::LoopEnd) { rulerMenu(xToTick(p.x)); return; }
        if (h.kind == Hit::Empty || h.kind == Hit::Lane) {
            const double tick = xToTick(p.x);
            juce::PopupMenu m;
            m.addItem(1, "Paste here", !clipboard.empty() && h.track >= 0);
            m.addItem(2, "New clip here", h.track >= 0 && proj().tracks[(size_t)h.track].hasInstrument());
            m.addItem(3, "Add marker here");
            m.addItem(4, "Select all clips");
            showMenu(m, this, [this, h, tick](int r) {
                if (r == 1) paste(h.track, snap(tick));
                else if (r == 2) createClipAt(h.track, tick);
                else if (r == 3) addMarker(snap(tick));
                else if (r == 4) { for (auto& t : proj().tracks) for (auto& c : t.clips) c.selected = true; repaint(); }
            });
        }
    }

    void rulerMenu(double tick) {
        juce::PopupMenu m;
        m.addItem(1, "Add marker here");
        m.addItem(2, "Add section marker here");
        m.addItem(3, "Set loop start here");
        m.addItem(4, "Set loop end here");
        m.addItem(5, proj().loopOn ? "Turn loop off" : "Turn loop on");
        m.addItem(6, "Loop the selected clips");
        m.addItem(7, "Duplicate the loop section (insert after)");
        showMenu(m, this, [this, tick](int r) {
            const int t = snap(tick);
            if (r == 1 || r == 2) addMarker(t, r == 2);
            else if (r == 3) { session.checkpoint("Loop start"); proj().loopStart = juce::jmin(t, proj().loopEnd - PPQ / 4); proj().loopOn = true; session.changed(); }
            else if (r == 4) { session.checkpoint("Loop end"); proj().loopEnd = juce::jmax(t, proj().loopStart + PPQ / 4); proj().loopOn = true; session.changed(); }
            else if (r == 5) { session.checkpoint("Toggle loop"); proj().loopOn = !proj().loopOn; session.changed(); }
            else if (r == 6) loopSelection();
            else if (r == 7) duplicateSection(proj().loopStart, proj().loopEnd);
        });
    }

    void loopSelection() {
        int lo = INT_MAX, hi = 0;
        for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.selected) { lo = juce::jmin(lo, c.start); hi = juce::jmax(hi, c.end()); }
        if (lo >= hi) { ctx().toast("Select one or more clips first"); return; }
        session.checkpoint("Loop selection");
        proj().loopStart = lo; proj().loopEnd = hi; proj().loopOn = true;
        session.changed();
    }

    // Inserts a copy of [a,b) right after it, shifting later material (song sections).
    void duplicateSection(int a, int b) {
        if (b <= a) return;
        session.checkpoint("Duplicate section");
        const int len = b - a;
        for (auto& t : proj().tracks) {
            std::vector<Clip> add;
            for (auto& c : t.clips) {
                if (c.start >= b) c.start += len;
                else if (c.start >= a) { Clip d = c; d.id = proj().newId("c"); d.start += len; d.selected = false; add.push_back(d); }
            }
            for (auto& d : add) t.clips.push_back(d);
            for (auto& l : t.automation) {
                std::vector<AutomationPoint> extra;
                for (auto& p : l.points) { if (p.tick >= b) p.tick += len; else if (p.tick >= a) extra.push_back({ p.tick + len, p.value, p.curve }); }
                for (auto& e : extra) l.points.push_back(e);
                std::sort(l.points.begin(), l.points.end(), [](auto& x, auto& y) { return x.tick < y.tick; });
            }
        }
        for (auto& m : proj().markers) if (m.tick >= b) m.tick += len;
        session.changed(true);
        ctx().toast("Section duplicated");
    }

    void addMarker(int tick, bool section = false) {
        askText(section ? "Section marker" : "Marker", "Name", section ? "Chorus" : "Marker " + juce::String((int)proj().markers.size() + 1), "Add", [this, tick, section](const juce::String& n) {
            session.checkpoint("Add marker");
            proj().markers.push_back({ tick, n, section, section ? pal().accent : Project::trackColour((int)proj().markers.size() + 5) });
            std::sort(proj().markers.begin(), proj().markers.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
            session.changed();
        });
    }

    void markerMenu(int i) {
        juce::PopupMenu m;
        m.addSectionHeader(proj().markers[(size_t)i].name);
        m.addItem(1, "Jump here");
        m.addItem(2, "Rename...");
        m.addItem(3, "Loop until next marker");
        m.addItem(4, "Duplicate this section");
        m.addItem(5, "Delete marker");
        showMenu(m, this, [this, i](int r) {
            if (i >= (int)proj().markers.size()) return;
            const int start = proj().markers[(size_t)i].tick;
            const int end = i + 1 < (int)proj().markers.size() ? proj().markers[(size_t)i + 1].tick : juce::jmax(start + proj().ticksPerBarAt(start) * 4, proj().songEndTick());
            if (r == 1) ctx().engine->seek(start);
            else if (r == 2) askText("Rename marker", {}, proj().markers[(size_t)i].name, "Rename", [this, i](const juce::String& n) { session.checkpoint("Rename marker"); proj().markers[(size_t)i].name = n; session.changed(); });
            else if (r == 3) { session.checkpoint("Loop section"); proj().loopStart = start; proj().loopEnd = end; proj().loopOn = true; session.changed(); }
            else if (r == 4) duplicateSection(start, end);
            else if (r == 5) { session.checkpoint("Delete marker"); proj().markers.erase(proj().markers.begin() + i); session.changed(); }
        });
    }

    void pointMenu(int track, int lane, int idx) {
        juce::PopupMenu m;
        auto& pt = proj().tracks[(size_t)track].automation[(size_t)lane].points[(size_t)idx];
        m.addItem(1, "Linear", true, pt.curve == 0);
        m.addItem(2, "Smooth curve", true, pt.curve == 1);
        m.addItem(3, "Hold (step)", true, pt.curve == 2);
        m.addSeparator();
        m.addItem(4, "Delete point");
        m.addItem(5, proj().tracks[(size_t)track].automation[(size_t)lane].enabled ? "Disable lane" : "Enable lane");
        m.addItem(6, "Clear lane");
        showMenu(m, this, [this, track, lane, idx](int r) {
            auto& l = proj().tracks[(size_t)track].automation[(size_t)lane];
            if (idx >= (int)l.points.size() || r == 0) return;
            session.checkpoint("Edit automation");
            if (r >= 1 && r <= 3) l.points[(size_t)idx].curve = r - 1;
            else if (r == 4 && l.points.size() > 1) l.points.erase(l.points.begin() + idx);
            else if (r == 5) l.enabled = !l.enabled;
            else if (r == 6) { const float v = l.points[(size_t)idx].value; l.points = { { 0, v, 0 } }; }
            session.changed(true);
        });
    }

    void clipMenu(int trackIdx, int clipIdx) {
        auto& t = proj().tracks[(size_t)trackIdx];
        auto& c = t.clips[(size_t)clipIdx];
        const auto id = c.id;
        juce::PopupMenu m;
        m.addSectionHeader(c.name);
        m.addItem(1, c.type == ClipType::Audio ? "Edit audio..." : (c.type == ClipType::Pattern ? "Edit pattern" : "Edit notes"));
        m.addItem(2, "Rename...");
        m.addItem(3, "Duplicate");
        m.addItem(4, "Split at playhead", ctx().engine->playheadTick() > c.start && ctx().engine->playheadTick() < c.end());
        m.addItem(5, "Copy");
        m.addItem(6, c.muted ? "Unmute" : "Mute");
        m.addItem(7, "Double length (repeat content)");
        m.addItem(8, "Loop this clip");
        if (c.type == ClipType::Midi) {
            m.addSeparator();
            m.addItem(20, "Quantize to grid");
            m.addItem(21, "Transpose up an octave");
            m.addItem(22, "Transpose down an octave");
            m.addItem(23, "Export as MIDI file...");
        }
        if (c.type == ClipType::Pattern) { m.addSeparator(); m.addItem(30, "Make pattern unique"); }
        if (c.type == ClipType::Audio) {
            m.addSeparator();
            m.addItem(40, "Reverse", true, c.audio.reverse);
            m.addItem(41, "Normalize", true, c.audio.normalize);
            m.addItem(42, "Loop audio", true, c.audio.loop);
            m.addItem(43, "Quick fades (10 ms)");
        }
        m.addSeparator();
        m.addItem(9, "Delete");
        showMenu(m, this, [this, trackIdx, id](int r) { clipAction(trackIdx, id, r); });
    }

    Clip* findClip(int trackIdx, const juce::String& id) {
        if (trackIdx >= (int)proj().tracks.size()) return nullptr;
        for (auto& c : proj().tracks[(size_t)trackIdx].clips) if (c.id == id) return &c;
        return nullptr;
    }

    void clipAction(int trackIdx, const juce::String& id, int r) {
        Clip* cp = findClip(trackIdx, id);
        if (!cp || r == 0) return;
        auto& t = proj().tracks[(size_t)trackIdx];
        if (r == 1) { for (int i = 0; i < (int)t.clips.size(); ++i) if (t.clips[(size_t)i].id == id) openClip(trackIdx, i); return; }
        if (r == 2) { askText("Rename clip", {}, cp->name, "Rename", [this, trackIdx, id](const juce::String& n) { if (auto* c = findClip(trackIdx, id)) { session.checkpoint("Rename clip"); c->name = n; session.changed(); } }); return; }
        if (r == 5) { copySelection(); return; }
        if (r == 23) {
            chooser = std::make_unique<juce::FileChooser>("Export MIDI clip", Paths::exports().getChildFile(juce::File::createLegalFileName(cp->name) + ".mid"), "*.mid");
            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, trackIdx, id](const juce::FileChooser& fc) {
                auto f = fc.getResult(); auto* c = findClip(trackIdx, id);
                if (f == juce::File() || !c) return;
                juce::String err;
                if (MidiIO::exportClip(proj(), *c, f.withFileExtension("mid"), err)) ctx().toast("Exported " + f.getFileName()); else ctx().toast(err);
            });
            return;
        }
        session.checkpoint("Edit clip");
        switch (r) {
            case 3: { Clip d = *cp; d.id = proj().newId("c"); d.start = cp->end(); d.selected = true; cp->selected = false; t.clips.push_back(d); break; }
            case 4: {
                const int at = snap(ctx().engine->playheadTick());
                if (at <= cp->start || at >= cp->end()) break;
                Clip right = *cp;
                right.id = proj().newId("c");
                const int cut = at - cp->start;
                right.start = at; right.length = cp->end() - at;
                if (cp->type == ClipType::Midi) {
                    // split notes between both halves (content positions are relative)
                    const int loopLen = juce::jmax(1, cp->contentLength - cp->contentOffset);
                    if (cp->length <= loopLen) {
                        right.notes.clear();
                        std::vector<Note> left;
                        for (auto& n : cp->notes) {
                            const int pos = n.start - cp->contentOffset;
                            if (pos < cut) left.push_back(n);
                            else { Note nn = n; nn.start = pos - cut; right.notes.push_back(nn); }
                        }
                        cp->notes = left;
                        right.contentOffset = 0;
                        right.contentLength = right.length;
                        cp->contentLength = cp->contentOffset + cut;
                    } else right.contentOffset = cp->contentOffset + cut % loopLen;   // looped clip: keep phase
                } else if (cp->type == ClipType::Audio) {
                    const double speed = cp->audio.keepPitch ? 1.0 / cp->audio.stretch : std::pow(2.0, cp->audio.pitchSt / 12.0) / cp->audio.stretch;
                    right.audio.offsetSec = cp->audio.offsetSec + (proj().ticksToSeconds(at) - proj().ticksToSeconds(cp->start)) * speed;
                    right.audio.fadeInSec = 0.0;
                    cp->audio.fadeOutSec = 0.0;
                }
                cp->length = cut;
                t.clips.push_back(right);
                break;
            }
            case 6: cp->muted = !cp->muted; break;
            case 7: cp->length *= 2; break;
            case 8: proj().loopStart = cp->start; proj().loopEnd = cp->end(); proj().loopOn = true; break;
            case 9: t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&id](auto& c) { return c.id == id; }), t.clips.end()); break;
            case 20: Theory::quantize(cp->notes, juce::jmax(PPQ / 8, snapTicks(proj(), proj().snap, cp->start)), 1.f, 0.f, false, false); break;
            case 21: Theory::transpose(cp->notes, 12, false); break;
            case 22: Theory::transpose(cp->notes, -12, false); break;
            case 30: if (auto* pat = proj().findPattern(cp->patternId)) { Pattern copy = *pat; copy.id = proj().newId("p"); copy.name = pat->name + " (copy)"; proj().patterns.push_back(copy); cp->patternId = copy.id; cp->name = copy.name; } break;
            case 40: cp->audio.reverse = !cp->audio.reverse; break;
            case 41: cp->audio.normalize = !cp->audio.normalize; break;
            case 42: cp->audio.loop = !cp->audio.loop; break;
            case 43: cp->audio.fadeInSec = cp->audio.fadeOutSec = 0.01; break;
            default: break;
        }
        session.changed(true);
    }

    // clipboard
    struct ClipCopy { int trackOffset; Clip clip; };
    std::vector<ClipCopy> clipboard;
    void copySelection() {
        clipboard.clear();
        int first = INT_MAX, firstTrack = INT_MAX;
        for (int i = 0; i < (int)proj().tracks.size(); ++i) for (auto& c : proj().tracks[(size_t)i].clips) if (c.selected) { first = juce::jmin(first, c.start); firstTrack = juce::jmin(firstTrack, i); }
        for (int i = 0; i < (int)proj().tracks.size(); ++i) for (auto& c : proj().tracks[(size_t)i].clips) if (c.selected) { Clip cc = c; cc.start -= first; clipboard.push_back({ i - firstTrack, cc }); }
        ctx().toast(clipboard.empty() ? "Select clips to copy" : "Copied " + juce::String((int)clipboard.size()) + " clip" + (clipboard.size() > 1 ? "s" : ""));
    }
    void paste(int trackIdx, int at) {
        if (clipboard.empty() || trackIdx < 0) return;
        session.checkpoint("Paste");
        clearSelection();
        for (auto& cc : clipboard) {
            const int ti = trackIdx + cc.trackOffset;
            if (ti >= (int)proj().tracks.size()) continue;
            auto& t = proj().tracks[(size_t)ti];
            const bool ok = (cc.clip.type == ClipType::Audio && t.type == TrackType::Audio) || (cc.clip.type == ClipType::Midi && t.hasInstrument()) || (cc.clip.type == ClipType::Pattern && t.type == TrackType::Drum);
            if (!ok) continue;
            Clip c = cc.clip; c.id = proj().newId("c"); c.start += at; c.selected = true;
            t.clips.push_back(c);
        }
        session.changed(true);
    }
    void deleteSelection() {
        bool any = false;
        for (auto& t : proj().tracks) for (auto& c : t.clips) any |= c.selected;
        if (!any) return;
        session.checkpoint("Delete clips");
        for (auto& t : proj().tracks) t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [](auto& c) { return c.selected; }), t.clips.end());
        session.changed(true);
    }
    void duplicateSelection() {
        int lo = INT_MAX, hi = 0;
        for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.selected) { lo = juce::jmin(lo, c.start); hi = juce::jmax(hi, c.end()); }
        if (lo >= hi) return;
        session.checkpoint("Duplicate clips");
        for (auto& t : proj().tracks) {
            std::vector<Clip> add;
            for (auto& c : t.clips) if (c.selected) { Clip d = c; d.id = proj().newId("c"); d.start += hi - lo; add.push_back(d); c.selected = false; }
            for (auto& d : add) t.clips.push_back(d);
        }
        session.changed(true);
    }
    void nudge(int dir) {
        session.checkpoint("Nudge");
        for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.selected) c.start = juce::jmax(0, c.start + dir * juce::jmax(PPQ / 4, snapTicks(proj(), proj().snap, c.start)));
        session.changed(true);
    }

    // ---------------------------------------------------------------- drag & drop (palette chips)
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("pattern:") || d.description.toString().startsWith("sample:"); }
    void itemDragMove(const SourceDetails& d) override {
        const Row* r = rowAt((float)d.localPosition.y);
        if (r && r->lane < 0 && d.localPosition.x > headerW()) {
            const int t = snap(xToTick((float)d.localPosition.x), false);
            dropHighlight = juce::Rectangle<float>(tickToX(t), r->y + 3.f, (float)(PPQ * 4 * pxPerTick), r->h - 6.f);
        } else dropHighlight.reset();
        repaint();
    }
    void itemDragExit(const SourceDetails&) override { dropHighlight.reset(); repaint(); }
    void itemDropped(const SourceDetails& d) override {
        dropHighlight.reset();
        layoutRows();
        const Row* r = rowAt((float)d.localPosition.y);
        const auto desc = d.description.toString();
        const int at = snap(xToTick((float)d.localPosition.x), false);
        if (desc.startsWith("pattern:")) {
            const auto pid = desc.fromFirstOccurrenceOf("pattern:", false, false);
            const Pattern* pat = proj().findPattern(pid);
            if (!pat) return;
            Track* target = r && r->lane < 0 ? &proj().tracks[(size_t)r->track] : nullptr;
            session.checkpoint("Add pattern");
            if (!target || target->type != TrackType::Drum) { ScreenUtil::addTrack(session, TrackType::Drum); target = session.track(); }
            Clip c; c.id = proj().newId("c"); c.type = ClipType::Pattern; c.patternId = pid; c.name = pat->name; c.start = at; c.length = juce::jmax(pat->lengthTicks(), proj().ticksPerBarAt(at));
            target->clips.push_back(c);
            session.changed(true);
        } else if (desc.startsWith("sample:")) {
            const auto ref = desc.fromFirstOccurrenceOf("sample:", false, false);
            auto smp = SampleBank::get().loadNow(ref);
            if (!smp) { ctx().toast("Couldn't load that sample"); return; }
            Track* target = r && r->lane < 0 ? &proj().tracks[(size_t)r->track] : nullptr;
            session.checkpoint("Add sample");
            if (!target || target->type != TrackType::Audio) { ScreenUtil::addTrack(session, TrackType::Audio); target = session.track(); }
            Clip c; c.id = proj().newId("c"); c.type = ClipType::Audio; c.audio.file = ref; c.name = smp->name; c.start = at;
            c.length = juce::jmax(PPQ / 4, (int)std::ceil(proj().secondsToTicks(proj().ticksToSeconds(at) + smp->seconds())) - at);
            target->clips.push_back(c);
            session.changed(true);
            AppSettings::get().addRecentSample(ref);
        }
        repaint();
    }

    void timerCallback() override {
        if (ctx().engine->isPlaying() && AppSettings::get().followPlayhead && !dragging && !pinch.active()) {
            const float px = tickToX(ctx().engine->playheadTick());
            if (px > getWidth() - 40 || px < headerW()) scrollTick = std::max(0.0, ctx().engine->playheadTick() - 2.0 * PPQ);
        }
        const double ph = ctx().engine->playheadTick();
        if (ph != lastPlayhead || ctx().engine->isPlaying()) { lastPlayhead = ph; repaint(); }   // no idle redraws (battery)
    }
    double lastPlayhead = -1.0;

    Session& session;
    Hit downHit;
    juce::Point<float> downPos;
    double downScrollTick = 0.0;
    float downScrollY = 0.f;
    bool dragging = false;
    struct Orig { int start, length, contentOffset; double offsetSec; };
    std::map<juce::String, Orig> dragOrig;
    AutomationLane* dragLane = nullptr;
    int dragPoint = -1;
    std::pair<int, int> loopOrig;
    std::optional<juce::Rectangle<float>> rubber, dropHighlight;
    LongPress lp;
    Pinch pinch;
    double pinchStartPx = 0.04, pinchStartScroll = 0.0;
    float pinchStartV = 1.f;
    juce::String lastTapClip;
    juce::uint32 lastTapTime = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    friend class ArrangeScreen;
};

// ------------------------------------------------------------------ palette strip (drag sources)
class Palette : public juce::Component {
public:
    explicit Palette(Session& s) : session(s) {}
    void refresh() {
        chips.clear();
        removeAllChildren();
        for (auto& p : session.project.patterns) addChip("pattern:" + p.id, p.name, p.colour);
        for (auto& r : AppSettings::get().recentSamples) {
            if (chips.size() > 24) break;
            const auto f = SampleBank::get().resolve(r);
            if (f.existsAsFile() || r.startsWith("builtin:")) addChip("sample:" + r, f.getFileNameWithoutExtension(), pal().dim);
        }
        resized();
    }
    void addChip(const juce::String& desc, const juce::String& name, juce::Colour c) {
        struct DragChip : juce::Component, juce::SettableTooltipClient {
            juce::String desc, name; juce::Colour colour;
            void paint(juce::Graphics& g) override {
                g.setColour(colour.withAlpha(0.3f)); g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.f), 14.f);
                g.setColour(pal().text); g.setFont(font(13.f, true));
                drawIcon(g, desc.startsWith("pattern") ? "drums" : "samples", getLocalBounds().removeFromLeft(30).toFloat().withSizeKeepingCentre(14.f, 14.f), colour.brighter(0.3f));
                g.drawFittedText(name, getLocalBounds().withTrimmedLeft(28).withTrimmedRight(8), juce::Justification::centredLeft, 1);
            }
            void mouseDrag(const juce::MouseEvent& e) override {
                if (e.getDistanceFromDragStart() < 8) return;
                if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this)) if (!dnd->isDragAndDropActive()) dnd->startDragging(desc, this);
            }
        };
        auto* ch = new DragChip();
        ch->desc = desc; ch->name = name; ch->colour = c;
        ch->setTooltip("Drag onto a track");
        ch->setTitle(name);
        chips.add(ch);
        addAndMakeVisible(ch);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(pal().surface);
        g.setColour(pal().line); g.fillRect(0, 0, getWidth(), 1);
        if (chips.isEmpty()) { g.setColour(pal().dim); g.setFont(font(13.f)); g.drawText("Drum patterns and recent samples appear here. Drag them onto tracks.", getLocalBounds().reduced(12, 0), juce::Justification::centredLeft); }
    }
    void resized() override {
        int x = 8;
        for (auto* c : chips) { const int w = juce::jlimit(90, 170, juce::GlyphArrangement::getStringWidthInt(font(13.f, true), static_cast<juce::Component*>(c)->getTitle()) + 120); c->setBounds(x, 6, w, getHeight() - 12); x += w + 4; }
    }
    Session& session;
    juce::OwnedArray<juce::Component> chips;
};

// ------------------------------------------------------------------ screen
class ArrangeScreen : public Screen, public juce::DragAndDropContainer {
public:
    ArrangeScreen() : timeline(session()), palette(session()) {
        addAndMakeVisible(timeline);
        addAndMakeVisible(palette);
        for (auto* b : { &addB, &drawB, &zoomIn, &zoomOut, &markerB, &snapB, &moreB }) addAndMakeVisible(b);
        addB.onClick = [this] { ScreenUtil::addTrackMenu(session(), &addB); };
        drawB.setClickingTogglesState(true);
        drawB.onClick = [this] { timeline.drawMode = drawB.getToggleState(); ctx().toast(timeline.drawMode ? "Draw: tap a lane to create a clip, drag to set its length" : "Select: drag clips to move, drag empty space to scroll"); };
        zoomIn.onClick = [this] { zoom(1.4); };
        zoomOut.onClick = [this] { zoom(1.0 / 1.4); };
        markerB.onClick = [this] { timeline.addMarker(timeline.snap(engine().playheadTick())); };
        snapB.onClick = [this] {
            juce::PopupMenu m;
            for (int i = 0; i < (int)snapTable().size(); ++i) m.addItem(i + 1, snapTable()[(size_t)i].name, true, proj().snap == i);
            showMenu(m, &snapB, [this](int r) { if (r > 0) { proj().snap = r - 1; session().changed(); timeline.repaint(); } });
        };
        moreB.onClick = [this] { more(); };
        timeline.openAudioClip = [this](const juce::String& id) {
            sheet = std::make_unique<AudioClipSheet>(session(), id);
            sheet->onClose = [this] { juce::MessageManager::callAsync([this] { sheet.reset(); }); };
            addAndMakeVisible(*sheet);
            sheet->setBounds(getLocalBounds());
        };
    }
    juce::String title() const override { return "Song"; }
    void refresh() override { palette.refresh(); timeline.repaint(); }
    void paint(juce::Graphics& g) override { g.fillAll(pal().surface); }
    void resized() override {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop(52).reduced(6, 4);
        addB.setBounds(bar.removeFromLeft(bar.getWidth() > 500 ? 120 : touch));
        addB.setCaption(bar.getWidth() > 380 ? "Track" : "");
        bar.removeFromLeft(4);
        drawB.setBounds(bar.removeFromLeft(touch));
        snapB.setBounds(bar.removeFromLeft(touch));
        moreB.setBounds(bar.removeFromRight(touch));
        zoomIn.setBounds(bar.removeFromRight(touch));
        zoomOut.setBounds(bar.removeFromRight(touch));
        markerB.setBounds(bar.removeFromRight(touch));
        palette.setBounds(r.removeFromBottom(48));
        timeline.setBounds(r);
        if (sheet) sheet->setBounds(getLocalBounds());
    }
    void zoom(double f) {
        const double centre = timeline.xToTick(timeline.headerW() + (timeline.getWidth() - timeline.headerW()) * 0.5f);
        timeline.pxPerTick = juce::jlimit(0.002, 0.6, timeline.pxPerTick * f);
        timeline.scrollTick = std::max(0.0, centre - (timeline.getWidth() - timeline.headerW()) * 0.5 / timeline.pxPerTick);
        timeline.repaint();
    }
    void more() {
        juce::PopupMenu m;
        m.addItem(1, "Copy selected clips");
        m.addItem(2, "Paste at playhead", !timeline.clipboard.empty());
        m.addItem(3, "Duplicate selected clips");
        m.addItem(4, "Delete selected clips");
        m.addItem(5, "Loop the selected clips");
        m.addSeparator();
        m.addItem(6, "Zoom to fit song");
        m.addItem(7, "Taller tracks", true, timeline.vZoom > 1.2f);
        m.addItem(8, "Follow playhead", true, AppSettings::get().followPlayhead);
        m.addSeparator();
        m.addItem(9, "Import MIDI file...");
        m.addItem(10, "Export song as MIDI file...");
        showMenu(m, &moreB, [this](int r) {
            if (r == 1) timeline.copySelection();
            else if (r == 2) timeline.paste(juce::jmax(0, proj().trackIndex(session().selectedTrack)), timeline.snap(engine().playheadTick()));
            else if (r == 3) timeline.duplicateSelection();
            else if (r == 4) timeline.deleteSelection();
            else if (r == 5) timeline.loopSelection();
            else if (r == 6) { const int end = juce::jmax(proj().songEndTick(), PPQ * 16); timeline.pxPerTick = (timeline.getWidth() - timeline.headerW() - 30) / (double)end; timeline.scrollTick = 0; timeline.repaint(); }
            else if (r == 7) { timeline.vZoom = timeline.vZoom > 1.2f ? 1.f : 1.6f; timeline.repaint(); }
            else if (r == 8) { AppSettings::get().followPlayhead = !AppSettings::get().followPlayhead; AppSettings::get().save(); }
            else if (r == 9) importMidi();
            else if (r == 10) exportMidi();
        });
    }
    void importMidi() {
        chooser = std::make_unique<juce::FileChooser>("Import MIDI file", Paths::root(), "*.mid;*.midi");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            session().checkpoint("Import MIDI");
            juce::String err; int added = 0;
            if (MidiIO::importFile(proj(), f, err, added)) { session().changed(true); ctx().toast("Imported " + juce::String(added) + " track" + (added == 1 ? "" : "s")); }
            else { session().undo(); ctx().toast(err); }
        });
    }
    void exportMidi() {
        chooser = std::make_unique<juce::FileChooser>("Export MIDI file", Paths::exports().getChildFile(juce::File::createLegalFileName(proj().name) + ".mid"), "*.mid");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            juce::String err;
            if (MidiIO::exportFile(proj(), f.withFileExtension("mid"), err)) ctx().toast("Exported " + f.withFileExtension("mid").getFileName()); else ctx().toast(err);
        });
    }
    bool handleKey(const juce::KeyPress& k) override {
        const bool cmd = k.getModifiers().isCommandDown();
        const int c = k.getKeyCode();
        if (c == juce::KeyPress::deleteKey || c == juce::KeyPress::backspaceKey) { timeline.deleteSelection(); return true; }
        if (cmd && (c == 'C' || c == 'c')) { timeline.copySelection(); return true; }
        if (cmd && (c == 'V' || c == 'v')) { timeline.paste(juce::jmax(0, proj().trackIndex(session().selectedTrack)), timeline.snap(engine().playheadTick())); return true; }
        if (cmd && (c == 'D' || c == 'd')) { timeline.duplicateSelection(); return true; }
        if (cmd && (c == 'A' || c == 'a')) { for (auto& t : proj().tracks) for (auto& cl : t.clips) cl.selected = true; timeline.repaint(); return true; }
        if (!cmd && (c == 'S' || c == 's')) {
            for (int ti = 0; ti < (int)proj().tracks.size(); ++ti)
                for (auto& cl : proj().tracks[(size_t)ti].clips)
                    if (cl.selected && engine().playheadTick() > cl.start && engine().playheadTick() < cl.end()) { timeline.clipAction(ti, cl.id, 4); return true; }
            return true;
        }
        if (!cmd && (c == 'B' || c == 'b')) { drawB.triggerClick(); return true; }
        if (c == juce::KeyPress::leftKey) { timeline.nudge(-1); return true; }
        if (c == juce::KeyPress::rightKey) { timeline.nudge(1); return true; }
        if (c == '=' || c == '+') { zoom(1.4); return true; }
        if (c == '-') { zoom(1.0 / 1.4); return true; }
        return false;
    }
private:
    Timeline timeline;
    Palette palette;
    IconButton addB { "plus", "Track", "Add a track" }, drawB { "pencil", {}, "Draw clips (B)" }, zoomIn { "zoomin", {}, "Zoom in (+)" }, zoomOut { "zoomout", {}, "Zoom out (-)" },
        markerB { "flag", {}, "Add marker at playhead" }, snapB { "magnet", {}, "Snap to grid" }, moreB { "more", {}, "More" };
    std::unique_ptr<AudioClipSheet> sheet;
    std::unique_ptr<juce::FileChooser> chooser;
};

}  // namespace

std::unique_ptr<Screen> makeArrangeScreen() { return std::make_unique<ArrangeScreen>(); }
