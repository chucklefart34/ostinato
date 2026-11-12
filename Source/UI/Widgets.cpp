#include "Widgets.h"
#include "Engine/Exporter.h"
#include "Core/Settings.h"

AppContext& ctx() { static AppContext c; return c; }

namespace UI {
void showMenu_(juce::PopupMenu& m, ParamSetPtr ps, int idx, juce::Component::SafePointer<Knob> self);

void haptic(int s) { if (AppSettings::get().haptics) PlatformAudio::haptic(s); }

void heading(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, float size) {
    g.setColour(pal().text);
    g.setFont(font(size, true));
    g.drawText(text, r, juce::Justification::centredLeft, true);
}

juce::String trackTypeName(TrackType t) {
    switch (t) {
        case TrackType::Instrument: return "Instrument";
        case TrackType::Drum: return "Drums";
        case TrackType::Audio: return "Audio";
        case TrackType::Bus: return "Group bus";
        case TrackType::Return: return "Effect return";
        default: return "Master";
    }
}
juce::String trackIcon(TrackType t) {
    switch (t) {
        case TrackType::Instrument: return "keys";
        case TrackType::Drum: return "drums";
        case TrackType::Audio: return "mic";
        case TrackType::Bus: return "mixer";
        case TrackType::Return: return "fx";
        default: return "volume";
    }
}
juce::String formatDuration(double s) {
    const int t = (int)std::round(s);
    return juce::String(t / 60) + ":" + juce::String(t % 60).paddedLeft('0', 2);
}

// ================================================================ IconButton
IconButton::IconButton(const juce::String& n, const juce::String& c, const juce::String& tip) : juce::Button(n), iconName(n), caption(c) {
    setTooltip(tip.isNotEmpty() ? tip : c);
    setTitle(tip.isNotEmpty() ? tip : (c.isNotEmpty() ? c : n));
    setWantsKeyboardFocus(false);
}

void IconButton::paintButton(juce::Graphics& g, bool over, bool down) {
    const auto& p = pal();
    auto r = getLocalBounds().toFloat();
    const bool on = getToggleState();
    const auto onC = hasOnColour ? onColour : p.accent;
    juce::Colour fg = on ? (filledWhenOn ? (onC.getPerceivedBrightness() > 0.55f ? juce::Colour(0xff17181b) : juce::Colours::white) : onC) : p.text;
    if (!isEnabled()) fg = p.faint;
    if (!quiet || on || down || over) {
        g.setColour(on && filledWhenOn ? onC : (down ? p.line : (over ? p.raised.brighter(0.05f) : (quiet ? juce::Colours::transparentBlack : p.raised))));
        g.fillRoundedRectangle(r.reduced(2.f), rControl);
    }
    if (captionBelow && caption.isNotEmpty()) {
        auto ir = r.withTrimmedBottom(16.f).withSizeKeepingCentre(22.f, 22.f);
        drawIcon(g, iconName, ir, fg);
        g.setColour(on && !filledWhenOn ? onC : (on ? fg : p.dim));
        g.setFont(font(11.5f, on));
        g.drawText(caption, r.removeFromBottom(18.f).toNearestInt(), juce::Justification::centredTop, true);
    } else if (caption.isNotEmpty() && r.getWidth() > r.getHeight() * 1.8f) {
        auto ir = r.removeFromLeft(r.getHeight()).withSizeKeepingCentre(20.f, 20.f).translated(4.f, 0.f);
        drawIcon(g, iconName, ir, fg);
        g.setColour(fg);
        g.setFont(font(15.f, true));
        g.drawText(caption, r.toNearestInt().withTrimmedRight(8).withTrimmedLeft(4), juce::Justification::centredLeft, true);
    } else {
        drawIcon(g, iconName, r.withSizeKeepingCentre(22.f, 22.f), fg);
    }
}

void IconButton::mouseDown(const juce::MouseEvent& e) {
    longFired = false;
    if (onLongPress) startTimer(500);
    juce::Button::mouseDown(e);
}
void IconButton::mouseUp(const juce::MouseEvent& e) {
    stopTimer();
    if (longFired) { setState(juce::Button::buttonNormal); return; }
    juce::Button::mouseUp(e);
}
void IconButton::timerCallback() {
    stopTimer();
    longFired = true;
    haptic(1);
    if (onLongPress) onLongPress();
}

// ================================================================ Chip
Chip::Chip(const juce::String& t, juce::Colour a) : juce::Button(t), accent(a) { setClickingTogglesState(true); }
void Chip::paintButton(juce::Graphics& g, bool over, bool down) {
    const auto& p = pal();
    auto r = getLocalBounds().toFloat().reduced(2.f);
    const auto a = accent.isTransparent() ? p.accent : accent;
    const bool on = getToggleState();
    g.setColour(on ? a : (down || over ? p.line : p.raised));
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    g.setColour(on ? (a.getPerceivedBrightness() > 0.55f ? juce::Colour(0xff17181b) : juce::Colours::white) : p.text);
    g.setFont(font(14.f, true));
    g.drawFittedText(getButtonText(), getLocalBounds().reduced(10, 0), juce::Justification::centred, 1);
}

// ================================================================ Picker
Picker::Picker() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
void Picker::setItems(const juce::StringArray& i, int sel) { items = i; selected = sel; repaint(); }
void Picker::setSelected(int i, bool notify) { selected = i; repaint(); if (notify && onChange) onChange(i); }
void Picker::paint(juce::Graphics& g) {
    const auto& p = pal();
    auto r = getLocalBounds().toFloat().reduced(1.f);
    g.setColour(p.raised);
    g.fillRoundedRectangle(r, rControl);
    auto t = getLocalBounds().reduced(12, 0);
    drawIcon(g, "down", t.removeFromRight(18).toFloat().withSizeKeepingCentre(16, 16), p.dim, 2.f);
    if (label.isNotEmpty()) {
        g.setColour(p.dim);
        g.setFont(font(13.f));
        g.drawText(label, t.removeFromLeft(juce::GlyphArrangement::getStringWidthInt(font(13.f), label) + 10), juce::Justification::centredLeft);
    }
    g.setColour(p.text);
    g.setFont(font(15.f, true));
    g.drawFittedText(getText(), t, juce::Justification::centredLeft, 1);
}
void Picker::mouseUp(const juce::MouseEvent& e) {
    if (!getLocalBounds().contains(e.getPosition())) return;
    juce::PopupMenu m;
    for (int i = 0; i < items.size(); ++i) m.addItem(i + 1, items[i], true, i == selected);
    showMenu(m, this, [this](int r) { if (r > 0) setSelected(r - 1, true); });
}

// ================================================================ Knob
Knob::Knob(ParamSetPtr ps, int i, const juce::String& tgt, const juce::String& tid) : params(std::move(ps)), index(i), target(tgt), trackId(tid) {
    colour = pal().accent;
    setTitle(params->meta(i).name);
    setTooltip(juce::String(params->meta(i).name) + ": drag to change, double-tap to reset, long-press for more");
    startTimerHz(20);
}

void Knob::timerCallback() {
    const float v = params->get(index);
    const bool a = params->isAutomated(index);
    if (v != lastShown || a != wasAutomated) { lastShown = v; wasAutomated = a; repaint(); }
    if (longPressPending && !dragging && juce::Time::getMillisecondCounter() - downTime > 500) {
        longPressPending = false;
        haptic(1);
        showMenu();
    }
}

void Knob::paint(juce::Graphics& g) {
    const auto& p = pal();
    const auto& m = params->meta(index);
    auto r = getLocalBounds().toFloat();
    auto label = r.removeFromBottom(compact ? 14.f : 34.f);
    const float d = juce::jmin(r.getWidth(), r.getHeight()) - 4.f;
    auto kr = r.withSizeKeepingCentre(d, d);
    const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
    const float norm = juce::jlimit(0.f, 1.f, params->toNorm(index, params->get(index)));
    const float th = juce::jmax(3.f, d * 0.1f);
    juce::Path track;
    track.addCentredArc(kr.getCentreX(), kr.getCentreY(), d * 0.5f - th, d * 0.5f - th, 0.f, a0, a1, true);
    g.setColour(p.line);
    g.strokePath(track, juce::PathStrokeType(th, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const bool bipolar = m.min < 0.f && m.max > 0.f;
    const float from = bipolar ? a0 + params->toNorm(index, 0.f) * (a1 - a0) : a0;
    const float ang = a0 + norm * (a1 - a0);
    juce::Path val;
    val.addCentredArc(kr.getCentreX(), kr.getCentreY(), d * 0.5f - th, d * 0.5f - th, 0.f, juce::jmin(from, ang), juce::jmax(from, ang) + 0.001f, true);
    g.setColour(params->isAutomated(index) ? p.play : colour);
    g.strokePath(val, juce::PathStrokeType(th, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    auto cap = kr.reduced(th * 2.f);
    g.setColour(dragging ? p.line : p.raised);
    g.fillEllipse(cap);
    juce::Path ind;
    ind.addRoundedRectangle(-1.5f, -cap.getHeight() * 0.5f + 3.f, 3.f, cap.getHeight() * 0.3f, 1.5f);
    g.setColour(p.text);
    g.fillPath(ind, juce::AffineTransform::rotation(ang).translated(cap.getCentreX(), cap.getCentreY()));
    if (compact) {
        g.setColour(p.dim);
        g.setFont(font(11.f));
        g.drawFittedText(dragging ? valueText() : labelText(), label.toNearestInt(), juce::Justification::centred, 1);
        return;
    }
    juce::ignoreUnused(m);
    g.setColour(p.dim);
    g.setFont(font(12.f));
    g.drawFittedText(labelText(), label.removeFromTop(16.f).toNearestInt(), juce::Justification::centred, 1);
    g.setColour(dragging ? p.accent : p.text);
    g.setFont(font(13.f, true));
    g.drawFittedText(valueText(), label.toNearestInt(), juce::Justification::centred, 1);
}
juce::String Knob::labelText() const { return labelFn ? labelFn() : juce::String(params->meta(index).name); }
juce::String Knob::valueText() const { return formatFn ? formatFn(params->get(index)) : params->format(index, params->get(index)); }

void Knob::setNorm(float n) {
    const float v = params->fromNorm(index, n);
    params->set(index, v);
    if (ctx().paramMoved) ctx().paramMoved(params.get(), index, params->toNorm(index, v));
    if (onChange) onChange();
    repaint();
}

void Knob::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) { showMenu(); return; }
    dragStartNorm = params->toNorm(index, params->getBase(index));
    downPos = e.position;
    downTime = juce::Time::getMillisecondCounter();
    longPressPending = true;
    dragging = false;
}

void Knob::mouseDrag(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) return;
    const auto delta = e.position - downPos;
    if (!dragging && delta.getDistanceFromOrigin() < 4.f) return;
    if (!dragging) {
        dragging = true;
        longPressPending = false;
        if (ctx().beginEdit) ctx().beginEdit("Change " + juce::String(params->meta(index).name));
    }
    // vertical drag is primary; horizontal also works (handy in scrolling lists)
    const float sens = e.mods.isShiftDown() ? 1200.f : 220.f;
    const float amount = (-delta.y + delta.x * 0.6f) / sens;
    setNorm(juce::jlimit(0.f, 1.f, dragStartNorm + amount));
}

void Knob::mouseUp(const juce::MouseEvent&) {
    longPressPending = false;
    if (dragging) { dragging = false; if (ctx().endEdit) ctx().endEdit(); repaint(); }
}

void Knob::mouseDoubleClick(const juce::MouseEvent&) {
    if (ctx().beginEdit) ctx().beginEdit("Reset " + juce::String(params->meta(index).name));
    params->set(index, params->meta(index).def);
    if (ctx().endEdit) ctx().endEdit();
    if (onChange) onChange();
    haptic(0);
    repaint();
}

void Knob::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    const float n = params->toNorm(index, params->getBase(index)) + w.deltaY * 0.05f;
    setNorm(juce::jlimit(0.f, 1.f, n));
}

void Knob::showMenu() {
    juce::PopupMenu m;
    m.addSectionHeader(juce::String(params->meta(index).name) + ": " + params->format(index, params->get(index)));
    m.addItem(1, "Type a value...");
    m.addItem(2, "Reset to default");
    m.addItem(3, ctx().engine && ctx().engine->isLearning() ? "Cancel MIDI learn" : "MIDI learn (move a controller)");
    if (target.isNotEmpty() && ctx().automate) m.addItem(4, "Automate this...");
    auto ps = params;
    const int idx = index;
    juce::Component::SafePointer<Knob> self(this);
    showMenu_(m, ps, idx, self);
}

}  // namespace UI

