#include "Screens.h"
#include "Core/Paths.h"
#include "Core/Settings.h"
#include "Core/Theory.h"
#include "Engine/Catalog.h"
#include <complex>

using namespace UI;

namespace {

juce::File userPresetDir(const juce::String& engine) { auto d = Paths::presets().getChildFile("Instruments").getChildFile(engine); d.createDirectory(); return d; }

// Touch strip for pitch bend (springs back) or mod wheel (stays).
class TouchStrip : public juce::Component {
public:
    TouchStrip(const juce::String& n, bool spring) : name(n), springBack(spring) { setTitle(n); }
    std::function<void(float)> onChange;
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        auto r = getLocalBounds().toFloat().reduced(2.f);
        g.setColour(p.raised);
        g.fillRoundedRectangle(r, rControl);
        const float y = r.getBottom() - (springBack ? (value * 0.5f + 0.5f) : value) * r.getHeight();
        g.setColour(p.accent);
        g.fillRoundedRectangle(r.getX() + 4.f, y - 4.f, r.getWidth() - 8.f, 8.f, 4.f);
        g.setColour(p.dim);
        g.setFont(font(10.5f));
        g.drawFittedText(name, r.toNearestInt().removeFromBottom(16), juce::Justification::centred, 1);
    }
    void mouseDown(const juce::MouseEvent& e) override { mouseDrag(e); }
    void mouseDrag(const juce::MouseEvent& e) override {
        auto r = getLocalBounds().toFloat().reduced(2.f);
        const float t = juce::jlimit(0.f, 1.f, (r.getBottom() - e.position.y) / r.getHeight());
        value = springBack ? t * 2.f - 1.f : t;
        if (onChange) onChange(value);
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override { if (springBack) { value = 0.f; if (onChange) onChange(0.f); repaint(); } }
private:
    juce::String name;
    bool springBack;
    float value = 0.f;
};

class PresetList : public juce::Component {
public:
    struct Item { juce::String name, engine, category; juce::File userFile; };
    std::vector<Item> items;
    juce::String current;
    std::function<void(const Item&)> onPick;
    std::function<void(const Item&, juce::Component*)> onMenu;
    static constexpr int rowH = 48;
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        auto favs = AppSettings::get().favoriteInstruments;
        for (int i = 0; i < (int)items.size(); ++i) {
            auto r = juce::Rectangle<int>(0, i * rowH, getWidth(), rowH).reduced(2, 2).toFloat();
            const bool sel = items[(size_t)i].name == current;
            g.setColour(sel ? p.raised : p.surface);
            g.fillRoundedRectangle(r, rClip);
            if (sel) { g.setColour(p.accent); g.fillRoundedRectangle(r.withWidth(4.f), 2.f); }
            auto t = r.reduced(14.f, 4.f);
            const bool fav = favs.contains(items[(size_t)i].name);
            drawIcon(g, fav ? "starF" : "star", t.removeFromRight(28.f).withSizeKeepingCentre(18.f, 18.f), fav ? p.accent : p.faint);
            g.setColour(p.text);
            g.setFont(font(15.f, sel));
            g.drawFittedText(items[(size_t)i].name, t.removeFromTop(t.getHeight() * 0.58f).toNearestInt(), juce::Justification::bottomLeft, 1);
            g.setColour(p.dim);
            g.setFont(font(12.f));
            g.drawFittedText(Catalog::engineName(items[(size_t)i].engine) + (items[(size_t)i].userFile.existsAsFile() ? ", my preset" : ""), t.toNearestInt(), juce::Justification::topLeft, 1);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override { downIdx = e.y / rowH; lp.fire = [this](juce::Point<float>) { haptic(1); if (onMenu && downIdx < (int)items.size()) onMenu(items[(size_t)downIdx], this); }; lp.down(e.position); }
    void mouseDrag(const juce::MouseEvent& e) override { lp.moved(e.position); }
    void mouseUp(const juce::MouseEvent& e) override {
        lp.up();
        if (lp.didFire() || e.getDistanceFromDragStart() > 8) return;
        const int i = e.y / rowH;
        if (i < 0 || i >= (int)items.size()) return;
        if (e.x > getWidth() - 50) {
            auto& favs = AppSettings::get().favoriteInstruments;
            if (favs.contains(items[(size_t)i].name)) favs.removeString(items[(size_t)i].name); else favs.add(items[(size_t)i].name);
            AppSettings::get().save();
            repaint();
            return;
        }
        if (onPick) onPick(items[(size_t)i]);
    }
private:
    LongPress lp;
    int downIdx = -1;
};


// Studio Synth editor sections (tabs) -> parameter groups.
const juce::StringArray& studioTabs() { static const juce::StringArray t { "Oscillators", "Filters", "Envelopes", "LFOs", "Mod Matrix", "Macros & Voice" }; return t; }
juce::StringArray studioGroups(int tab) {
    switch (tab) {
        case 0: return { "Osc 1", "Osc 2", "Osc 3", "Osc Interaction", "Sub & Noise" };
        case 1: return { "Filter 1", "Filter 2", "Filter Routing" };
        case 2: return { "Amp Envelope", "Envelope 2", "Envelope 3" };
        case 3: return { "LFO 1", "LFO 2", "LFO 3" };
        case 4: return { "Mod 1", "Mod 2", "Mod 3", "Mod 4", "Mod 5", "Mod 6", "Mod 7", "Mod 8" };
        default: return { "Macros", "Voice", "Output", "Shape" };
    }
}

// Live picture of the sound: output scope, plus the filter response or the envelopes.
class SynthView : public juce::Component, private juce::Timer {
public:
    SynthView() { startTimerHz(30); }
    juce::String trackId;
    ParamSetPtr params;
    bool studio = false;
    int tab = 0;
    void timerCallback() override { if (isShowing()) repaint(); }
    float v(const char* id) const { const int i = params ? params->indexOf(id) : -1; return i >= 0 ? params->get(i) : 0.f; }
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        auto r = getLocalBounds().toFloat();
        g.setColour(p.surface);
        g.fillRoundedRectangle(r, rControl);
        r = r.reduced(8.f, 6.f);
        const bool extra = studio && params && (tab == 1 || tab == 2);
        auto scopeR = extra ? r.removeFromLeft(r.getWidth() * 0.42f) : r;
        if (extra) r.removeFromLeft(10.f);
        drawScope(g, scopeR);
        if (extra) { if (tab == 1) drawFilter(g, r); else drawEnvelopes(g, r); }
    }
    void drawScope(juce::Graphics& g, juce::Rectangle<float> r) {
        const auto& p = pal();
        g.setColour(p.line);
        g.drawHorizontalLine((int)r.getCentreY(), r.getX(), r.getRight());
        auto ch = ctx().engine ? ctx().engine->channelFor(trackId) : nullptr;
        g.setColour(p.dim); g.setFont(font(11.f));
        g.drawText("Output", r.removeFromTop(14.f).toNearestInt(), juce::Justification::topLeft);
        if (!ch) return;
        constexpr int N = 2048, show = 600;
        float buf[N];
        const int pos = ch->scopePos.load(std::memory_order_acquire);
        for (int i = 0; i < N; ++i) buf[i] = ch->scope[(size_t)((pos + i) & (N - 1))].load(std::memory_order_relaxed);
        int start = N - show - 1;   // trigger on a rising zero crossing so the wave stands still
        for (int i = N - show - 2; i > N / 4; --i) if (buf[i] <= 0.f && buf[i + 1] > 0.f) { start = i; break; }
        float peak = 0.08f;
        for (int i = 0; i < show; ++i) peak = std::max(peak, std::fabs(buf[start + i]));
        juce::Path path;
        for (int i = 0; i < show; ++i) {
            const float x = r.getX() + r.getWidth() * (float)i / (float)(show - 1);
            const float y = r.getCentreY() - buf[start + i] / peak * r.getHeight() * 0.45f;
            if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
        }
        g.setColour(p.accent);
        g.strokePath(path, juce::PathStrokeType(1.6f));
    }
    static float filterMag(int type, float f, float fc, float res, float sr) {
        using C = std::complex<float>;
        const float q = 0.6f + res * res * 18.f;
        auto bi = [](float x, float Q, int kind) {   // kind 0 lp 1 hp 2 bp
            const C s(0.f, x);
            const C den = s * s + s / Q + 1.f;
            return kind == 0 ? C(1.f) / den : (kind == 1 ? s * s / den : (s / Q) / den);
        };
        const float x = f / std::max(fc, 1.f);
        switch (type) {
            case 0: return std::abs(bi(x, q, 0));
            case 1: return std::abs(bi(x, 0.54f + res * res * 12.f, 0) * bi(x, 0.54f, 0));
            case 2: return std::abs(bi(x, q, 1));
            case 3: return std::abs(bi(x, 0.54f + res * res * 12.f, 1) * bi(x, 0.54f, 1));
            case 4: return std::abs(bi(x, q, 2));
            case 5: return std::abs(bi(x, q, 0) + bi(x, q, 1));
            case 6: return std::abs(C(1.f) + bi(x, q, 2) * (res * 4.f));
            case 7: { const float g = res * 0.97f, w = dsp::kTwoPi * f / sr, D = sr / fc; return std::abs(C(1.f) / (C(1.f) - g * 0.85f * std::exp(C(0.f, -w * D)))) * (1.f - g * 0.6f); }
            default: {
                static const float F[5][3] = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 350, 1700, 2700 }, { 450, 800, 2830 }, { 325, 700, 2530 } };
                const float vx = juce::jlimit(0.f, 1.f, std::log2(fc / 20.f) / std::log2(1000.f)) * 4.f;
                const int a = std::min(3, (int)vx); const float fr = vx - (float)a;
                C sum(0.f);
                const float gains[3] = { 1.f, 0.5f, 0.25f };
                for (int k = 0; k < 3; ++k) sum += bi(f / dsp::lerp(F[a][k], F[a + 1][k], fr), (4.f + res * 16.f) * (1.f + 0.5f * k), 2) * gains[k];
                return std::abs(sum) * 1.6f;
            }
        }
    }
    void drawFilter(juce::Graphics& g, juce::Rectangle<float> r) {
        const auto& p = pal();
        g.setColour(p.dim); g.setFont(font(11.f));
        g.drawText("Filters (before envelope and modulation)", r.removeFromTop(14.f).toNearestInt(), juce::Justification::topLeft);
        const float sr = 48000.f;
        for (int k = 0; k < 2; ++k) {
            if (k == 1 && v("f2_on") < 0.5f) continue;
            const juce::String id = "f" + juce::String(k + 1);
            const int type = (int)v((id + "_type").toRawUTF8());
            const float fc = v((id + "_cut").toRawUTF8()), res = v((id + "_res").toRawUTF8()) * 0.01f;
            juce::Path path;
            for (int i = 0; i <= 200; ++i) {
                const float f = 20.f * std::pow(1000.f, (float)i / 200.f);
                const float db = juce::jlimit(-36.f, 24.f, dsp::lin2db(filterMag(type, f, fc, res, sr)));
                const float x = r.getX() + r.getWidth() * (float)i / 200.f;
                const float y = r.getY() + r.getHeight() * (1.f - (db + 36.f) / 60.f);
                if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
            }
            g.setColour(k == 0 ? p.accent : p.play);
            g.strokePath(path, juce::PathStrokeType(k == 0 ? 2.f : 1.5f));
        }
        g.setColour(p.line);
        const float y0 = r.getY() + r.getHeight() * (1.f - 36.f / 60.f);
        g.drawHorizontalLine((int)y0, r.getX(), r.getRight());
    }
    void drawEnvelopes(juce::Graphics& g, juce::Rectangle<float> r) {
        const auto& p = pal();
        g.setColour(p.dim); g.setFont(font(11.f));
        g.drawText("Amp, Envelope 2, Envelope 3", r.removeFromTop(14.f).toNearestInt(), juce::Justification::topLeft);
        const char* ids[3] = { "a", "e2", "e3" };
        const juce::Colour cols[3] = { p.accent, p.play, p.warn };
        float total = 0.f;
        for (auto* e : ids) total = std::max(total, v((juce::String(e) + "_a").toRawUTF8()) + v((juce::String(e) + "_d").toRawUTF8()) + v((juce::String(e) + "_r").toRawUTF8()));
        total = std::max(total * 1.3f, 0.1f);
        for (int k = 0; k < 3; ++k) {
            const juce::String e(ids[k]);
            const float a = v((e + "_a").toRawUTF8()), d = v((e + "_d").toRawUTF8()), s = v((e + "_s").toRawUTF8()) * 0.01f, rl = v((e + "_r").toRawUTF8());
            const float hold = total * 0.23f;
            auto X = [&](float t) { return r.getX() + r.getWidth() * t / (total + hold); };
            auto Y = [&](float lv) { return r.getBottom() - r.getHeight() * lv * 0.95f; };
            juce::Path path;
            path.startNewSubPath(X(0), Y(0));
            path.lineTo(X(a), Y(1));
            path.quadraticTo(X(a + d * 0.3f), Y(s), X(a + d), Y(s));
            path.lineTo(X(a + d + hold), Y(s));
            path.quadraticTo(X(a + d + hold + rl * 0.3f), Y(0), X(a + d + hold + rl), Y(0));
            g.setColour(cols[k]);
            g.strokePath(path, juce::PathStrokeType(k == 0 ? 2.f : 1.4f));
        }
    }
};

