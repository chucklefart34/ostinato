// Reusable touch-first controls.
#pragma once
#include "AppContext.h"
#include "Engine/Renderer.h"

namespace UI {

// Icon (+ optional caption) button. Toggleable; long-press fires onLongPress.
class IconButton : public juce::Button, private juce::Timer {
public:
    IconButton(const juce::String& iconName, const juce::String& caption = {}, const juce::String& tip = {});
    void setIcon(const juce::String& n) { iconName = n; repaint(); }
    void setCaption(const juce::String& c) { caption = c; repaint(); }
    void setOnColour(juce::Colour c) { onColour = c; hasOnColour = true; repaint(); }
    void setFilledWhenOn(bool f) { filledWhenOn = f; }
    void setCaptionBelow(bool b) { captionBelow = b; }
    void setQuiet(bool q) { quiet = q; }
    std::function<void()> onLongPress;
    void paintButton(juce::Graphics&, bool over, bool down) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
private:
    void timerCallback() override;
    juce::String iconName, caption;
    juce::Colour onColour;
    bool hasOnColour = false, filledWhenOn = true, captionBelow = false, quiet = false, longFired = false;
};

// Pill-shaped text toggle.
class Chip : public juce::Button {
public:
    explicit Chip(const juce::String& text, juce::Colour accent = {});
    void paintButton(juce::Graphics&, bool over, bool down) override;
    juce::Colour accent;
};

// Popup-menu backed picker that looks like a field (touch friendly, no tiny dropdown).
class Picker : public juce::Component {
public:
    Picker();
    void setItems(const juce::StringArray& items, int selected = 0);
    void setSelected(int i, bool notify = false);
    int getSelected() const { return selected; }
    juce::String getText() const { return juce::isPositiveAndBelow(selected, items.size()) ? items[selected] : juce::String(); }
    void setLabel(const juce::String& l) { label = l; repaint(); }
    std::function<void(int)> onChange;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
private:
    juce::StringArray items;
    juce::String label;
    int selected = 0;
};

// Knob bound to a parameter. Drag up/down (or sideways), double-tap = default,
// long-press / right-click = menu (type value, reset, MIDI learn, automate).
class Knob : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
public:
    Knob(ParamSetPtr ps, int index, const juce::String& automationTarget = {}, const juce::String& trackId = {});
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void setColour(juce::Colour c) { colour = c; }
    void setCompact(bool c) { compact = c; }
    std::function<void()> onChange;
    // Optional context-dependent label and value text (e.g. Effect Designer module knobs).
    std::function<juce::String()> labelFn;
    std::function<juce::String(float)> formatFn;
    juce::String labelText() const;
    juce::String valueText() const;
    ParamSetPtr params;
    int index;
private:
    void timerCallback() override;
    void showMenu();
    void setNorm(float n);
    juce::String target, trackId;
    juce::Colour colour;
    float dragStartNorm = 0.f, lastShown = -1.e9f;
    bool compact = false, dragging = false, longPressPending = false, wasAutomated = false;
    juce::Point<float> downPos;
    juce::uint32 downTime = 0;
};

// Choice/bool/int parameter as a tappable field.
class ParamField : public juce::Component, private juce::Timer {
public:
    ParamField(ParamSetPtr ps, int index);
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    std::function<void()> onChange;
private:
    void timerCallback() override { if (params->get(index) != shown) { shown = params->get(index); repaint(); } }
    ParamSetPtr params;
    int index;
    float shown = -1.f;
};

// Auto-generated editor for a ParamSet, grouped by section, reflowing for any width.
class ParamPanel : public juce::Component {
public:
    ParamPanel() = default;
    // onlyGroups: show just these sections (tabbed editors); empty = all.
    void setParams(ParamSetPtr ps, const juce::String& targetPrefix, const juce::String& trackId, juce::Colour colour = {},
                   const juce::StringArray& hide = {}, const juce::StringArray& onlyGroups = {});
    std::function<juce::String(int index)> knobLabel;               // optional dynamic knob labels
    std::function<juce::String(int index, float value)> knobFormat; // optional dynamic value text
    void resized() override;
    void paint(juce::Graphics&) override;
    int preferredHeight(int width);
    std::function<void()> onChange;
private:
    struct Group { juce::String name; std::vector<juce::Component*> items; };
    ParamSetPtr params;
    std::vector<Group> groups;
    juce::OwnedArray<juce::Component> owned;
    int layout(int width, bool apply);
};