// helper kept outside the class to keep the async lambda simple
namespace UI {
void showMenu_(juce::PopupMenu& m, ParamSetPtr ps, int idx, juce::Component::SafePointer<Knob> self) {
    showMenu(m, self.getComponent(), [ps, idx, self](int r) {
        if (r == 1) {
            askText(ps->meta(idx).name, "Enter a value (" + ps->format(idx, ps->meta(idx).min) + " to " + ps->format(idx, ps->meta(idx).max) + ")",
                    juce::String(ps->getBase(idx)), "Set", [ps, idx, self](const juce::String& t) {
                        if (ctx().beginEdit) ctx().beginEdit("Set " + juce::String(ps->meta(idx).name));
                        ps->set(idx, t.getFloatValue());
                        if (ctx().endEdit) ctx().endEdit();
                        if (self) { self->repaint(); if (self->onChange) self->onChange(); }
                    });
        } else if (r == 2) {
            if (ctx().beginEdit) ctx().beginEdit("Reset");
            ps->set(idx, ps->meta(idx).def);
            if (ctx().endEdit) ctx().endEdit();
            if (self) self->repaint();
        } else if (r == 3) {
            if (ctx().engine->isLearning()) { ctx().engine->cancelMidiLearn(); return; }
            ctx().engine->startMidiLearn(ps, idx);
            if (ctx().toast) ctx().toast("Move a knob or fader on your MIDI controller");
        } else if (r == 4 && self) {
            ctx().automate(ps, idx, juce::String(), juce::String());
        }
    });
}
}  // namespace UI