class InstrumentScreen : public Screen, private juce::KeyListener {
public:
    InstrumentScreen() {
        for (auto* c : { &catAll, &catFav, &catSyn, &catKeys, &catInst, &catDrums, &catSamp, &catMine }) {
            addAndMakeVisible(c);
            c->setRadioGroupId(3);
            c->onClick = [this] { refreshList(); };
        }
        catAll.setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(listView);
        listView.setViewedComponent(&list, false);
        listView.setScrollBarsShown(true, false);
        listView.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
        list.onPick = [this](const PresetList::Item& it) { load(it); };
        list.onMenu = [this](const PresetList::Item& it, juce::Component* c) { presetMenu(it, c); };
        addAndMakeVisible(editView);
        editView.setViewedComponent(&editContent, false);
        editView.setScrollBarsShown(true, false);
        editView.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
        editContent.addAndMakeVisible(panel);
        editContent.addAndMakeVisible(arpPanel);
        editContent.addAndMakeVisible(sampleB);
        editContent.addAndMakeVisible(rootB);
        sampleB.onClick = [this] { ctx().go(ScreenId::Samples); ctx().toast("Pick a sample, then choose \"Load into sampler\""); };
        rootB.onClick = [this] { rootMenu(); };
        for (auto* b : std::initializer_list<juce::Component*> { &saveB, &newTrackB, &browseB, &octDown, &octUp, &velB }) addAndMakeVisible(b);
        saveB.onClick = [this] { savePreset(); };
        newTrackB.onClick = [this] { ScreenUtil::addTrackMenu(session(), &newTrackB); };
        browseB.setClickingTogglesState(true);
        browseB.onClick = [this] { resized(); };
        octDown.onClick = [this] { keys.shiftOctave(-1); };
        octUp.onClick = [this] { keys.shiftOctave(1); };
        velB.setClickingTogglesState(true);
        velB.onClick = [this] { keys.fixedVelocity = velB.getToggleState() ? 0.85f : -1.f; ctx().toast(velB.getToggleState() ? "Fixed velocity" : "Velocity follows where you touch each key"); };
        for (int i = 0; i < 6; ++i) {
            auto* c = tabs.add(new Chip(studioTabs()[i]));
            addChildComponent(c);
            c->setRadioGroupId(5);
            c->setClickingTogglesState(true);
            c->onClick = [this, i] { tab = i; lastSig.clear(); refresh(); editView.setViewPosition(0, 0); };
        }
        tabs[0]->setToggleState(true, juce::dontSendNotification);
        addChildComponent(synthView);
        addAndMakeVisible(designB);
        designB.onClick = [this] { designMenu(); };
        addAndMakeVisible(keys);
        keys.setRange(48, 15);
        keys.onNoteOn = [this](int n, float v) { if (auto* t = session().track()) engine().liveNoteOn(t->id, n, v); };
        keys.onNoteOff = [this](int n) { if (auto* t = session().track()) engine().liveNoteOff(t->id, n); };
        keys.setScaleHighlight([this](int n) { return Theory::inScale(proj(), n); });
        addAndMakeVisible(bendS); addAndMakeVisible(modS);
        bendS.onChange = [this](float v) { if (auto* t = session().track()) engine().liveControl(t->id, LiveEvent::PitchBend, v); };
        modS.onChange = [this](float v) { if (auto* t = session().track()) engine().liveControl(t->id, LiveEvent::ModWheel, v); };
        addKeyListener(this);
        setWantsKeyboardFocus(false);
    }
    juce::String title() const override { return "Instruments"; }
    void onShow() override { refresh(); }
    void onHide() override { keys.allOff(); }