// Vertical fader for a channel volume parameter (dB, -60..+6).
class Fader : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
public:
    Fader(ParamSetPtr ps, int index, juce::Colour colour);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
private:
    void timerCallback() override;
    float valueToY(float db) const;
    float yToValue(float y) const;
    ParamSetPtr params;
    int index;
    juce::Colour colour;
    float startDb = 0.f, shown = -1000.f;
};

// Stereo peak meter fed by a ChannelProc; tap to clear the clip indicator.
class Meter : public juce::Component, private juce::Timer {
public:
    Meter() { startTimerHz(30); }
    void setSource(std::shared_ptr<ChannelProc> c) { src = std::move(c); }
    void setHorizontal(bool h) { horizontal = h; }
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override { if (src) src->clipped = false; clip = false; repaint(); }
    // external level source (input meters)
    std::function<std::pair<float, float>()> levelSource;
private:
    void timerCallback() override;
    std::shared_ptr<ChannelProc> src;
    float l = 0.f, r = 0.f, holdL = 0.f, holdR = 0.f;
    int holdTicks = 0;
    bool clip = false, horizontal = false;
};

// Draws a min/max overview of a sample.
void drawWaveform(juce::Graphics& g, const SampleData& s, juce::Rectangle<float> area, juce::Colour c,
                  double startSec = 0.0, double lengthSec = -1.0, bool reverse = false);

// Multi-touch on-screen keyboard. Velocity from touch height on the key; glissando by sliding.
class Keyboard : public juce::Component {
public:
    Keyboard();
    void setRange(int lowestNote, int numWhiteKeys);
    void shiftOctave(int delta);
    int lowest() const { return lowNote; }
    void setScaleHighlight(std::function<bool(int)> f) { inScale = std::move(f); repaint(); }
    std::function<void(int note, float vel)> onNoteOn;
    std::function<void(int note)> onNoteOff;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void allOff();
    float fixedVelocity = -1.f;    // >0 overrides touch-position velocity
private:
    int noteAt(juce::Point<float> p, float* vel) const;
    juce::Rectangle<float> keyRect(int note) const;
    bool isBlack(int n) const { const int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }
    int lowNote = 48, whiteKeys = 15;
    std::map<int, int> touchNote;   // input source index -> note
    std::set<int> down;
    std::function<bool(int)> inScale;
};

// Pop-over editor for a parameter set (drum pad shaping etc.), with an optional audition button.
void showParamSheet(const juce::String& title, ParamSetPtr ps, juce::Component* target, const juce::String& trackId,
                    std::function<void()> audition = nullptr);

// Section heading text inside screens.
void heading(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, float size = 17.f);

// --- dialogs (async, touch friendly)
void askText(const juce::String& title, const juce::String& message, const juce::String& initial, const juce::String& okText,
             std::function<void(const juce::String&)> onOk);
void confirm(const juce::String& title, const juce::String& message, const juce::String& okText, std::function<void()> onOk, bool destructive = false);
void info(const juce::String& title, const juce::String& message);
void showMenu(juce::PopupMenu& m, juce::Component* target, std::function<void(int)> onResult);

// Track helpers for consistent naming in the UI.
juce::String trackTypeName(TrackType t);
juce::String trackIcon(TrackType t);
juce::String formatDuration(double seconds);

// Long-press detection helper for custom components.
class LongPress : private juce::Timer {
public:
    std::function<void(juce::Point<float>)> fire;
    void down(juce::Point<float> p) { pos = p; fired = false; startTimer(480); }
    void moved(juce::Point<float> p) { if (p.getDistanceFrom(pos) > 10.f) stopTimer(); }
    void up() { stopTimer(); }
    bool didFire() const { return fired; }
    void cancel() { stopTimer(); }
private:
    void timerCallback() override { stopTimer(); fired = true; if (fire) fire(pos); }
    juce::Point<float> pos;
    bool fired = false;
};

// Tracks two-finger pinch on a component: returns scale factors since gesture start.
class Pinch {
public:
    bool active() const { return points.size() >= 2; }
    void down(const juce::MouseEvent& e);
    bool drag(const juce::MouseEvent& e, float& scaleX, float& scaleY, juce::Point<float>& centre);
    void up(const juce::MouseEvent& e);
private:
    std::map<int, juce::Point<float>> points, start;
};

}  // namespace UI