namespace UI {
// ================================================================ ParamField
ParamField::ParamField(ParamSetPtr ps, int i) : params(std::move(ps)), index(i) { startTimerHz(10); setTitle(params->meta(i).name); }
void ParamField::paint(juce::Graphics& g) {
    const auto& p = pal();
    const auto& m = params->meta(index);
    auto r = getLocalBounds().toFloat().reduced(2.f);
    if (m.scale == ParamScale::Bool) {
        const bool on = params->getBool(index);
        g.setColour(on ? p.accent : p.raised);
        g.fillRoundedRectangle(r, rControl);
        g.setColour(on ? p.accentText : p.text);
        g.setFont(font(14.f, true));
        g.drawFittedText(m.name, r.toNearestInt().reduced(6, 0), juce::Justification::centred, 2);
        return;
    }
    g.setColour(p.raised);
    g.fillRoundedRectangle(r, rControl);
    auto t = r.toNearestInt().reduced(10, 4);
    g.setColour(p.dim);
    g.setFont(font(12.f));
    g.drawFittedText(m.name, t.removeFromTop(t.getHeight() / 2), juce::Justification::centredLeft, 1);
    g.setColour(p.text);
    g.setFont(font(14.f, true));
    g.drawFittedText(params->format(index, params->get(index)), t, juce::Justification::centredLeft, 1);
    drawIcon(g, m.scale == ParamScale::Int ? "more" : "down", { r.getRight() - 22.f, r.getCentreY() - 7.f, 14.f, 14.f }, p.dim, 2.f);
}
void ParamField::mouseUp(const juce::MouseEvent& e) {
    if (!getLocalBounds().contains(e.getPosition())) return;
    const auto& m = params->meta(index);
    auto ps = params;
    const int idx = index;
    juce::Component::SafePointer<ParamField> self(this);
    auto apply = [ps, idx, self](float v) {
        if (ctx().beginEdit) ctx().beginEdit("Change " + juce::String(ps->meta(idx).name));
        ps->set(idx, v);
        if (ctx().endEdit) ctx().endEdit();
        if (self) { self->repaint(); if (self->onChange) self->onChange(); }
    };
    if (m.scale == ParamScale::Bool) { apply(params->getBool(index) ? 0.f : 1.f); haptic(0); return; }
    juce::PopupMenu menu;
    const int lo = (int)m.min, hi = (int)m.max;
    for (int v = lo; v <= hi; ++v) menu.addItem(v - lo + 1, params->format(index, (float)v), true, params->getInt(index) == v);
    showMenu(menu, this, [apply, lo](int r) { if (r > 0) apply((float)(r - 1 + lo)); });
}

// ================================================================ ParamPanel
void ParamPanel::setParams(ParamSetPtr ps, const juce::String& prefix, const juce::String& trackId, juce::Colour colour, const juce::StringArray& hide, const juce::StringArray& onlyGroups) {
    params = ps;
    owned.clear();
    groups.clear();
    removeAllChildren();
    if (!ps) return;
    for (int i = 0; i < ps->size(); ++i) {
        const auto& m = ps->meta(i);
        if (hide.contains(m.id)) continue;
        if (!onlyGroups.isEmpty() && !onlyGroups.contains(m.group)) continue;
        juce::Component* c;
        if (m.scale == ParamScale::Bool || m.scale == ParamScale::Choice || (m.scale == ParamScale::Int && m.max - m.min <= 24 && juce::String(m.unit) != "st")) {
            auto* f = new ParamField(ps, i);
            f->onChange = [this] { if (onChange) onChange(); };
            c = f;
        } else {
            auto* k = new Knob(ps, i, prefix.isNotEmpty() ? prefix + m.id : juce::String(), trackId);
            if (!colour.isTransparent()) k->setColour(colour);
            k->onChange = [this] { if (onChange) onChange(); };
            if (knobLabel) k->labelFn = [this, i] { return knobLabel(i); };
            if (knobFormat) k->formatFn = [this, i](float v) { return knobFormat(i, v); };
            c = k;
        }
        owned.add(c);
        addAndMakeVisible(c);
        if (groups.empty() || groups.back().name != m.group) groups.push_back({ m.group, {} });
        groups.back().items.push_back(c);
    }
    resized();
}

namespace {
class ParamSheet : public juce::Component {
public:
    ParamSheet(const juce::String& t, ParamSetPtr ps, const juce::String& trackId, std::function<void()> aud) : title(t), audition(std::move(aud)) {
        panel.setParams(ps, {}, trackId);
        view.setViewedComponent(&panel, false);
        view.setScrollBarsShown(true, false);
        view.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
        addAndMakeVisible(view);
        if (audition) { addAndMakeVisible(play); play.onClick = [this] { audition(); }; }
        const int w = 360;
        setSize(w, juce::jmin(560, panel.preferredHeight(w - 24) + 70));
    }
    void paint(juce::Graphics& g) override {
        g.setColour(pal().text);
        g.setFont(font(16.f, true));
        g.drawText(title, getLocalBounds().reduced(12, 8).removeFromTop(36), juce::Justification::centredLeft);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(12, 8);
        auto top = r.removeFromTop(40);
        if (audition) play.setBounds(top.removeFromRight(90).reduced(0, 2));
        view.setBounds(r);
        panel.setSize(r.getWidth() - 10, panel.preferredHeight(r.getWidth() - 10) + 10);
    }
private:
    juce::String title;
    std::function<void()> audition;
    juce::Viewport view;
    ParamPanel panel;
    juce::TextButton play { "Play" };
};
}  // namespace

void showParamSheet(const juce::String& title, ParamSetPtr ps, juce::Component* target, const juce::String& trackId, std::function<void()> audition) {
    if (!ps || !target) return;
    auto sheet = std::make_unique<ParamSheet>(title, ps, trackId, std::move(audition));
    auto* top = target->getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously(std::move(sheet), top->getLocalArea(target, target->getLocalBounds()), top);
}

int ParamPanel::layout(int width, bool apply) {
    const int knobW = 78, knobH = 96, fieldW = 150, fieldH = 52, head = 30;
    int y = 0;
    for (auto& gr : groups) {
        y += head;
        int x = 0, rowH = 0;
        for (auto* c : gr.items) {
            const bool isKnob = dynamic_cast<Knob*>(c) != nullptr;
            const int w = isKnob ? knobW : fieldW, h = isKnob ? knobH : fieldH;
            if (x + w > width && x > 0) { x = 0; y += rowH + 6; rowH = 0; }
            if (apply) c->setBounds(x, y, w, h);
            x += w + 6;
            rowH = juce::jmax(rowH, h);
        }
        y += rowH + 14;
    }
    return y;
}
int ParamPanel::preferredHeight(int width) { return layout(width, false); }
void ParamPanel::resized() { layout(getWidth(), true); }
void ParamPanel::paint(juce::Graphics& g) {
    // group headings
    const int knobW = 78, knobH = 96, fieldW = 150, fieldH = 52, head = 30;
    int y = 0;
    for (auto& gr : groups) {
        heading(g, gr.name, { 0, y + 4, getWidth(), head - 8 }, 15.f);
        y += head;
        int x = 0, rowH = 0;
        for (auto* c : gr.items) {
            const bool isKnob = dynamic_cast<Knob*>(c) != nullptr;
            const int w = isKnob ? knobW : fieldW, h = isKnob ? knobH : fieldH;
            if (x + w > getWidth() && x > 0) { x = 0; y += rowH + 6; rowH = 0; }
            x += w + 6;
            rowH = juce::jmax(rowH, h);
        }
        y += rowH + 14;
    }
}

// ================================================================ Fader
Fader::Fader(ParamSetPtr ps, int i, juce::Colour c) : params(std::move(ps)), index(i), colour(c) {
    setTitle("Volume");
    setTooltip("Volume: drag, double-tap for 0 dB");
    startTimerHz(20);
}
void Fader::timerCallback() { const float v = params->get(index); if (v != shown) { shown = v; repaint(); } }
float Fader::valueToY(float db) const {
    // fader law: top 2/3 covers -18..+6 dB, bottom third -60..-18
    const float top = 12.f, bot = (float)getHeight() - 12.f;
    float t;
    if (db >= -18.f) t = (db + 18.f) / 24.f * 0.667f + 0.333f;
    else t = (db + 60.f) / 42.f * 0.333f;
    return bot - t * (bot - top);
}
float Fader::yToValue(float y) const {
    const float top = 12.f, bot = (float)getHeight() - 12.f;
    const float t = juce::jlimit(0.f, 1.f, (bot - y) / (bot - top));
    return t >= 0.333f ? -18.f + (t - 0.333f) / 0.667f * 24.f : -60.f + t / 0.333f * 42.f;
}
void Fader::paint(juce::Graphics& g) {
    const auto& p = pal();
    const float x = getWidth() * 0.5f;
    const float db = params->get(index);
    g.setColour(p.line);
    g.fillRoundedRectangle(x - 3.f, 12.f, 6.f, getHeight() - 24.f, 3.f);
    g.setColour(params->isAutomated(index) ? p.play : colour);
    const float y = valueToY(db);
    g.fillRoundedRectangle(x - 3.f, y, 6.f, getHeight() - 12.f - y, 3.f);
    g.setColour(p.dim);
    for (float m : { 6.f, 0.f, -6.f, -12.f, -24.f, -48.f }) g.fillRect(x + 10.f, valueToY(m), m == 0.f ? 8.f : 5.f, 1.f);
    auto thumb = juce::Rectangle<float>(x - 18.f, y - 11.f, 36.f, 22.f);
    g.setColour(p.text);
    g.fillRoundedRectangle(thumb, 6.f);
    g.setColour(p.surface);
    g.fillRect(thumb.withSizeKeepingCentre(20.f, 2.f));
}
void Fader::mouseDown(const juce::MouseEvent& e) {
    startDb = params->getBase(index);
    if (ctx().beginEdit) ctx().beginEdit("Change volume");
    juce::ignoreUnused(e);
}
void Fader::mouseDrag(const juce::MouseEvent& e) {
    // relative drag: precise and does not jump under the finger
    const float y0 = valueToY(startDb);
    const float sens = e.mods.isShiftDown() ? 0.25f : 1.f;
    const float v = yToValue(y0 + e.getDistanceFromDragStartY() * sens);
    params->set(index, v);
    if (ctx().paramMoved) ctx().paramMoved(params.get(), index, params->toNorm(index, v));
    repaint();
}
void Fader::mouseUp(const juce::MouseEvent&) { if (ctx().endEdit) ctx().endEdit(); }
void Fader::mouseDoubleClick(const juce::MouseEvent&) { params->set(index, 0.f); haptic(0); repaint(); }

// ================================================================ Meter
void Meter::timerCallback() {
    float nl = 0.f, nr = 0.f;
    if (src) { nl = src->peakL.exchange(0.f); nr = src->peakR.exchange(0.f); clip = clip || src->clipped.load(); }
    else if (levelSource) { auto lv = levelSource(); nl = lv.first; nr = lv.second; clip = clip || nl > 0.99f || nr > 0.99f; }
    l = juce::jmax(nl, l * 0.82f);
    r = juce::jmax(nr, r * 0.82f);
    if (nl >= holdL || holdTicks <= 0) { holdL = nl; holdTicks = 30; }
    if (nr >= holdR) holdR = nr;
    if (--holdTicks <= 0) { holdL *= 0.9f; holdR *= 0.9f; }
    repaint();
}
void Meter::paint(juce::Graphics& g) {
    const auto& p = pal();
    auto r0 = getLocalBounds().toFloat();
    auto toPos = [](float lin) { const float db = juce::Decibels::gainToDecibels(lin, -60.f); return juce::jlimit(0.f, 1.f, (db + 60.f) / 66.f); };
    auto clipR = horizontal ? r0.removeFromRight(6.f) : r0.removeFromTop(6.f);
    g.setColour(clip ? p.record : p.line);
    g.fillRoundedRectangle(clipR.reduced(0.5f), 2.f);
    const float vals[2] = { l, r }, holds[2] = { holdL, holdR };
    for (int c = 0; c < 2; ++c) {
        auto bar = horizontal ? r0.withHeight(r0.getHeight() * 0.5f - 1.f).withY(r0.getY() + c * r0.getHeight() * 0.5f)
                              : r0.withWidth(r0.getWidth() * 0.5f - 1.f).withX(r0.getX() + c * r0.getWidth() * 0.5f);
        g.setColour(p.line.withAlpha(0.6f));
        g.fillRoundedRectangle(bar, 2.f);
        const float pos = toPos(vals[c]);
        juce::Colour col = vals[c] > 0.89f ? p.record : (vals[c] > 0.5f ? p.warn : p.play);
        g.setColour(col);
        if (horizontal) g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * pos), 2.f);
        else g.fillRoundedRectangle(bar.withTop(bar.getBottom() - bar.getHeight() * pos), 2.f);
        const float hp = toPos(holds[c]);
        g.setColour(p.text.withAlpha(0.7f));
        if (horizontal) g.fillRect(bar.getX() + bar.getWidth() * hp - 1.f, bar.getY(), 2.f, bar.getHeight());
        else g.fillRect(bar.getX(), bar.getBottom() - bar.getHeight() * hp, bar.getWidth(), 2.f);
    }
}