    void refresh() override {
        Track* t = session().track();
        const bool studio = t && t->inst.engine == "studio";
        const auto sig = t ? t->id + t->inst.engine + juce::String((juce::pointer_sized_int)t->inst.params.get()) + t->inst.sample + juce::String(studio ? tab : -1) : juce::String();
        if (sig != lastSig) {
            lastSig = sig;
            if (t && t->hasInstrument()) {
                const auto prefix = "inst:";
                panel.setParams(t->inst.params, prefix, t->id, t->colour, {}, studio ? studioGroups(tab) : juce::StringArray());
                const bool showArp = t->type != TrackType::Drum && (!studio || tab == 5);
                arpPanel.setParams(showArp ? t->inst.arp : nullptr, {}, t->id, t->colour);
            } else { panel.setParams(nullptr, {}, {}); arpPanel.setParams(nullptr, {}, {}); }
        }
        for (auto* c : tabs) c->setVisible(studio);
        synthView.setVisible(t && t->hasInstrument() && t->type != TrackType::Drum);
        synthView.studio = studio; synthView.tab = tab;
        synthView.trackId = t ? t->id : juce::String();
        synthView.params = t ? t->inst.params : nullptr;
        designB.setVisible(t && t->hasInstrument() && t->type != TrackType::Drum);
        const bool sampler = t && t->inst.engine == "sampler";
        sampleB.setVisible(sampler);
        rootB.setVisible(sampler);
        if (sampler) {
            sampleB.setButtonText(t->inst.sample.isEmpty() ? juce::String("Choose a sample...") : "Sample: " + SampleBank::get().resolve(t->inst.sample).getFileNameWithoutExtension());
            rootB.setButtonText("Root note " + Theory::noteName(t->inst.sampleRoot));
        }
        list.current = t ? t->inst.preset : juce::String();
        refreshList();
        resized();
        repaint();
    }

