#include "Screens.h"
#include "Core/MidiIO.h"
#include "Core/Paths.h"
#include "Core/Theory.h"
#include <climits>

using namespace UI;

namespace {

class Roll : public juce::Component, private juce::Timer {
public:
    explicit Roll(Session& s) : session(s) {
        lp.fire = [this](juce::Point<float> p) { longPress(p); };
        startTimerHz(30);
        setTitle("Piano roll grid");
    }
    // view
    double pxPerTick = 0.12;
    double scrollTick = 0.0;
    float rowH = 18.f, scrollY = -1.f;
    bool drawMode = true, scaleGuide = true, snapToScale = false, velocityLane = true;
    int chordType = -1;          // -1 off, else chord index; 1000+degree = diatonic
    int lastLength = PPQ / 2;
    static constexpr int keysW = 58, rulerH = 30;
    int velH() const { return velocityLane ? juce::jlimit(70, 120, getHeight() / 6) : 0; }

    Clip* clip(Track** owner = nullptr) { return session.editClip(owner); }
    Project& proj() { return session.project; }
    float tickToX(double t) const { return (float)(keysW + (t - scrollTick) * pxPerTick); }
    double xToTick(float x) const { return scrollTick + (x - keysW) / pxPerTick; }
    float pitchToY(int p) const { return rulerH + (127 - p) * rowH - scrollY; }
    int yToPitch(float y) const { return juce::jlimit(0, 127, 127 - (int)std::floor((y - rulerH + scrollY) / rowH)); }
    int gridH() const { return getHeight() - rulerH - velH(); }
    int snapT(double t, bool round = true) {
        auto* c = clip();
        const int base = c ? c->start : 0;
        return snapTick(proj(), (int)std::max(0.0, t) + base, proj().snap == 0 ? 0 : juce::jmax(5, proj().snap), round) - base;   // bar snaps are too coarse here: min 1/16
    }
    int snapLen() { auto* c = clip(); return juce::jmax(PPQ / 16, snapTicks(proj(), juce::jmax(5, proj().snap), c ? c->start : 0)); }

    void centreOnNotes() {
        auto* c = clip();
        int lo = 60, hi = 72;
        if (c && !c->notes.empty()) { lo = 127; hi = 0; for (auto& n : c->notes) { lo = juce::jmin(lo, n.pitch); hi = juce::jmax(hi, n.pitch); } }
        scrollY = juce::jlimit(0.f, juce::jmax(0.f, 128 * rowH - gridH()), (127 - (lo + hi) / 2) * rowH - gridH() * 0.5f);
        scrollTick = 0.0;
        if (c && getWidth() > keysW) pxPerTick = juce::jlimit(0.02, 1.0, (getWidth() - keysW - 20) / (double)juce::jmax(PPQ * 4, c->contentLength));
    }