// ================================================================ waveform
void drawWaveform(juce::Graphics& g, const SampleData& s, juce::Rectangle<float> a, juce::Colour c, double startSec, double lenSec, bool reverse) {
    if (s.ovMax.empty() || a.getWidth() < 1.f) return;
    if (lenSec < 0) lenSec = s.seconds() - startSec;
    const double binsPerSec = s.sampleRate / SampleData::overviewStep;
    const float mid = a.getCentreY(), half = a.getHeight() * 0.5f;
    juce::Path path;
    const int w = (int)a.getWidth();
    std::vector<float> tops((size_t)w), bots((size_t)w);
    for (int x = 0; x < w; ++x) {
        double t0 = startSec + lenSec * x / w, t1 = startSec + lenSec * (x + 1) / w;
        if (reverse) { const double L = s.seconds(); const double a0 = L - t1, a1 = L - t0; t0 = a0; t1 = a1; }
        int b0 = (int)(t0 * binsPerSec), b1 = juce::jmax(b0 + 1, (int)(t1 * binsPerSec));
        float lo = 0.f, hi = 0.f;
        for (int b = juce::jmax(0, b0); b < juce::jmin((int)s.ovMax.size(), b1); ++b) { lo = juce::jmin(lo, s.ovMin[(size_t)b]); hi = juce::jmax(hi, s.ovMax[(size_t)b]); }
        tops[(size_t)x] = mid - hi * half;
        bots[(size_t)x] = mid - lo * half;
    }
    path.startNewSubPath(a.getX(), tops[0]);
    for (int x = 1; x < w; ++x) path.lineTo(a.getX() + (float)x, tops[(size_t)x]);
    for (int x = w - 1; x >= 0; --x) path.lineTo(a.getX() + (float)x, juce::jmax(bots[(size_t)x], tops[(size_t)x] + 1.f));
    path.closeSubPath();
    g.setColour(c);
    g.fillPath(path);
}