    void refreshList() {
        list.items.clear();
        const auto favs = AppSettings::get().favoriteInstruments;
        auto want = [this, &favs](const juce::String& cat, const juce::String& name) {
            if (catAll.getToggleState()) return true;
            if (catFav.getToggleState()) return favs.contains(name);
            if (catSyn.getToggleState()) return cat == "Synths";
            if (catKeys.getToggleState()) return cat == "Keys";
            if (catInst.getToggleState()) return cat == "Instruments";
            if (catDrums.getToggleState()) return cat == "Drums";
            if (catSamp.getToggleState()) return cat == "Sampler";
            return false;
        };
        for (auto& p : Catalog::instrumentPresets()) if (want(p.category, p.name)) list.items.push_back({ p.name, p.engine, p.category, {} });
        if (catAll.getToggleState() || catMine.getToggleState() || catFav.getToggleState())
            for (auto& e : Catalog::engines())
                for (auto& f : userPresetDir(e.id).findChildFiles(juce::File::findFiles, false, "*.json"))
                    if (!catFav.getToggleState() || favs.contains(f.getFileNameWithoutExtension())) list.items.push_back({ f.getFileNameWithoutExtension(), e.id, "Mine", f });
        list.setSize(juce::jmax(100, listView.getWidth() - 8), (int)list.items.size() * PresetList::rowH + 8);
        list.repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(pal().bg);
        Track* t = session().track();
        auto head = headerArea();
        if (!t || !t->hasInstrument()) {
            heading(g, "No instrument track selected", head, 18.f);
            g.setColour(pal().dim); g.setFont(font(14.f));
            g.drawFittedText("Pick a sound from the list to create a new track with it, or select an instrument track in the Song view.", editView.getBounds().reduced(10), juce::Justification::topLeft, 4);
            return;
        }
        g.setColour(t->colour);
        g.fillRoundedRectangle(head.removeFromLeft(6).toFloat(), 3.f);
        head.removeFromLeft(10);
        g.setColour(pal().text);
        g.setFont(font(20.f, true));
        g.drawText(t->inst.preset.isNotEmpty() ? t->inst.preset : Catalog::engineName(t->inst.engine), head.removeFromTop(26), juce::Justification::centredLeft);
        g.setColour(pal().dim);
        g.setFont(font(13.f));
        g.drawText(t->name + ", " + Catalog::engineName(t->inst.engine), head, juce::Justification::centredLeft);
    }
    juce::Rectangle<int> headerArea() const { return headerRect; }