    // ---------------------------------------------------------------- paint
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        g.fillAll(p.bg);
        if (scrollY < 0.f) centreOnNotes();
        Track* owner = nullptr;
        auto* c = clip(&owner);
        auto grid = juce::Rectangle<int>(keysW, rulerH, getWidth() - keysW, gridH());
        const bool drums = owner && owner->type == TrackType::Drum;
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(grid);
            // rows
            for (int pitch = yToPitch((float)grid.getBottom()); pitch <= yToPitch((float)grid.getY()); ++pitch) {
                const float y = pitchToY(pitch);
                const bool black = juce::MidiMessage::isMidiNoteBlack(pitch);
                juce::Colour row = black ? p.bg : p.surface;
                if (scaleGuide && !drums) {
                    if (!Theory::inScale(proj(), pitch)) row = p.bg.darker(0.25f);
                    else if ((pitch - proj().key) % 12 == 0) row = p.accent.withAlpha(0.10f).overlaidWith(p.surface.withAlpha(0.f));
                }
                g.setColour(row);
                g.fillRect((float)grid.getX(), y, (float)grid.getWidth(), rowH);
                g.setColour(p.line.withAlpha(pitch % 12 == 0 ? 0.9f : 0.25f));
                g.fillRect((float)grid.getX(), y + rowH - 1.f, (float)grid.getWidth(), 1.f);
            }
            if (c) {
                drawBeats(g, grid, *c);
                // area beyond the loop/content end
                const float endX = tickToX(c->contentLength);
                g.setColour(juce::Colours::black.withAlpha(0.35f));
                g.fillRect(juce::Rectangle<float>(endX, (float)grid.getY(), (float)getWidth(), (float)grid.getHeight()));
                const float offX = tickToX(c->contentOffset);
                if (c->contentOffset > 0) g.fillRect(juce::Rectangle<float>((float)keysW, (float)grid.getY(), offX - keysW, (float)grid.getHeight()));
                for (auto& n : c->notes) drawNote(g, n, owner ? owner->colour : p.accent);
                // playhead (relative to clip)
                const double ph = ctx().engine->playheadTick();
                if (proj().previewTrack.isNotEmpty() || (ph >= c->start && ph < c->end())) {
                    const int loopLen = juce::jmax(1, c->contentLength - c->contentOffset);
                    const double rel = proj().previewTrack.isNotEmpty() ? std::fmod(ph, (double)loopLen) + c->contentOffset : c->contentOffset + std::fmod(ph - c->start, (double)loopLen);
                    g.setColour(p.accent);
                    g.fillRect(tickToX(rel) - 1.f, (float)grid.getY(), 2.f, (float)grid.getHeight());
                }
            }
            if (rubber.has_value()) { g.setColour(p.accent.withAlpha(0.15f)); g.fillRect(*rubber); g.setColour(p.accent); g.drawRect(*rubber, 1.f); }
        }
        drawKeys(g, grid, drums ? owner : nullptr);
        drawRuler(g, c);
        if (velocityLane) drawVelocity(g, c, owner ? owner->colour : p.accent);
        if (!c) {
            g.setColour(p.dim);
            g.setFont(font(16.f));
            g.drawFittedText(owner && owner->hasInstrument() ? "No clip selected. Tap \"New clip\" or double-tap a clip in the Song view." : "Select an instrument or drum track to write notes.",
                             grid.reduced(40), juce::Justification::centred, 3);
        }
    }

    void drawBeats(juce::Graphics& g, juce::Rectangle<int> grid, const Clip& c) {
        const auto& p = pal();
        const int beat = PPQ * 4 / proj().timeSigAtBar(proj().tickToBar(c.start)).den;
        const int bar = proj().ticksPerBarAt(c.start);
        const int step = beat / 4 * pxPerTick > 10.0 ? beat / 4 : beat;
        const double t0 = juce::jmax(0.0, xToTick((float)grid.getX())), t1 = xToTick((float)grid.getRight());
        for (int t = (int)(t0 / step) * step; t < t1; t += step) {
            const bool isBar = t % bar == 0, isBeat = t % beat == 0;
            g.setColour(p.line.withAlpha(isBar ? 1.f : (isBeat ? 0.5f : 0.2f)));
            g.fillRect(tickToX(t), (float)grid.getY(), isBar ? 1.5f : 1.f, (float)grid.getHeight());
        }
    }

    void drawNote(juce::Graphics& g, const Note& n, juce::Colour base) {
        const auto& p = pal();
        auto r = juce::Rectangle<float>(tickToX(n.start), pitchToY(n.pitch) + 1.f, juce::jmax(4.f, (float)(n.length * pxPerTick)), rowH - 2.f);
        const auto c = base.withMultipliedSaturation(0.4f + n.velocity * 0.6f).withMultipliedBrightness(0.6f + n.velocity * 0.4f);
        g.setColour(c);
        g.fillRoundedRectangle(r, 3.f);
        if (n.selected) { g.setColour(p.text); g.drawRoundedRectangle(r.reduced(0.5f), 3.f, 2.f); }
        if (r.getWidth() > 26.f && rowH >= 14.f) {
            g.setColour(c.getPerceivedBrightness() > 0.6f ? juce::Colour(0xff17181b) : juce::Colours::white);
            g.setFont(font(10.5f, true));
            g.drawText(Theory::noteName(n.pitch), r.reduced(4.f, 0.f).toNearestInt(), juce::Justification::centredLeft, false);
        }
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.fillRect(r.getRight() - 4.f, r.getY() + 3.f, 2.f, r.getHeight() - 6.f);
    }

    void drawKeys(juce::Graphics& g, juce::Rectangle<int> grid, const Track* drumTrack) {
        const auto& p = pal();
        g.setColour(p.surface);
        g.fillRect(0, grid.getY(), keysW, grid.getHeight());
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(0, grid.getY(), keysW, grid.getHeight());
        for (int pitch = yToPitch((float)grid.getBottom()); pitch <= yToPitch((float)grid.getY()); ++pitch) {
            const float y = pitchToY(pitch);
            const bool black = juce::MidiMessage::isMidiNoteBlack(pitch);
            g.setColour(pitch == auditioning ? p.accent : (black ? juce::Colour(0xff0e0f11) : juce::Colour(0xffe9e6e0)));
            g.fillRect(0.f, y, black ? keysW * 0.62f : (float)keysW - 1.f, rowH - 1.f);
            if (drumTrack && pitch >= 36 && pitch < 52) {
                g.setColour(black ? juce::Colours::white : juce::Colour(0xff17181b));
                g.setFont(font(9.5f, true));
                g.drawFittedText(drumTrack->inst.pads[(size_t)(pitch - 36)].name, juce::Rectangle<int>(2, (int)y, keysW - 4, (int)rowH), juce::Justification::centredLeft, 1);
            } else if (pitch % 12 == 0 && rowH >= 12.f) {
                g.setColour(juce::Colour(0xff6b6862));
                g.setFont(font(10.5f, true));
                g.drawText(Theory::noteName(pitch), juce::Rectangle<float>(0.f, y, (float)keysW - 6.f, rowH).toNearestInt(), juce::Justification::centredRight);
            }
        }
    }

    void drawRuler(juce::Graphics& g, Clip* c) {
        const auto& p = pal();
        g.setColour(p.surface);
        g.fillRect(0, 0, getWidth(), rulerH);
        g.setColour(p.line);
        g.fillRect(0, rulerH - 1, getWidth(), 1);
        if (!c) return;
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(keysW, 0, getWidth() - keysW, rulerH);
        const int bar = proj().ticksPerBarAt(c->start);
        g.setFont(font(11.5f, true));
        for (int t = 0; t < xToTick((float)getWidth()); t += bar) {
            const float x = tickToX(t);
            g.setColour(p.dim);
            g.fillRect(x, 14.f, 1.f, (float)rulerH - 14.f);
            g.setColour(p.text.withAlpha(0.8f));
            g.drawText(juce::String(proj().tickToBar(c->start + t) + 1), (int)x + 4, 12, 40, 16, juce::Justification::centredLeft);
        }
        // loop end handle
        const float ex = tickToX(c->contentLength);
        g.setColour(p.accent);
        g.fillRoundedRectangle(ex - 6.f, 2.f, 12.f, (float)rulerH - 4.f, 4.f);
        g.setColour(p.accentText);
        g.fillRect(ex - 1.f, 7.f, 2.f, (float)rulerH - 14.f);
    }

    void drawVelocity(juce::Graphics& g, Clip* c, juce::Colour base) {
        const auto& p = pal();
        auto r = juce::Rectangle<int>(0, getHeight() - velH(), getWidth(), velH());
        g.setColour(p.surface);
        g.fillRect(r);
        g.setColour(p.line);
        g.fillRect(r.getX(), r.getY(), r.getWidth(), 1);
        g.setColour(p.dim);
        g.setFont(font(11.f));
        g.drawFittedText("Velocity", r.withWidth(keysW).reduced(4), juce::Justification::centred, 2);
        if (!c) return;
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(r.withTrimmedLeft(keysW));
        auto area = r.withTrimmedLeft(keysW).reduced(0, 8).toFloat();
        for (auto& n : c->notes) {
            const float x = tickToX(n.start), h = area.getHeight() * n.velocity;
            g.setColour(n.selected ? p.text : base.withAlpha(0.85f));
            g.fillRect(x, area.getBottom() - h, 4.f, h);
            g.fillEllipse(x - 3.f, area.getBottom() - h - 5.f, 10.f, 10.f);
        }
    }

    // ---------------------------------------------------------------- interaction
    enum class Mode { None, Draw, Move, Resize, Rubber, Pan, Velocity, LoopEnd, Keys };
    int noteAt(juce::Point<float> pt, bool& edge) {
        auto* c = clip();
        edge = false;
        if (!c) return -1;
        for (int i = (int)c->notes.size() - 1; i >= 0; --i) {
            const auto& n = c->notes[(size_t)i];
            auto r = juce::Rectangle<float>(tickToX(n.start), pitchToY(n.pitch), juce::jmax(4.f, (float)(n.length * pxPerTick)), rowH);
            if (r.expanded(2.f, rowH < 16.f ? (16.f - rowH) * 0.5f : 0.f).contains(pt)) { edge = pt.x > r.getRight() - juce::jlimit(6.f, 16.f, r.getWidth() * 0.3f); return i; }
        }
        return -1;
    }

    void audition(int pitch, float vel = 0.8f) {
        Track* owner = nullptr;
        clip(&owner);
        if (!owner) owner = session.track();
        if (!owner || !owner->hasInstrument()) return;
        if (auditioning >= 0) ctx().engine->liveNoteOff(owner->id, auditioning);
        auditioning = pitch;
        auditionOff = juce::Time::getMillisecondCounter() + 250;
        ctx().engine->liveNoteOn(owner->id, pitch, vel);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        pinch.down(e);
        if (pinch.active()) { mode = Mode::Pan; lp.cancel(); pinchStart = { pxPerTick, rowH, scrollTick, scrollY }; return; }
        auto* c = clip();
        downPos = e.position;
        mode = Mode::None;
        moved = false;
        if (e.mods.isPopupMenu()) { longPress(e.position); return; }
        lp.down(e.position);
        if (!c) return;
        if (e.position.y < rulerH) { if (std::abs(e.position.x - tickToX(c->contentLength)) < 18.f) { mode = Mode::LoopEnd; session.checkpoint("Clip loop length"); } else mode = Mode::Pan; downScroll = { scrollTick, scrollY }; return; }
        if (velocityLane && e.position.y > getHeight() - velH()) { mode = Mode::Velocity; session.checkpoint("Velocity"); setVelocityAt(e.position); return; }
        if (e.position.x < keysW) { mode = Mode::Keys; audition(yToPitch(e.position.y)); downScroll = { scrollTick, scrollY }; return; }
        bool edge = false;
        const int idx = noteAt(e.position, edge);
        if (idx >= 0) {
            auto& n = c->notes[(size_t)idx];
            if (!n.selected && !e.mods.isShiftDown()) for (auto& o : c->notes) o.selected = false;
            n.selected = true;
            grabbed = idx;
            orig = c->notes;
            mode = edge ? Mode::Resize : Mode::Move;
            audition(n.pitch, n.velocity);
            repaint();
            return;
        }
        if (drawMode && !e.mods.isShiftDown()) {
            session.checkpoint("Draw note");
            for (auto& o : c->notes) o.selected = false;
            const int start = snapT(xToTick(e.position.x), false);
            int pitch = yToPitch(e.position.y);
            if (snapToScale) pitch = toScale(pitch);
            std::vector<int> pitches { pitch };
            if (chordType >= 1000) pitches = Theory::diatonicChord(proj(), degreeOf(pitch), pitch / 12 - 1, chordType == 1001);
            else if (chordType >= 0) pitches = Theory::chordPitches(pitch, chordType);
            for (int pp : pitches) c->notes.push_back({ start, lastLength, pp, 0.8f, true });
            grabbed = (int)c->notes.size() - 1;
            orig = c->notes;
            mode = Mode::Draw;
            audition(pitch);
            session.changed();
            return;
        }
        mode = Mode::Rubber;
        downScroll = { scrollTick, scrollY };
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        float sx, sy; juce::Point<float> ctr;
        if (pinch.drag(e, sx, sy, ctr)) {
            if (!pinchCentre.has_value()) pinchCentre = ctr;
            const double anchorT = pinchStart.scrollTick + (ctr.x - keysW) / pinchStart.px;
            pxPerTick = juce::jlimit(0.02, 1.2, pinchStart.px * sx);
            rowH = juce::jlimit(9.f, 40.f, pinchStart.rowH * sy);
            scrollTick = std::max(0.0, anchorT - (ctr.x - keysW) / pxPerTick - (ctr.x - pinchCentre->x) / pxPerTick);
            scrollY = juce::jlimit(0.f, juce::jmax(0.f, 128 * rowH - gridH()), (pinchStart.scrollY + ctr.y - rulerH) * rowH / pinchStart.rowH - (ctr.y - rulerH) - (ctr.y - pinchCentre->y));
            repaint();
            return;
        }
        lp.moved(e.position);
        auto* c = clip();
        if (!c) return;
        const auto d = e.position - downPos;
        if (!moved && d.getDistanceFromOrigin() < 5.f) return;
        if (!moved && (mode == Mode::Move || mode == Mode::Resize)) session.checkpoint(mode == Mode::Move ? "Move notes" : "Resize notes");
        moved = true;
        lp.cancel();
        switch (mode) {
            case Mode::Move: {
                const auto& g0 = orig[(size_t)grabbed];
                const int dt = snapT(g0.start + d.x / pxPerTick) - g0.start;
                int dp = (int)std::round(-d.y / rowH);
                for (size_t i = 0; i < c->notes.size() && i < orig.size(); ++i) {
                    if (!c->notes[i].selected) continue;
                    c->notes[i].start = juce::jmax(0, orig[i].start + dt);
                    int np = juce::jlimit(0, 127, orig[i].pitch + dp);
                    if (snapToScale) np = toScale(np);
                    c->notes[i].pitch = np;
                }
                if (c->notes[(size_t)grabbed].pitch != auditioning) audition(c->notes[(size_t)grabbed].pitch, c->notes[(size_t)grabbed].velocity);
                break;
            }
            case Mode::Resize: case Mode::Draw: {
                const auto& g0 = orig[(size_t)grabbed];
                const int end = snapT(g0.start + g0.length + d.x / pxPerTick);
                const int dl = juce::jmax(snapLen(), end - g0.start) - g0.length;
                for (size_t i = 0; i < c->notes.size() && i < orig.size(); ++i)
                    if (c->notes[i].selected) c->notes[i].length = juce::jmax(PPQ / 32, orig[i].length + dl);
                lastLength = c->notes[(size_t)grabbed].length;
                break;
            }
            case Mode::Rubber: rubber = juce::Rectangle<float>(downPos, e.position); break;
            case Mode::Pan: case Mode::Keys:
                scrollTick = std::max(0.0, downScroll.first - (mode == Mode::Keys ? 0.0 : d.x / pxPerTick));
                scrollY = juce::jlimit(0.f, juce::jmax(0.f, 128 * rowH - gridH()), downScroll.second - d.y);
                break;
            case Mode::Velocity: setVelocityAt(e.position); break;
            case Mode::LoopEnd: {
                const int bar = proj().ticksPerBarAt(c->start);
                const int beat = PPQ * 4 / proj().timeSigAtBar(proj().tickToBar(c->start)).den;
                int len = (int)std::round((xToTick(e.position.x)) / beat) * beat;
                len = juce::jmax(beat, len);
                c->contentLength = len;
                if (c->length < len || c->length % juce::jmax(1, bar) == 0) c->length = juce::jmax(c->length, len);
                break;
            }
            default: break;
        }
        session.engine.markDirty();
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override {
        const bool wasPinch = pinch.active();
        pinch.up(e);
        if (!pinch.active()) pinchCentre.reset();
        lp.up();
        if (wasPinch || lp.didFire()) { mode = Mode::None; return; }
        auto* c = clip();
        if (c && mode == Mode::Rubber) {
            if (rubber.has_value()) {
                for (auto& n : c->notes) {
                    auto r = juce::Rectangle<float>(tickToX(n.start), pitchToY(n.pitch), (float)(n.length * pxPerTick), rowH);
                    n.selected = r.intersects(*rubber) || (e.mods.isShiftDown() && n.selected);
                }
                rubber.reset();
            } else if (!moved) {
                for (auto& n : c->notes) n.selected = false;
            }
        }
        if (c && !moved && (mode == Mode::Move || mode == Mode::Resize)) {
            // tap on a note: second quick tap deletes (draw mode), like most mobile DAWs
            const auto now = juce::Time::getMillisecondCounter();
            if (drawMode && grabbed == lastTapNote && now - lastTapTime < 400) {
                session.checkpoint("Delete note");
                c->notes.erase(c->notes.begin() + grabbed);
                lastTapNote = -1;
                session.changed();
            } else { lastTapNote = grabbed; lastTapTime = now; }
        }
        if (moved && (mode == Mode::Move || mode == Mode::Resize || mode == Mode::Draw || mode == Mode::Velocity || mode == Mode::LoopEnd)) session.changed();
        if (mode == Mode::Velocity && !moved) session.changed();
        mode = Mode::None;
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
            const double anchor = xToTick(e.position.x);
            pxPerTick = juce::jlimit(0.02, 1.2, pxPerTick * (w.deltaY > 0 ? 1.15 : 1.0 / 1.15));
            scrollTick = std::max(0.0, anchor - (e.position.x - keysW) / pxPerTick);
        } else if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY)) {
            scrollTick = std::max(0.0, scrollTick - (w.deltaX != 0.f ? w.deltaX : w.deltaY) * 300.0 / pxPerTick * 0.3);
        } else scrollY = juce::jlimit(0.f, juce::jmax(0.f, 128 * rowH - gridH()), scrollY - w.deltaY * 240.f);
        repaint();
    }

    void setVelocityAt(juce::Point<float> pt) {
        auto* c = clip();
        if (!c) return;
        auto area = juce::Rectangle<float>((float)keysW, (float)(getHeight() - velH() + 8), (float)(getWidth() - keysW), (float)(velH() - 16));
        const float v = juce::jlimit(0.02f, 1.f, (area.getBottom() - pt.y) / area.getHeight());
        bool anySel = false;
        for (auto& n : c->notes) anySel |= n.selected;
        // nearest note under the finger horizontally (or every selected note)
        int best = -1; float bestD = 14.f;
        for (int i = 0; i < (int)c->notes.size(); ++i) { const float dx = std::abs(tickToX(c->notes[(size_t)i].start) - pt.x); if (dx < bestD) { bestD = dx; best = i; } }
        if (best < 0) return;
        if (anySel && c->notes[(size_t)best].selected) { for (auto& n : c->notes) if (n.selected) n.velocity = v; }
        else c->notes[(size_t)best].velocity = v;
        repaint();
    }

    int toScale(int pitch) {
        for (int d = 0; d < 12; ++d) {
            if (Theory::inScale(proj(), pitch - d)) return pitch - d;
            if (Theory::inScale(proj(), pitch + d)) return pitch + d;
        }
        return pitch;
    }
    int degreeOf(int pitch) {
        int deg = 0;
        const int pc = ((pitch - proj().key) % 12 + 12) % 12;
        for (int i = 0; i < pc; ++i) if (Theory::inScale(proj(), proj().key + i)) ++deg;
        return deg;
    }

    void longPress(juce::Point<float> pt) {
        haptic(1);
        auto* c = clip();
        if (!c) return;
        bool edge;
        const int idx = noteAt(pt, edge);
        if (idx >= 0 && !c->notes[(size_t)idx].selected) { for (auto& n : c->notes) n.selected = false; c->notes[(size_t)idx].selected = true; }
        mode = Mode::None;
        juce::PopupMenu m;
        const int sel = numSelected();
        m.addSectionHeader(sel > 0 ? juce::String(sel) + " note" + (sel > 1 ? "s" : "") + " selected" : juce::String("Notes"));
        m.addItem(1, "Delete", sel > 0);
        m.addItem(2, "Duplicate (after selection)", sel > 0);
        m.addItem(3, "Set velocity...", sel > 0);
        m.addItem(4, "Quantize", true);
        m.addItem(5, "Transpose...", true);
        m.addItem(6, "Legato (fill gaps)", sel > 1);
        m.addItem(7, "Select all");
        showMenu(m, this, [this](int r) { action(r); });
    }

    int numSelected() { int n = 0; if (auto* c = clip()) for (auto& x : c->notes) n += x.selected ? 1 : 0; return n; }

    void action(int r) {
        auto* c = clip();
        if (!c) return;
        const bool sel = numSelected() > 0;
        switch (r) {
            case 1: session.checkpoint("Delete notes"); c->notes.erase(std::remove_if(c->notes.begin(), c->notes.end(), [](auto& n) { return n.selected; }), c->notes.end()); break;
            case 2: {
                session.checkpoint("Duplicate notes");
                int lo = INT_MAX, hi = 0;
                for (auto& n : c->notes) if (n.selected) { lo = juce::jmin(lo, n.start); hi = juce::jmax(hi, n.start + n.length); }
                const int span = snapT(hi - lo + snapLen() - 1, false);
                std::vector<Note> add;
                for (auto& n : c->notes) if (n.selected) { Note d = n; d.start += juce::jmax(snapLen(), span); add.push_back(d); n.selected = false; }
                for (auto& d : add) c->notes.push_back(d);
                const int end = add.empty() ? 0 : std::max_element(add.begin(), add.end(), [](auto& a, auto& b) { return a.start + a.length < b.start + b.length; })->start + add.back().length;
                if (end > c->contentLength) { const int bar = proj().ticksPerBarAt(c->start); c->contentLength = ((end + bar - 1) / bar) * bar; c->length = juce::jmax(c->length, c->contentLength); }
                break;
            }
            case 3:
                askText("Velocity", "1 to 127", juce::String(juce::roundToInt(firstSelected().velocity * 127)), "Set", [this](const juce::String& t) {
                    auto* cc = clip(); if (!cc) return;
                    session.checkpoint("Set velocity");
                    for (auto& n : cc->notes) if (n.selected) n.velocity = juce::jlimit(1, 127, t.getIntValue()) / 127.f;
                    session.changed();
                });
                return;
            case 4: session.checkpoint("Quantize"); Theory::quantize(c->notes, snapLen(), 1.f, 0.f, false, sel); break;
            case 5:
                askText("Transpose", "Semitones (negative = down, 12 = one octave)", "12", "Transpose", [this](const juce::String& t) {
                    auto* cc = clip(); if (!cc) return;
                    session.checkpoint("Transpose");
                    Theory::transpose(cc->notes, t.getIntValue(), numSelected() > 0);
                    session.changed();
                });
                return;
            case 6: session.checkpoint("Legato"); Theory::legato(c->notes, true); break;
            case 7: for (auto& n : c->notes) n.selected = true; repaint(); return;
            default: return;
        }
        session.changed();
    }
    Note firstSelected() { if (auto* c = clip()) for (auto& n : c->notes) if (n.selected) return n; return {}; }

    void timerCallback() override {
        if (auditioning >= 0 && juce::Time::getMillisecondCounter() > auditionOff && mode != Mode::Move && mode != Mode::Keys) {
            Track* owner = nullptr; clip(&owner);
            if (!owner) owner = session.track();
            if (owner) ctx().engine->liveNoteOff(owner->id, auditioning);
            auditioning = -1;
        }
        if (ctx().engine->isPlaying()) repaint();
    }

    Session& session;
    Mode mode = Mode::None;
    juce::Point<float> downPos;
    std::pair<double, float> downScroll;
    std::vector<Note> orig;
    int grabbed = -1, auditioning = -1, lastTapNote = -1;
    juce::uint32 auditionOff = 0, lastTapTime = 0;
    bool moved = false;
    std::optional<juce::Rectangle<float>> rubber;
    LongPress lp;
    Pinch pinch;
    struct PinchStart { double px; float rowH; double scrollTick; float scrollY; } pinchStart { 0.1, 18.f, 0.0, 0.f };
    std::optional<juce::Point<float>> pinchCentre;
};