// ================================================================ Keyboard
Keyboard::Keyboard() { setTitle("Keyboard"); }
void Keyboard::setRange(int lo, int whites) { lowNote = lo - (isBlack(lo) ? 1 : 0); whiteKeys = juce::jlimit(7, 52, whites); repaint(); }
void Keyboard::shiftOctave(int d) { allOff(); lowNote = juce::jlimit(0, 108, lowNote + d * 12); repaint(); }
void Keyboard::allOff() { for (int n : down) if (onNoteOff) onNoteOff(n); down.clear(); touchNote.clear(); repaint(); }

juce::Rectangle<float> Keyboard::keyRect(int note) const {
    const float ww = (float)getWidth() / whiteKeys;
    int whiteIdx = 0;
    for (int n = lowNote; n < note; ++n) if (!isBlack(n)) ++whiteIdx;
    if (!isBlack(note)) return { whiteIdx * ww, 0.f, ww, (float)getHeight() };
    return { whiteIdx * ww - ww * 0.32f, 0.f, ww * 0.64f, getHeight() * 0.6f };
}

int Keyboard::noteAt(juce::Point<float> p, float* vel) const {
    int hi = lowNote;
    for (int whites = 0; whites < whiteKeys; ++hi) if (!isBlack(hi)) ++whites;
    for (int n = lowNote; n < hi; ++n)
        if (isBlack(n) && keyRect(n).contains(p)) { if (vel) *vel = juce::jlimit(0.25f, 1.f, 0.3f + p.y / (getHeight() * 0.6f) * 0.7f); return n; }
    for (int n = lowNote; n < hi; ++n)
        if (!isBlack(n) && keyRect(n).contains(p)) { if (vel) *vel = juce::jlimit(0.25f, 1.f, 0.3f + p.y / getHeight() * 0.7f); return n; }
    return -1;
}

