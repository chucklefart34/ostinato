// Design system: palette (dark "console at night" + light), type scale, metrics, icons, LookAndFeel.
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>

namespace UI {

struct Palette {
    juce::Colour bg, surface, raised, line, text, dim, faint, accent, accentText, record, play, warn;
};
const Palette& pal();
void setDark(bool dark);
bool isDark();

// Metrics (logical pixels). Touch targets never go below `touch`.
constexpr int touch = 44;
constexpr int gap = 8;
constexpr float rClip = 6.f, rControl = 10.f, rSheet = 16.f;

juce::Font font(float size, bool semibold = false);
juce::FontOptions fontOpts(float size, bool semibold = false);

// Vector icons (24x24 design grid).
juce::Path icon(const juce::String& name);
void drawIcon(juce::Graphics& g, const juce::String& name, juce::Rectangle<float> r, juce::Colour c, float stroke = 1.8f);

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    LookAndFeel();
    void refreshColours();
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& f) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override { return font(15.f); }
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getTextButtonFont(juce::TextButton& b, int) override { return font(b.getWidth() < 56 ? 12.5f : 15.f, true); }
    juce::Font getPopupMenuFont() override { return font(16.f); }
    void drawPopupMenuBackground(juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>&, bool sep, bool active, bool highlighted, bool ticked, bool sub,
                           const juce::String& text, const juce::String& shortcut, const juce::Drawable* icon, const juce::Colour* colour) override;
    void getIdealPopupMenuItemSize(const juce::String& text, bool sep, int standardHeight, int& w, int& h) override;
    int getPopupMenuBorderSize() override { return 6; }
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart, int thumbSize, bool over, bool down) override;
    int getDefaultScrollbarWidth() override { return 8; }
    void fillTextEditorBackground(juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int w, int h, juce::TextEditor&) override;
    juce::Font getAlertWindowTitleFont() override { return font(19.f, true); }
    juce::Font getAlertWindowMessageFont() override { return font(15.f); }
    juce::Font getAlertWindowFont() override { return font(15.f); }
    void drawAlertBox(juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea, juce::TextLayout&) override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int w, int h) override;
    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawLabel(juce::Graphics&, juce::Label&) override;
private:
    juce::Typeface::Ptr regular, semibold;
};

}  // namespace UI