    void resized() override {
        auto r = getLocalBounds().reduced(8, 6);
        const bool narrow = r.getWidth() < 700;
        // keyboard area
        auto kb = r.removeFromBottom(r.getHeight() < 420 ? 104 : (narrow ? 150 : 170));
        auto kbTools = kb.removeFromLeft(narrow ? 96 : 120);
        auto ktop = kbTools.removeFromTop(44);
        octDown.setBounds(ktop.removeFromLeft(ktop.getWidth() / 2).reduced(1));
        octUp.setBounds(ktop.reduced(1));
        velB.setBounds(kbTools.removeFromTop(40).reduced(1));
        bendS.setBounds(kbTools.removeFromLeft(kbTools.getWidth() / 2).reduced(1, 4));
        modS.setBounds(kbTools.reduced(1, 4));
        keys.setBounds(kb.withTrimmedLeft(6));
        keys.setRange(keys.lowest(), juce::jmax(10, kb.getWidth() / (narrow ? 42 : 48)));
        r.removeFromBottom(8);
        // browser
        const bool showBrowser = !narrow || browseB.getToggleState();
        auto top = r.removeFromTop(50);
        browseB.setVisible(narrow);
        if (narrow) browseB.setBounds(top.removeFromLeft(110).reduced(0, 3));
        newTrackB.setBounds(top.removeFromRight(narrow ? touch : 150).reduced(0, 3));
        newTrackB.setCaption(narrow ? juce::String() : "New track");
        saveB.setBounds(top.removeFromRight(narrow ? touch : 160).reduced(4, 3));
        saveB.setCaption(narrow ? juce::String() : "Save preset");
        designB.setBounds(top.removeFromRight(narrow ? touch : 150).reduced(4, 3));
        designB.setCaption(narrow ? juce::String() : "Randomize");
        headerRect = top.reduced(6, 0);
        juce::Rectangle<int> browser;
        if (showBrowser) browser = narrow ? r : r.removeFromLeft(juce::jmin(320, r.getWidth() / 3));
        for (auto* c : std::initializer_list<juce::Component*> { &catAll, &catFav, &catSyn, &catKeys, &catInst, &catDrums, &catSamp, &catMine, &listView }) c->setVisible(showBrowser);
        editView.setVisible(!narrow || !showBrowser);
        if (showBrowser) {
            auto chips = browser.removeFromTop(narrow ? 84 : 120);
            const int perRow = narrow ? 4 : 3;
            const int cw = chips.getWidth() / perRow;
            int i = 0;
            for (auto* c : { &catAll, &catFav, &catSyn, &catKeys, &catInst, &catDrums, &catSamp, &catMine }) {
                c->setBounds(chips.getX() + (i % perRow) * cw, chips.getY() + (i / perRow) * 40, cw, 38);
                ++i;
            }
            listView.setBounds(browser.withTrimmedTop(4));
            list.setSize(juce::jmax(100, listView.getWidth() - 8), (int)list.items.size() * PresetList::rowH + 8);
            if (!narrow) r.removeFromLeft(10);
        }
        if (editView.isVisible()) {
            if (synthView.isVisible()) {
                synthView.setBounds(r.removeFromTop(r.getHeight() < 260 ? 70 : 110));
                r.removeFromTop(6);
            }
            if (tabs[0]->isVisible()) {
                auto tr = r.removeFromTop(r.getWidth() < 520 ? 80 : 42);
                const int perRow = r.getWidth() < 520 ? 3 : 6;
                const int cw = tr.getWidth() / perRow;
                for (int i = 0; i < 6; ++i) tabs[i]->setBounds(tr.getX() + (i % perRow) * cw, tr.getY() + (i / perRow) * 40, cw, 38);
                r.removeFromTop(4);
            }
            editView.setBounds(r);
            const int w = r.getWidth() - 10;
            int y = 0;
            if (sampleB.isVisible()) { sampleB.setBounds(0, 0, w * 2 / 3 - 4, 44); rootB.setBounds(w * 2 / 3, 0, w / 3, 44); y = 54; }
            const int h1 = panel.preferredHeight(w);
            panel.setBounds(0, y, w, h1);
            y += h1;
            const int h2 = arpPanel.preferredHeight(w);
            arpPanel.setBounds(0, y, w, h2);
            editContent.setSize(w, y + h2 + 20);
        }
    }