void Keyboard::paint(juce::Graphics& g) {
    const auto& p = pal();
    int hi = lowNote;
    for (int whites = 0; whites < whiteKeys; ++hi) if (!isBlack(hi)) ++whites;
    for (int pass = 0; pass < 2; ++pass)
        for (int n = lowNote; n < hi; ++n) {
            if (isBlack(n) != (pass == 1)) continue;
            auto r = keyRect(n).reduced(pass ? 0.f : 1.f, 0.f);
            const bool on = down.count(n) > 0;
            juce::Colour c = pass ? (UI::isDark() ? juce::Colour(0xff0e0f11) : juce::Colour(0xff2a2b2f)) : (UI::isDark() ? juce::Colour(0xffe9e6e0) : juce::Colours::white);
            if (inScale && !inScale(n)) c = pass ? c : c.darker(0.12f);
            if (on) c = p.accent;
            g.setColour(c);
            g.fillRoundedRectangle(r.withTrimmedTop(-6.f), pass ? 4.f : 6.f);
            if (!pass && n % 12 == 0) {
                g.setColour(on ? p.accentText : juce::Colour(0xff6b6862));
                g.setFont(font(11.f, true));
                g.drawText("C" + juce::String(n / 12 - 1), r.removeFromBottom(20.f).toNearestInt(), juce::Justification::centred);
            }
        }
}

