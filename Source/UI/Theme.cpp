#include "Theme.h"
#include "BinaryData.h"

namespace UI {

static Palette darkP {
    juce::Colour(0xff17181b), juce::Colour(0xff1f2125), juce::Colour(0xff2a2d33), juce::Colour(0xff363a41),
    juce::Colour(0xffece8e1), juce::Colour(0xff9a968f), juce::Colour(0xff5d5a55),
    juce::Colour(0xfff2b544), juce::Colour(0xff1d1606), juce::Colour(0xffff5a4e), juce::Colour(0xff5bd69a), juce::Colour(0xffff9f43) };
static Palette lightP {
    juce::Colour(0xffeeede9), juce::Colour(0xffffffff), juce::Colour(0xfff4f3ef), juce::Colour(0xffd6d3cc),
    juce::Colour(0xff1e1f22), juce::Colour(0xff6b6862), juce::Colour(0xffa9a59d),
    juce::Colour(0xffc98a12), juce::Colour(0xffffffff), juce::Colour(0xffe5392d), juce::Colour(0xff1f9d63), juce::Colour(0xffd9740b) };
static bool dark = true;

const Palette& pal() { return dark ? darkP : lightP; }
void setDark(bool d) { dark = d; }
bool isDark() { return dark; }

static juce::Typeface::Ptr& regularTf() {
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor(BinaryData::InterRegular_otf, BinaryData::InterRegular_otfSize);
    return t;
}
static juce::Typeface::Ptr& semiTf() {
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor(BinaryData::InterSemiBold_otf, BinaryData::InterSemiBold_otfSize);
    return t;
}

juce::FontOptions fontOpts(float size, bool sb) { return juce::FontOptions(sb ? semiTf() : regularTf()).withHeight(size); }
juce::Font font(float size, bool sb) { return juce::Font(fontOpts(size, sb)); }

// ---------------------------------------------------------------- icons
// "F:" prefix = filled shape, otherwise stroked. 24x24 grid.
static const std::map<juce::String, juce::String>& iconData() {
    static const std::map<juce::String, juce::String> m = {
        { "play", "F:M7 4.5 L19.5 12 L7 19.5 Z" },
        { "stop", "F:M6 6 H18 V18 H6 Z" },
        { "record", "F:M12 5 A7 7 0 1 1 11.99 5 Z" },
        { "pause", "F:M6.5 5 H10 V19 H6.5 Z M14 5 H17.5 V19 H14 Z" },
        { "loop", "M4 12 V9.5 A3.5 3.5 0 0 1 7.5 6 H18 M15 3 L18 6 L15 9 M20 12 V14.5 A3.5 3.5 0 0 1 16.5 18 H6 M9 21 L6 18 L9 15" },
        { "metronome", "M9 3 H15 L19 21 H5 Z M12 17 L17 6 M8 15 H16" },
        { "undo", "M9 14 L4 9 L9 4 M4 9 H14 A6 6 0 0 1 14 21 H9" },
        { "redo", "M15 14 L20 9 L15 4 M20 9 H10 A6 6 0 0 0 10 21 H15" },
        { "more", "F:M5 10.5 A1.5 1.5 0 1 1 4.99 10.5 Z M12 10.5 A1.5 1.5 0 1 1 11.99 10.5 Z M19 10.5 A1.5 1.5 0 1 1 18.99 10.5 Z" },
        { "home", "M3 11 L12 4 L21 11 M6 9.5 V20 H18 V9.5 M10 20 V14 H14 V20" },
        { "song", "M3 6 H13 M15 6 H21 M3 12 H8 M10 12 H21 M3 18 H15 M17 18 H21" },
        { "keys", "M3 5 H21 V19 H3 Z M7.5 5 V13 M12 5 V13 M16.5 5 V13 M7.5 13 V19 M12 13 V19 M16.5 13 V19" },
        { "drums", "M4 4 H9 V9 H4 Z M10 4 H15 V9 H10 Z M16 4 H21 V9 H16 Z M4 10 H9 V15 H4 Z M10 10 H15 V15 H10 Z M16 10 H21 V15 H16 Z M4 16 H9 V21 H4 Z M10 16 H15 V21 H10 Z M16 16 H21 V21 H16 Z" },
        { "mixer", "M6 3 V21 M12 3 V21 M18 3 V21 M3.5 15 H8.5 M9.5 8 H14.5 M15.5 13 H20.5" },
        { "instrument", "M3 7 H21 V17 H3 Z M7 10 V14 M11 10 V14 M15 10 V14 M7 4 V7 M17 4 V7" },
        { "fx", "M3 12 C6 4 8 4 10 12 C12 20 14 20 17 12 C18.5 8 20 8 21 10" },
        { "samples", "M3 12 H5 M7 8 V16 M10 5 V19 M13 9 V15 M16 7 V17 M19 10 V14 M21 12 H21.5" },
        { "mic", "M12 3 A3 3 0 0 1 15 6 V12 A3 3 0 0 1 9 12 V6 A3 3 0 0 1 12 3 Z M6 11 A6 6 0 0 0 18 11 M12 17 V21 M8 21 H16" },
        { "export", "M12 15 V3 M7 8 L12 3 L17 8 M5 13 V20 H19 V13" },
        { "settings", "M12 8.5 A3.5 3.5 0 1 1 11.99 8.5 Z M12 2.5 V5 M12 19 V21.5 M2.5 12 H5 M19 12 H21.5 M5.3 5.3 L7 7 M17 17 L18.7 18.7 M5.3 18.7 L7 17 M17 7 L18.7 5.3" },
        { "plus", "M12 5 V19 M5 12 H19" },
        { "minus", "M5 12 H19" },
        { "trash", "M4 7 H20 M9 7 V4 H15 V7 M6 7 L7 20 H17 L18 7 M10 11 V16 M14 11 V16" },
        { "copy", "M8 8 H19 V19 H8 Z M5 16 V5 H16" },
        { "split", "M6 4 A2.5 2.5 0 1 1 5.99 4 Z M6 15 A2.5 2.5 0 1 1 5.99 15 Z M8 8 L20 18 M8 16 L20 6" },
        { "search", "M10.5 4 A6.5 6.5 0 1 1 10.49 4 Z M15.5 15.5 L21 21" },
        { "star", "M12 3 L14.6 9 L21 9.5 L16 13.8 L17.6 20.5 L12 17 L6.4 20.5 L8 13.8 L3 9.5 L9.4 9 Z" },
        { "starF", "F:M12 3 L14.6 9 L21 9.5 L16 13.8 L17.6 20.5 L12 17 L6.4 20.5 L8 13.8 L3 9.5 L9.4 9 Z" },
        { "back", "M15 5 L8 12 L15 19" },
        { "chevron", "M9 5 L16 12 L9 19" },
        { "down", "M5 9 L12 16 L19 9" },
        { "close", "M6 6 L18 18 M18 6 L6 18" },
        { "check", "M5 12.5 L10 17.5 L19 7" },
        { "headphones", "M4 15 V12 A8 8 0 0 1 20 12 V15 M4 14 H7 V20 H4 Z M17 14 H20 V20 H17 Z" },
        { "magnet", "M6 3 V12 A6 6 0 0 0 18 12 V3 M6 7 H9.5 M14.5 7 H18 M9.5 3 V12 A2.5 2.5 0 0 0 14.5 12 V3" },
        { "zoomin", "M10.5 4 A6.5 6.5 0 1 1 10.49 4 Z M15.5 15.5 L21 21 M7.5 10.5 H13.5 M10.5 7.5 V13.5" },
        { "zoomout", "M10.5 4 A6.5 6.5 0 1 1 10.49 4 Z M15.5 15.5 L21 21 M7.5 10.5 H13.5" },
        { "pencil", "M4 20 L5 15 L16 4 L20 8 L9 19 Z M13.5 6.5 L17.5 10.5" },
        { "select", "M5 3 L5 18 L9 14 L12 21 L14.5 20 L11.5 13 L17 13 Z" },
        { "save", "M5 4 H16 L20 8 V20 H4 V4 Z M8 4 V9 H15 V4 M7 14 H17 V20 H7 Z" },
        { "volume", "M4 9 H8 L13 5 V19 L8 15 H4 Z M16 9 A4 4 0 0 1 16 15 M18.5 6.5 A7.5 7.5 0 0 1 18.5 17.5" },
        { "help", "M12 3 A9 9 0 1 1 11.99 3 Z M9.5 9.5 A2.5 2.5 0 1 1 13 11.8 C12.3 12.2 12 12.7 12 13.5 M12 16.5 V17" },
        { "folder", "M3 6 H9 L11 8 H21 V19 H3 Z" },
        { "power", "M12 3 V11 M7 6 A7.5 7.5 0 1 0 17 6" },
        { "grip", "F:M9 6 A1.3 1.3 0 1 1 8.99 6 Z M15 6 A1.3 1.3 0 1 1 14.99 6 Z M9 12 A1.3 1.3 0 1 1 8.99 12 Z M15 12 A1.3 1.3 0 1 1 14.99 12 Z M9 18 A1.3 1.3 0 1 1 8.99 18 Z M15 18 A1.3 1.3 0 1 1 14.99 18 Z" },
        { "tap", "M9 11 V5 A1.5 1.5 0 0 1 12 5 V11 M12 10 A1.5 1.5 0 0 1 15 10 V12 M15 11 A1.5 1.5 0 0 1 18 11 V15 A6 6 0 0 1 12 21 A6 6 0 0 1 6.5 17 L4.5 13 A1.4 1.4 0 0 1 7 11.5 L9 14" },
        { "curve", "M3 18 C8 18 9 6 14 6 C17 6 18 10 21 10 M3 18 A1.5 1.5 0 1 1 2.99 18 M14 6 A1.5 1.5 0 1 1 13.99 6" },
        { "flag", "M5 21 V4 M5 4 H17 L14.5 8 L17 12 H5" },
        { "import", "M12 3 V15 M7 10 L12 15 L17 10 M5 13 V20 H19 V13" },
        { "midi", "M12 3 A9 9 0 1 1 11.99 3 Z M7.5 12 A0.7 0.7 0 1 1 7.49 12 M9 8.5 A0.7 0.7 0 1 1 8.99 8.5 M12 7.5 A0.7 0.7 0 1 1 11.99 7.5 M15 8.5 A0.7 0.7 0 1 1 14.99 8.5 M16.5 12 A0.7 0.7 0 1 1 16.49 12 M10.5 17 H13.5" },
        { "share", "M12 15 V3 M7 8 L12 3 L17 8 M5 13 V20 H19 V13" },
        { "wand", "M4 20 L15 9 M13 7 L17 11 M17 3 V6 M15.5 4.5 H18.5 M20 9 V11 M19 10 H21 M10 3 V5 M9 4 H11" },
        { "dice", "M4 4 H20 V20 H4 Z M8.5 8.5 A0.8 0.8 0 1 1 8.49 8.5 M15.5 8.5 A0.8 0.8 0 1 1 15.49 8.5 M12 12 A0.8 0.8 0 1 1 11.99 12 M8.5 15.5 A0.8 0.8 0 1 1 8.49 15.5 M15.5 15.5 A0.8 0.8 0 1 1 15.49 15.5" },
    };
    return m;
}

juce::Path icon(const juce::String& name) {
    auto it = iconData().find(name);
    if (it == iconData().end()) return {};
    auto d = it->second;
    if (d.startsWith("F:")) d = d.substring(2);
    return juce::Drawable::parseSVGPath(d);
}

void drawIcon(juce::Graphics& g, const juce::String& name, juce::Rectangle<float> r, juce::Colour c, float stroke) {
    auto it = iconData().find(name);
    if (it == iconData().end()) return;
    const bool filled = it->second.startsWith("F:");
    auto p = icon(name);
    const float s = juce::jmin(r.getWidth(), r.getHeight()) / 24.f;
    p.applyTransform(juce::AffineTransform::scale(s).translated(r.getCentreX() - 12.f * s, r.getCentreY() - 12.f * s));
    g.setColour(c);
    if (filled) g.fillPath(p);
    else g.strokePath(p, juce::PathStrokeType(stroke * juce::jmax(0.8f, s), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// ---------------------------------------------------------------- look and feel
LookAndFeel::LookAndFeel() {
    regular = regularTf();
    semibold = semiTf();
    refreshColours();
}

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont(const juce::Font& f) {
    return f.isBold() ? semibold : regular;
}

void LookAndFeel::refreshColours() {
    const auto& p = pal();
    setColour(juce::ResizableWindow::backgroundColourId, p.bg);
    setColour(juce::DocumentWindow::backgroundColourId, p.bg);
    setColour(juce::TextButton::buttonColourId, p.raised);
    setColour(juce::TextButton::buttonOnColourId, p.accent);
    setColour(juce::TextButton::textColourOffId, p.text);
    setColour(juce::TextButton::textColourOnId, p.accentText);
    setColour(juce::ToggleButton::textColourId, p.text);
    setColour(juce::ToggleButton::tickColourId, p.accent);
    setColour(juce::Label::textColourId, p.text);
    setColour(juce::Slider::thumbColourId, p.accent);
    setColour(juce::Slider::trackColourId, p.accent);
    setColour(juce::Slider::rotarySliderFillColourId, p.accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, p.line);
    setColour(juce::Slider::textBoxTextColourId, p.text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::backgroundColourId, p.raised);
    setColour(juce::ComboBox::textColourId, p.text);
    setColour(juce::ComboBox::arrowColourId, p.dim);
    setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::PopupMenu::backgroundColourId, p.raised);
    setColour(juce::PopupMenu::textColourId, p.text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, p.line);
    setColour(juce::PopupMenu::highlightedTextColourId, p.text);
    setColour(juce::TextEditor::backgroundColourId, p.raised);
    setColour(juce::TextEditor::textColourId, p.text);
    setColour(juce::TextEditor::highlightColourId, p.accent.withAlpha(0.35f));
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, p.accent);
    setColour(juce::CaretComponent::caretColourId, p.accent);
    setColour(juce::ListBox::backgroundColourId, p.surface);
    setColour(juce::ScrollBar::thumbColourId, p.line);
    setColour(juce::AlertWindow::backgroundColourId, p.surface);
    setColour(juce::AlertWindow::textColourId, p.text);
    setColour(juce::AlertWindow::outlineColourId, p.line);
    setColour(juce::TooltipWindow::backgroundColourId, p.text);
    setColour(juce::TooltipWindow::textColourId, p.bg);
    setColour(juce::TreeView::backgroundColourId, p.surface);
    setColour(juce::FileListComponent::backgroundColourId, p.surface);
    setColour(juce::DirectoryContentsDisplayComponent::textColourId, p.text);
    setColour(juce::DirectoryContentsDisplayComponent::highlightColourId, p.line);
    setColour(juce::GroupComponent::outlineColourId, p.line);
    setColour(juce::GroupComponent::textColourId, p.dim);
    auto scheme = getCurrentColourScheme();
    scheme.setUIColour(ColourScheme::windowBackground, p.surface);
    scheme.setUIColour(ColourScheme::widgetBackground, p.raised);
    scheme.setUIColour(ColourScheme::menuBackground, p.raised);
    scheme.setUIColour(ColourScheme::outline, p.line);
    scheme.setUIColour(ColourScheme::defaultText, p.text);
    scheme.setUIColour(ColourScheme::defaultFill, p.accent);
    scheme.setUIColour(ColourScheme::highlightedText, p.accentText);
    scheme.setUIColour(ColourScheme::highlightedFill, p.accent);
    scheme.setUIColour(ColourScheme::menuText, p.text);
    setColourScheme(scheme);
}

void LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bgc, bool over, bool down) {
    auto r = b.getLocalBounds().toFloat().reduced(1.f);
    auto c = b.getToggleState() ? b.findColour(juce::TextButton::buttonOnColourId) : bgc;
    if (!b.isEnabled()) c = c.withMultipliedAlpha(0.4f);
    if (down) c = c.brighter(0.08f);
    else if (over) c = c.brighter(0.04f);
    g.setColour(c);
    g.fillRoundedRectangle(r, rControl);
}

void LookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) {
    g.setFont(getTextButtonFont(b, b.getHeight()));
    auto c = b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    g.setColour(b.isEnabled() ? c : c.withAlpha(0.4f));
    g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(8, 2), juce::Justification::centred, 2);
}

void LookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool, bool) {
    const auto& p = pal();
    auto r = b.getLocalBounds().toFloat();
    auto sw = r.removeFromRight(46.f).withSizeKeepingCentre(42.f, 26.f);
    g.setColour(b.getToggleState() ? p.accent : p.line);
    g.fillRoundedRectangle(sw, 13.f);
    const float kx = b.getToggleState() ? sw.getRight() - 24.f : sw.getX() + 2.f;
    g.setColour(b.getToggleState() ? p.accentText : p.text);
    g.fillEllipse(kx, sw.getY() + 2.f, 22.f, 22.f);
    g.setColour(b.isEnabled() ? p.text : p.dim);
    g.setFont(font(15.f));
    g.drawFittedText(b.getButtonText(), r.toNearestInt().withTrimmedRight(8), juce::Justification::centredLeft, 2);
}

void LookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) {
    const auto& p = pal();
    auto r = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(4.f);
    const float d = juce::jmin(r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre(d, d);
    const float th = juce::jmax(3.f, d * 0.09f);
    const float ang = a0 + pos * (a1 - a0);
    juce::Path track;
    track.addCentredArc(r.getCentreX(), r.getCentreY(), d * 0.5f - th, d * 0.5f - th, 0.f, a0, a1, true);
    g.setColour(p.line);
    g.strokePath(track, juce::PathStrokeType(th, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float from = bipolar ? (a0 + a1) * 0.5f : a0;
    juce::Path val;
    val.addCentredArc(r.getCentreX(), r.getCentreY(), d * 0.5f - th, d * 0.5f - th, 0.f, juce::jmin(from, ang), juce::jmax(from, ang), true);
    const auto c = s.findColour(juce::Slider::rotarySliderFillColourId);
    g.setColour(s.isEnabled() ? c : p.dim);
    g.strokePath(val, juce::PathStrokeType(th, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // cap
    auto cap = r.reduced(th * 2.2f);
    g.setColour(p.raised);
    g.fillEllipse(cap);
    juce::Path ind;
    ind.addRoundedRectangle(-th * 0.4f, -cap.getHeight() * 0.5f + th * 0.6f, th * 0.8f, cap.getHeight() * 0.32f, th * 0.4f);
    g.setColour(p.text);
    g.fillPath(ind, juce::AffineTransform::rotation(ang).translated(cap.getCentreX(), cap.getCentreY()));
}

void LookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle st, juce::Slider& s) {
    const auto& p = pal();
    auto r = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h);
    const auto c = s.findColour(juce::Slider::trackColourId);
    if (st == juce::Slider::LinearVertical) {
        auto tr = r.withSizeKeepingCentre(6.f, r.getHeight());
        g.setColour(p.line); g.fillRoundedRectangle(tr, 3.f);
        g.setColour(c); g.fillRoundedRectangle(tr.withTop(pos), 3.f);
        auto thumb = juce::Rectangle<float>(r.getCentreX() - 16.f, pos - 9.f, 32.f, 18.f);
        g.setColour(p.text); g.fillRoundedRectangle(thumb, 5.f);
        g.setColour(p.bg); g.fillRect(thumb.withSizeKeepingCentre(18.f, 2.f));
    } else {
        auto tr = r.withSizeKeepingCentre(r.getWidth(), 6.f);
        g.setColour(p.line); g.fillRoundedRectangle(tr, 3.f);
        g.setColour(c); g.fillRoundedRectangle(tr.withRight(pos), 3.f);
        g.setColour(p.text); g.fillEllipse(pos - 10.f, r.getCentreY() - 10.f, 20.f, 20.f);
    }
}

void LookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& b) {
    const auto& p = pal();
    auto r = juce::Rectangle<float>(0, 0, (float)w, (float)h).reduced(1.f);
    g.setColour(b.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(r, rControl);
    drawIcon(g, "down", { (float)w - 26.f, (float)h * 0.5f - 8.f, 16.f, 16.f }, p.dim, 2.f);
}

void LookAndFeel::positionComboBoxText(juce::ComboBox& b, juce::Label& l) {
    l.setBounds(10, 1, b.getWidth() - 36, b.getHeight() - 2);
    l.setFont(font(15.f));
}

void LookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h) {
    const auto& p = pal();
    g.fillAll(p.raised);
    g.setColour(p.line);
    g.drawRect(0, 0, w, h, 1);
}

void LookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool sep, bool active, bool hi, bool ticked, bool sub,
                                    const juce::String& text, const juce::String& shortcut, const juce::Drawable*, const juce::Colour* colour) {
    const auto& p = pal();
    if (sep) { g.setColour(p.line); g.fillRect(area.reduced(12, 0).withHeight(1).withY(area.getCentreY())); return; }
    auto r = area.reduced(4, 1);
    if (hi && active) { g.setColour(p.line); g.fillRoundedRectangle(r.toFloat(), 8.f); }
    auto c = colour ? *colour : p.text;
    if (!active) c = p.faint;
    auto t = r.reduced(12, 0);
    if (ticked) drawIcon(g, "check", t.removeFromLeft(20).toFloat().withSizeKeepingCentre(18, 18), p.accent, 2.2f);
    else t.removeFromLeft(20);
    t.removeFromLeft(4);
    if (sub) drawIcon(g, "chevron", t.removeFromRight(16).toFloat().withSizeKeepingCentre(14, 14), p.dim);
    if (shortcut.isNotEmpty()) { g.setColour(p.dim); g.setFont(font(13.f)); g.drawText(shortcut, t, juce::Justification::centredRight); }
    g.setColour(c);
    g.setFont(font(16.f));
    g.drawFittedText(text, t, juce::Justification::centredLeft, 1);
}

void LookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool sep, int, int& w, int& h) {
    if (sep) { w = 50; h = 9; return; }
    h = 44;
    w = juce::GlyphArrangement::getStringWidthInt(font(16.f), text) + 80;
}

void LookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int ts, int tsize, bool over, bool down) {
    const auto& p = pal();
    auto r = vertical ? juce::Rectangle<int>(x, ts, w, tsize) : juce::Rectangle<int>(ts, y, tsize, h);
    g.setColour((over || down) ? p.dim : p.line);
    g.fillRoundedRectangle(r.reduced(2).toFloat(), 3.f);
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int w, int h, juce::TextEditor& e) {
    g.setColour(e.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(0, 0, (float)w, (float)h, rControl);
}
void LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor& e) {
    if (e.hasKeyboardFocus(true)) { g.setColour(pal().accent); g.drawRoundedRectangle(1, 1, (float)w - 2, (float)h - 2, rControl, 1.5f); }
}

void LookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& a, const juce::Rectangle<int>& textArea, juce::TextLayout& layout) {
    const auto& p = pal();
    g.setColour(p.surface);
    g.fillRoundedRectangle(a.getLocalBounds().toFloat(), rSheet);
    g.setColour(p.line);
    g.drawRoundedRectangle(a.getLocalBounds().toFloat().reduced(0.5f), rSheet, 1.f);
    layout.draw(g, textArea.toFloat());
}

void LookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int w, int h) {
    const auto& p = pal();
    g.setColour(p.text);
    g.fillRoundedRectangle(0, 0, (float)w, (float)h, 8.f);
    g.setColour(p.bg);
    g.setFont(font(14.f));
    g.drawFittedText(text, juce::Rectangle<int>(w, h).reduced(10, 4), juce::Justification::centredLeft, 3);
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds(const juce::String& tip, juce::Point<int> pos, juce::Rectangle<int> parent) {
    const int w = juce::jmin(320, juce::GlyphArrangement::getStringWidthInt(font(14.f), tip) + 24);
    const int h = w >= 320 ? 52 : 32;
    return juce::Rectangle<int>(pos.x > parent.getCentreX() ? pos.x - w - 8 : pos.x + 16, pos.y + 18, w, h).constrainedWithin(parent);
}

void LookAndFeel::drawLabel(juce::Graphics& g, juce::Label& l) {
    if (l.isBeingEdited()) return;
    g.setColour(l.findColour(juce::Label::textColourId).withMultipliedAlpha(l.isEnabled() ? 1.f : 0.5f));
    g.setFont(l.getFont());
    g.drawFittedText(l.getText(), l.getLocalBounds().reduced(2, 0), l.getJustificationType(), juce::jmax(1, l.getHeight() / 16), 0.9f);
}

}  // namespace UI