    // computer keyboard as a piano: A W S E D F T G Y H U J K, Z/X octave
    bool keyPressed(const juce::KeyPress& k, juce::Component*) override { return handleKey(k); }
    bool handleKey(const juce::KeyPress& k) override {
        if (k.getModifiers().isCommandDown()) return false;
        const int code = juce::CharacterFunctions::toLowerCase((juce::juce_wchar)k.getKeyCode());
        if (code == 'z') { keys.shiftOctave(-1); return true; }
        if (code == 'x') { keys.shiftOctave(1); return true; }
        static const juce::String layout = "awsedftgyhujkolp";
        const int idx = layout.indexOfChar((juce::juce_wchar)code);
        if (idx < 0) return false;
        const int note = keys.lowest() + (keys.lowest() % 12 == 0 ? 0 : 12 - keys.lowest() % 12) + idx;
        if (heldKeys.count(code)) return true;
        heldKeys.insert(code);
        if (auto* t = session().track()) engine().liveNoteOn(t->id, note, 0.8f);
        pollRelease(code, note);
        return true;
    }
    void pollRelease(int code, int note) {
        juce::Timer::callAfterDelay(30, [this, code, note, sp = juce::Component::SafePointer<juce::Component>(this)] {
            if (!sp) return;
            if (juce::KeyPress::isKeyCurrentlyDown(code) || juce::KeyPress::isKeyCurrentlyDown(juce::CharacterFunctions::toUpperCase((juce::juce_wchar)code))) { pollRelease(code, note); return; }
            heldKeys.erase(code);
            if (auto* t = session().track()) engine().liveNoteOff(t->id, note);
        });
    }

private:
    void designMenu() {
        Track* t = session().track();
        if (!t || !t->hasInstrument() || t->type == TrackType::Drum) return;
        juce::PopupMenu m;
        m.addItem(1, "Randomize: a brand new sound");
        m.addItem(2, "Mutate: small changes");
        m.addItem(3, "Mutate: bigger changes");
        m.addSeparator();
        m.addItem(4, "Start from scratch (Init)");
        showMenu(m, &designB, [this](int r) {
            Track* tt = session().track();
            if (!tt || r <= 0) return;
            session().checkpoint(r == 4 ? "Init sound" : (r == 1 ? "Randomize sound" : "Mutate sound"));
            const auto seed = (uint32_t)juce::Random::getSystemRandom().nextInt();
            if (r == 4) tt->inst.params->resetToDefaults();
            else Catalog::randomizeSound(*tt->inst.params, tt->inst.engine, r == 1 ? 1.f : (r == 2 ? 0.06f : 0.18f), seed);
            tt->inst.preset = r == 4 ? juce::String("Init") : (r == 1 ? "Random " + juce::String::toHexString((int)(seed & 0xffff)) : tt->inst.preset);
            session().changed();
            panel.repaint();
            repaint();
            engine().liveNoteOn(tt->id, 60, 0.8f);
            juce::Timer::callAfterDelay(500, [this, id = tt->id] { engine().liveNoteOff(id, 60); });
            if (r != 4) ctx().toast("Undo to go back. Save it with Save preset if you like it");
        });
    }
    void load(const PresetList::Item& it) {
        Track* t = session().track();
        const bool drumPreset = it.engine == "drums";
        if (!t || !t->hasInstrument() || (drumPreset != (t->type == TrackType::Drum))) {
            ScreenUtil::addTrack(session(), drumPreset ? TrackType::Drum : TrackType::Instrument, it.userFile.existsAsFile() ? juce::String() : it.name);
            t = session().track();
            if (it.userFile.existsAsFile() && t) applyUserPreset(*t, it);
            ctx().toast("New track: " + it.name);
            return;
        }
        session().checkpoint("Change sound");
        if (it.userFile.existsAsFile()) applyUserPreset(*t, it);
        else if (drumPreset) {
            for (auto& [name, sounds] : Catalog::drumKits())
                if (name == it.name) for (int i = 0; i < 16; ++i) {
                    t->inst.pads[(size_t)i].sample = "builtin:" + sounds[i];
                    for (auto& d : Catalog::drumSounds()) if (sounds[i] == d.id) t->inst.pads[(size_t)i].name = d.name;
                }
            t->inst.preset = it.name;
        } else {
            const auto* p = Catalog::findPreset(it.name);
            if (t->inst.engine != it.engine) { const auto keepArp = t->inst.arp; proj().initInstrument(t->inst, it.engine); t->inst.arp = keepArp; }
            else t->inst.params->resetToDefaults();
            if (p) Catalog::applyValues(*t->inst.params, p->values);
            t->inst.preset = it.name;
        }
        session().changed(true);
        haptic(0);
        engine().liveNoteOn(t->id, 60, 0.8f);
        juce::Timer::callAfterDelay(400, [this, id = t->id] { engine().liveNoteOff(id, 60); });
    }
    void applyUserPreset(Track& t, const PresetList::Item& it) {
        auto v = juce::JSON::parse(it.userFile);
        if (t.inst.engine != it.engine) { const auto keepArp = t.inst.arp; proj().initInstrument(t.inst, it.engine); t.inst.arp = keepArp; }
        t.inst.params->fromVar(v["params"]);
        if (v.hasProperty("arp")) t.inst.arp->fromVar(v["arp"]);
        t.inst.preset = it.name;
    }
    void savePreset() {
        Track* t = session().track();
        if (!t || !t->hasInstrument()) return;
        askText("Save preset", "Saves this sound so you can use it on any track.", t->inst.preset + " (mine)", "Save", [this](const juce::String& n) {
            Track* tt = session().track();
            if (!tt || n.trim().isEmpty()) return;
            auto* o = new juce::DynamicObject();
            o->setProperty("engine", tt->inst.engine);
            o->setProperty("params", tt->inst.params->toVar());
            o->setProperty("arp", tt->inst.arp->toVar());
            userPresetDir(tt->inst.engine).getChildFile(juce::File::createLegalFileName(n.trim()) + ".json").replaceWithText(juce::JSON::toString(juce::var(o)));
            session().checkpoint("Name preset");
            tt->inst.preset = n.trim();
            session().changed();
            refreshList();
            ctx().toast("Preset saved");
        });
    }
    void presetMenu(const PresetList::Item& it, juce::Component* c) {
        juce::PopupMenu m;
        m.addSectionHeader(it.name);
        m.addItem(1, "Load on this track");
        m.addItem(2, "New track with this sound");
        if (it.userFile.existsAsFile()) { m.addItem(3, "Rename..."); m.addItem(4, "Delete preset..."); }
        showMenu(m, c, [this, it](int r) {
            if (r == 1) load(it);
            else if (r == 2) {
                ScreenUtil::addTrack(session(), it.engine == "drums" ? TrackType::Drum : TrackType::Instrument, it.userFile.existsAsFile() ? juce::String() : it.name);
                if (it.userFile.existsAsFile()) if (auto* t = session().track()) { applyUserPreset(*t, it); session().changed(true); }
            } else if (r == 3) askText("Rename preset", {}, it.name, "Rename", [this, it](const juce::String& n) { it.userFile.moveFileTo(it.userFile.getSiblingFile(juce::File::createLegalFileName(n) + ".json")); refreshList(); });
            else if (r == 4) confirm("Delete \"" + it.name + "\"?", "Tracks already using this sound keep it.", "Delete", [this, it] { it.userFile.deleteFile(); refreshList(); }, true);
        });
    }
    void rootMenu() {
        juce::PopupMenu m;
        for (int n = 36; n <= 84; ++n) m.addItem(n, Theory::noteName(n), true, session().track() && session().track()->inst.sampleRoot == n);
        showMenu(m, &rootB, [this](int r) { if (r > 0) if (auto* t = session().track()) { session().checkpoint("Root note"); t->inst.sampleRoot = r; session().changed(true); } });
    }

    Chip catAll { "All" }, catFav { "Favorites" }, catSyn { "Synths" }, catKeys { "Keys" }, catInst { "Instruments" }, catDrums { "Drums" }, catSamp { "Sampler" }, catMine { "My presets" };
    juce::Viewport listView, editView;
    PresetList list;
    juce::Component editContent;
    ParamPanel panel, arpPanel;
    juce::OwnedArray<Chip> tabs;
    int tab = 0;
    SynthView synthView;
    IconButton designB { "dice", "Randomize", "Randomize or mutate this sound" };
    juce::TextButton sampleB, rootB;
    IconButton saveB { "save", "Save preset" }, newTrackB { "plus", "New track" }, octDown { "back", {}, "Octave down (Z)" }, octUp { "chevron", {}, "Octave up (X)" }, velB { "volume", {}, "Fixed velocity" };
    juce::TextButton browseB { "Sounds" };
    Keyboard keys;
    TouchStrip bendS { "Bend", true }, modS { "Mod", false };
    juce::String lastSig;
    juce::Rectangle<int> headerRect;
    std::set<int> heldKeys;
};

}  // namespace

std::unique_ptr<Screen> makeInstrumentScreen() { return std::make_unique<InstrumentScreen>(); }