void Keyboard::mouseDown(const juce::MouseEvent& e) {
    float vel = 0.8f;
    const int n = noteAt(e.position, &vel);
    if (n < 0) return;
    if (fixedVelocity > 0.f) vel = fixedVelocity;
    touchNote[e.source.getIndex()] = n;
    down.insert(n);
    if (onNoteOn) onNoteOn(n, vel);
    repaint();
}
void Keyboard::mouseDrag(const juce::MouseEvent& e) {
    auto it = touchNote.find(e.source.getIndex());
    if (it == touchNote.end()) return;
    float vel = 0.8f;
    const int n = noteAt(e.position, &vel);
    if (n == it->second || n < 0) return;
    if (fixedVelocity > 0.f) vel = fixedVelocity;
    down.erase(it->second);
    if (onNoteOff) onNoteOff(it->second);
    it->second = n;
    down.insert(n);
    if (onNoteOn) onNoteOn(n, vel);
    repaint();
}
void Keyboard::mouseUp(const juce::MouseEvent& e) {
    auto it = touchNote.find(e.source.getIndex());
    if (it == touchNote.end()) return;
    down.erase(it->second);
    if (onNoteOff) onNoteOff(it->second);
    touchNote.erase(it);
    repaint();
}

// ================================================================ dialogs
static juce::AlertWindow* makeAlert(const juce::String& title, const juce::String& msg) {
    auto* w = new juce::AlertWindow(title, msg, juce::MessageBoxIconType::NoIcon, ctx().root);
    return w;
}