class PianoRollScreen : public Screen {
public:
    PianoRollScreen() : roll(session()) {
        addAndMakeVisible(roll);
        addAndMakeVisible(clipPicker);
        clipPicker.setLabel("Clip");
        clipPicker.onChange = [this](int i) {
            if (i < (int)clipIds.size()) { session().editClipId = clipIds[(size_t)i]; roll.scrollY = -1.f; roll.repaint(); }
            else newClip();
        };
        for (auto* b : { &drawB, &snapB, &chordB, &scaleB, &quantB, &playB, &moreB, &velB }) addAndMakeVisible(b);
        drawB.setClickingTogglesState(true);
        drawB.setToggleState(true, juce::dontSendNotification);
        drawB.onClick = [this] { roll.drawMode = drawB.getToggleState(); };
        velB.setClickingTogglesState(true);
        velB.setToggleState(true, juce::dontSendNotification);
        velB.onClick = [this] { roll.velocityLane = velB.getToggleState(); roll.repaint(); };
        snapB.onClick = [this] {
            juce::PopupMenu m;
            for (int i = 0; i < (int)snapTable().size(); ++i) m.addItem(i + 1, snapTable()[(size_t)i].name, true, proj().snap == i);
            showMenu(m, &snapB, [this](int r) { if (r > 0) { proj().snap = r - 1; session().changed(); } });
        };
        chordB.onClick = [this] { chordMenu(); };
        scaleB.onClick = [this] { scaleMenu(); };
        quantB.onClick = [this] { quantizeMenu(); };
        playB.onClick = [this] { togglePreview(); };
        moreB.onClick = [this] { more(); };
    }
    juce::String title() const override { return "Piano roll"; }
    void onShow() override {
        refresh();
        roll.scrollY = -1.f;
    }
    void onHide() override { if (proj().previewTrack.isNotEmpty()) togglePreview(); }
    void refresh() override {
        Track* owner = nullptr;
        Clip* c = session().editClip(&owner);
        Track* t = session().track();
        if (c && t && owner != t) { owner = t; c = nullptr; }   // selection moved to another track
        if (!c && t) for (auto& cl : t->clips) if (cl.type == ClipType::Midi) { session().editClipId = cl.id; c = &cl; owner = t; roll.scrollY = -1.f; break; }
        if (!t && owner) t = owner;
        if (!c) session().editClipId.clear();   // never show a clip from a different track
        clipIds.clear();
        juce::StringArray names;
        int sel = 0;
        if (t) for (auto& cl : t->clips) if (cl.type == ClipType::Midi) {
            if (c && cl.id == c->id) sel = (int)clipIds.size();
            clipIds.push_back(cl.id);
            names.add(cl.name + " (bar " + juce::String(proj().tickToBar(cl.start) + 1) + ")");
        }
        names.add("New clip...");
        clipPicker.setItems(names, clipIds.empty() ? -1 : sel);
        clipPicker.setLabel(t ? t->name : juce::String("Clip"));
        scaleB.setToggleState(roll.scaleGuide, juce::dontSendNotification);
        chordB.setToggleState(roll.chordType >= 0, juce::dontSendNotification);
        playB.setToggleState(proj().previewTrack.isNotEmpty(), juce::dontSendNotification);
        roll.repaint();
    }
    void paint(juce::Graphics& g) override { g.fillAll(pal().surface); }
    void resized() override {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop(52).reduced(6, 4);
        const bool narrow = bar.getWidth() < 560;
        if (narrow) {
            auto bar2 = r.removeFromTop(48).reduced(6, 0).withTrimmedBottom(4);
            clipPicker.setBounds(bar2);
        }
        playB.setBounds(bar.removeFromLeft(touch));
        drawB.setBounds(bar.removeFromLeft(touch));
        snapB.setBounds(bar.removeFromLeft(touch));
        moreB.setBounds(bar.removeFromRight(touch));
        velB.setBounds(bar.removeFromRight(touch));
        quantB.setBounds(bar.removeFromRight(touch));
        chordB.setBounds(bar.removeFromRight(touch));
        scaleB.setBounds(bar.removeFromRight(touch));
        if (!narrow) clipPicker.setBounds(bar.reduced(6, 0));
        roll.setBounds(r);
    }
    bool handleKey(const juce::KeyPress& k) override {
        const bool cmd = k.getModifiers().isCommandDown();
        const int c = k.getKeyCode();
        auto* clip = session().editClip();
        if (!clip) return false;
        if (c == juce::KeyPress::deleteKey || c == juce::KeyPress::backspaceKey) { roll.action(1); return true; }
        if (cmd && (c == 'A' || c == 'a')) { roll.action(7); return true; }
        if (cmd && (c == 'D' || c == 'd')) { roll.action(2); return true; }
        if (!cmd && (c == 'Q' || c == 'q')) { roll.action(4); return true; }
        if (c == juce::KeyPress::upKey || c == juce::KeyPress::downKey) {
            session().checkpoint("Transpose");
            Theory::transpose(clip->notes, (c == juce::KeyPress::upKey ? 1 : -1) * (k.getModifiers().isShiftDown() ? 12 : 1), true);
            session().changed();
            return true;
        }
        if (c == juce::KeyPress::leftKey || c == juce::KeyPress::rightKey) {
            session().checkpoint("Nudge notes");
            for (auto& n : clip->notes) if (n.selected) n.start = juce::jmax(0, n.start + (c == juce::KeyPress::rightKey ? 1 : -1) * roll.snapLen());
            session().changed();
            return true;
        }
        return false;
    }
private:
    void newClip() {
        Track* t = session().track();
        if (!t || !t->hasInstrument()) { ctx().toast("Select an instrument or drum track first"); refresh(); return; }
        session().checkpoint("New clip");
        const int start = proj().barToTick(proj().tickToBar((int)engine().playheadTick()));
        Clip c = proj().makeMidiClip(start, proj().ticksPerBarAt(start) * 2);
        c.name = t->name;
        t->clips.push_back(c);
        session().editClipId = c.id;
        session().changed(true);
        roll.scrollY = -1.f;
    }
    void togglePreview() {
        auto& p = proj();
        if (p.previewTrack.isNotEmpty()) { p.previewTrack.clear(); p.previewClip.clear(); engine().stop(); engine().rebuild(); engine().seek(0); playB.setToggleState(false, juce::dontSendNotification); return; }
        Track* owner = nullptr;
        auto* c = session().editClip(&owner);
        if (!c || !owner) { ctx().toast("No clip to preview"); return; }
        p.previewTrack = owner->id; p.previewClip = c->id; p.previewPattern.clear();
        engine().rebuild();
        engine().seek(0);
        engine().play();
        playB.setToggleState(true, juce::dontSendNotification);
    }
    void chordMenu() {
        juce::PopupMenu m;
        m.addItem(1, "Single notes", true, roll.chordType < 0);
        m.addItem(2, "Diatonic triads (in key)", true, roll.chordType == 1000);
        m.addItem(3, "Diatonic sevenths (in key)", true, roll.chordType == 1001);
        m.addSeparator();
        for (int i = 0; i < (int)Theory::chords().size(); ++i) m.addItem(100 + i, Theory::chords()[(size_t)i].name, true, roll.chordType == i);
        showMenu(m, &chordB, [this](int r) {
            if (r == 1) roll.chordType = -1; else if (r == 2) roll.chordType = 1000; else if (r == 3) roll.chordType = 1001; else if (r >= 100) roll.chordType = r - 100;
            if (r > 0) ctx().toast(roll.chordType < 0 ? "Drawing single notes" : "Chord tool on: tap the grid to place a chord");
            refresh();
        });
    }
    void scaleMenu() {
        juce::PopupMenu m, keys, scales;
        for (int k = 0; k < 12; ++k) keys.addItem(100 + k, Theory::keyNames()[k], true, proj().key == k);
        for (int s = 0; s < (int)Theory::scales().size(); ++s) scales.addItem(200 + s, Theory::scales()[(size_t)s].name, true, proj().scale == s);
        m.addItem(1, "Highlight scale", true, roll.scaleGuide);
        m.addItem(2, "Snap notes to scale", true, roll.snapToScale);
        m.addSubMenu("Key: " + Theory::keyNames()[proj().key], keys);
        m.addSubMenu("Scale: " + juce::String(Theory::scales()[(size_t)juce::jlimit(0, (int)Theory::scales().size() - 1, proj().scale)].name), scales);
        m.addItem(3, "Custom scale...");
        m.addItem(4, "Detect key from this clip");
        showMenu(m, &scaleB, [this](int r) {
            if (r == 1) roll.scaleGuide = !roll.scaleGuide;
            else if (r == 2) roll.snapToScale = !roll.snapToScale;
            else if (r >= 100 && r < 112) { session().checkpoint("Key"); proj().key = r - 100; session().changed(); }
            else if (r >= 200) { session().checkpoint("Scale"); proj().scale = r - 200; session().changed(); }
            else if (r == 3) askText("Custom scale", "12 characters from the root, 1 = in scale (e.g. 101011010101 is major)", proj().customScale, "Use", [this](const juce::String& t) {
                if (t.length() != 12 || !t.containsOnly("01")) { ctx().toast("Use exactly 12 characters of 0 and 1"); return; }
                session().checkpoint("Custom scale"); proj().customScale = t; proj().scale = (int)Theory::scales().size() - 1; session().changed();
            });
            else if (r == 4) {
                auto* c = session().editClip();
                if (!c || c->notes.size() < 4) { ctx().toast("Need a few more notes to detect the key"); return; }
                auto k = Theory::detectKey(c->notes);
                confirm("Detected " + Theory::keyNames()[k.key] + (k.minor ? " minor" : " major"), "Confidence " + juce::String(juce::roundToInt(k.confidence * 100)) + "%. Set the song key?", "Set key",
                        [this, k] { session().checkpoint("Key"); proj().key = k.key; proj().scale = k.minor ? 1 : 0; session().changed(); });
            }
            refresh();
        });
    }
    void quantizeMenu() {
        juce::PopupMenu m;
        m.addItem(1, "Quantize to grid (100%)");
        m.addItem(2, "Quantize 50% (keeps feel)");
        m.addItem(3, "Quantize starts and ends");
        m.addItem(4, "Quantize with swing 50%");
        m.addSeparator();
        m.addItem(5, "Humanize timing and velocity");
        m.addItem(6, "Velocity: louder (+10%)");
        m.addItem(7, "Velocity: softer (-10%)");
        m.addItem(8, "Velocity: compress toward 80");
        showMenu(m, &quantB, [this](int r) {
            auto* c = session().editClip();
            if (!c || r == 0) return;
            const bool sel = roll.numSelected() > 0;
            session().checkpoint("Edit notes");
            const int grid = roll.snapLen();
            switch (r) {
                case 1: Theory::quantize(c->notes, grid, 1.f, 0.f, false, sel); break;
                case 2: Theory::quantize(c->notes, grid, 0.5f, 0.f, false, sel); break;
                case 3: Theory::quantize(c->notes, grid, 1.f, 0.f, true, sel); break;
                case 4: Theory::quantize(c->notes, grid, 1.f, 0.5f, false, sel); break;
                case 5: Theory::humanize(c->notes, PPQ / 48, 0.08f, sel, (uint32_t)juce::Time::getMillisecondCounter()); break;
                case 6: Theory::scaleVelocity(c->notes, 1.1f, 0.f, sel); break;
                case 7: Theory::scaleVelocity(c->notes, 0.9f, 0.f, sel); break;
                case 8: Theory::scaleVelocity(c->notes, 0.5f, 0.4f, sel); break;
            }
            session().changed();
        });
    }
    void more() {
        juce::PopupMenu m;
        m.addItem(1, "Select all");
        m.addItem(2, "Delete selected");
        m.addItem(3, "Duplicate selected");
        m.addItem(4, "Transpose...");
        m.addItem(5, "Legato");
        m.addSeparator();
        m.addItem(6, "Rename clip...");
        m.addItem(7, "Export clip as MIDI...");
        m.addItem(8, "Import MIDI into this clip...");
        m.addItem(9, "Zoom to fit");
        showMenu(m, &moreB, [this](int r) {
            if (r == 1) roll.action(7); else if (r == 2) roll.action(1); else if (r == 3) roll.action(2); else if (r == 4) roll.action(5); else if (r == 5) roll.action(6);
            else if (r == 6) { if (auto* c = session().editClip()) askText("Rename clip", {}, c->name, "Rename", [this](const juce::String& n) { if (auto* cc = session().editClip()) { session().checkpoint("Rename clip"); cc->name = n; session().changed(); } }); }
            else if (r == 7) exportClip();
            else if (r == 8) importIntoClip();
            else if (r == 9) { roll.scrollY = -1.f; roll.repaint(); }
        });
    }
    void exportClip() {
        auto* c = session().editClip();
        if (!c) return;
        chooser = std::make_unique<juce::FileChooser>("Export MIDI clip", Paths::exports().getChildFile(juce::File::createLegalFileName(c->name) + ".mid"), "*.mid");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc) {
            auto* cc = session().editClip();
            if (!cc || fc.getResult() == juce::File()) return;
            juce::String err;
            if (MidiIO::exportClip(proj(), *cc, fc.getResult().withFileExtension("mid"), err)) ctx().toast("Exported"); else ctx().toast(err);
        });
    }
    void importIntoClip() {
        chooser = std::make_unique<juce::FileChooser>("Import MIDI notes", Paths::root(), "*.mid;*.midi");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            auto* cc = session().editClip();
            if (!cc || fc.getResult() == juce::File()) return;
            Project tmp; juce::String err; int added = 0;
            if (!MidiIO::importFile(tmp, fc.getResult(), err, added)) { ctx().toast(err); return; }
            session().checkpoint("Import MIDI notes");
            for (auto& t : tmp.tracks) for (auto& cl : t.clips) for (auto& n : cl.notes) cc->notes.push_back(n);
            int end = 0; for (auto& n : cc->notes) end = juce::jmax(end, n.start + n.length);
            const int bar = proj().ticksPerBarAt(cc->start);
            cc->contentLength = juce::jmax(cc->contentLength, ((end + bar - 1) / bar) * bar);
            cc->length = juce::jmax(cc->length, cc->contentLength);
            session().changed();
        });
    }

    Roll roll;
    Picker clipPicker;
    std::vector<juce::String> clipIds;
    IconButton drawB { "pencil", {}, "Draw notes (tap) / select (drag)" }, snapB { "magnet", {}, "Grid" }, chordB { "keys", {}, "Chord tool" }, scaleB { "check", {}, "Scale guide and key" },
        quantB { "wand", {}, "Quantize, humanize, velocity" }, playB { "play", {}, "Preview this clip in a loop" }, moreB { "more" }, velB { "volume", {}, "Velocity lane" };
    std::unique_ptr<juce::FileChooser> chooser;
};

}  // namespace

std::unique_ptr<Screen> makePianoRollScreen() { return std::make_unique<PianoRollScreen>(); }