void askText(const juce::String& title, const juce::String& message, const juce::String& initial, const juce::String& okText, std::function<void(const juce::String&)> onOk) {
    auto* w = makeAlert(title, message);
    w->addTextEditor("text", initial);
    if (auto* ed = w->getTextEditor("text")) { ed->setFont(font(17.f)); ed->setSelectAllWhenFocused(true); ed->setJustification(juce::Justification::centredLeft); }
    w->addButton(okText, 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([w, onOk](int r) {
        if (r == 1 && onOk) onOk(w->getTextEditorContents("text"));
    }), true);
}

void confirm(const juce::String& title, const juce::String& message, const juce::String& okText, std::function<void()> onOk, bool destructive) {
    auto* w = makeAlert(title, message);
    w->addButton(okText, 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    if (destructive) if (auto* b = w->getButton(okText)) b->setColour(juce::TextButton::buttonColourId, pal().record.withAlpha(0.85f));
    w->enterModalState(true, juce::ModalCallbackFunction::create([onOk](int r) { if (r == 1 && onOk) onOk(); }), true);
}

void info(const juce::String& title, const juce::String& message) {
    auto* w = makeAlert(title, message);
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->enterModalState(true, nullptr, true);
}

void showMenu(juce::PopupMenu& m, juce::Component* target, std::function<void(int)> onResult) {
    auto opts = juce::PopupMenu::Options().withMinimumWidth(220).withStandardItemHeight(44);
    if (target) opts = opts.withTargetComponent(target);
    if (ctx().root) opts = opts.withParentComponent(ctx().root);
    m.setLookAndFeel(&juce::LookAndFeel::getDefaultLookAndFeel());
    m.showMenuAsync(opts, [onResult](int r) { if (onResult) onResult(r); });
}

// ================================================================ Pinch
void Pinch::down(const juce::MouseEvent& e) { points[e.source.getIndex()] = e.position; start = points; }
void Pinch::up(const juce::MouseEvent& e) { points.erase(e.source.getIndex()); start = points; }
bool Pinch::drag(const juce::MouseEvent& e, float& sx, float& sy, juce::Point<float>& centre) {
    points[e.source.getIndex()] = e.position;
    if (points.size() < 2 || start.size() < 2) return false;
    auto a = points.begin(), b = std::next(a);
    auto a0 = start.find(a->first), b0 = start.find(b->first);
    if (a0 == start.end() || b0 == start.end()) return false;
    const float dx0 = std::abs(a0->second.x - b0->second.x), dy0 = std::abs(a0->second.y - b0->second.y);
    const float dx1 = std::abs(a->second.x - b->second.x), dy1 = std::abs(a->second.y - b->second.y);
    sx = dx0 > 30.f ? dx1 / dx0 : 1.f;
    sy = dy0 > 30.f ? dy1 / dy0 : 1.f;
    centre = (a->second + b->second) * 0.5f;
    return true;
}

}  // namespace UI
