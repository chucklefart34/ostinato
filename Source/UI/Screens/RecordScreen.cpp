// Record screen: the "booth". Arm tracks, then for the focused audio track set the input gain,
// how you hear yourself (off / interface direct monitoring / through the effects), what gets
// recorded (clean or with effects), and build the live vocal / instrument chain from presets.
#include "Screens.h"
#include "UI/FxChainEditor.h"
#include "Core/Settings.h"
#include "Core/Theory.h"
#include "Engine/Catalog.h"

using namespace UI;

namespace {

juce::String inputLabel(const Track& t) {
    return t.stereoInput ? "In " + juce::String(t.inputChannel + 1) + "+" + juce::String(t.inputChannel + 2) : "In " + juce::String(t.inputChannel + 1) + " (mono)";
}
juce::String monitorShort(const Track& t) {
    switch (t.monitorMode()) {
        case Track::MonitorDirect: return "hear: interface";
        case Track::MonitorThrough: return "hear: through effects";
        default: return "hear: off";
    }
}

// Estimates the pitch of the input (YIN difference function) for the tuner.
float detectPitchHz(const std::vector<float>& x, double sr) {
    const int W = 1024;
    const int maxTau = std::min((int)(sr / 60.0), (int)x.size() - W - 1);
    const int minTau = (int)(sr / 1200.0);
    if (maxTau <= minTau) return 0.f;
    float energy = 0.f;
    for (int i = 0; i < W; ++i) energy += x[(size_t)i] * x[(size_t)i];
    if (energy / W < 1.0e-5f) return 0.f;   // too quiet
    std::vector<float> d((size_t)maxTau + 1, 0.f);
    float running = 0.f;
    d[0] = 1.f;
    for (int tau = 1; tau <= maxTau; ++tau) {
        float s = 0.f;
        for (int i = 0; i < W; ++i) { const float e = x[(size_t)i] - x[(size_t)(i + tau)]; s += e * e; }
        running += s;
        d[(size_t)tau] = running > 0.f ? s * (float)tau / running : 1.f;
    }
    int best = -1;
    for (int tau = minTau; tau < maxTau; ++tau)
        if (d[(size_t)tau] < 0.15f) { while (tau + 1 < maxTau && d[(size_t)tau + 1] < d[(size_t)tau]) ++tau; best = tau; break; }
    if (best < 0) return 0.f;
    // parabolic interpolation
    const float a = d[(size_t)best - 1], b = d[(size_t)best], c = d[(size_t)best + 1];
    const float den = a - 2.f * b + c;
    const float shift = std::fabs(den) > 1.0e-9f ? 0.5f * (a - c) / den : 0.f;
    return (float)sr / ((float)best + shift);
}

class ArmList : public juce::Component {
public:
    explicit ArmList(Session& s) : session(s) {}
    static constexpr int rowH = 58;
    std::vector<juce::String> ids;
    juce::String focus;
    std::function<void(const juce::String&)> onFocus;
    void rebuild() { ids.clear(); for (auto& t : session.project.tracks) if (t.hasClips()) ids.push_back(t.id); repaint(); }
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        for (int i = 0; i < (int)ids.size(); ++i) {
            Track* t = session.project.findTrack(ids[(size_t)i]);
            if (!t) continue;
            auto r = juce::Rectangle<int>(0, i * rowH, getWidth(), rowH).reduced(2).toFloat();
            g.setColour(t->id == focus ? p.raised : (t->armed ? p.record.withAlpha(0.12f) : p.surface));
            g.fillRoundedRectangle(r, rControl);
            if (t->id == focus) { g.setColour(p.accent); g.drawRoundedRectangle(r.reduced(0.5f), rControl, 1.5f); }
            g.setColour(t->colour);
            g.fillRoundedRectangle(r.removeFromLeft(5.f), 2.f);
            auto arm = r.removeFromLeft(54.f);
            g.setColour(t->armed ? p.record : p.line);
            g.fillEllipse(arm.withSizeKeepingCentre(26.f, 26.f));
            g.setColour(p.text);
            g.setFont(font(15.f, true));
            g.drawFittedText(t->name, r.removeFromTop(r.getHeight() * 0.55f).toNearestInt(), juce::Justification::bottomLeft, 1);
            g.setColour(p.dim);
            g.setFont(font(12.f));
            g.drawFittedText(t->type == TrackType::Audio ? inputLabel(*t) + ", " + monitorShort(*t) + (t->inputEffects.empty() ? juce::String() : ", " + juce::String((int)t->inputEffects.size()) + " fx")
                                                         : juce::String("MIDI: keyboard below or a MIDI device"), r.toNearestInt(), juce::Justification::topLeft, 1);
        }
        if (ids.empty()) {
            g.setColour(p.dim); g.setFont(font(15.f));
            g.drawFittedText("Add an audio track to record vocals or instruments, or an instrument track to record MIDI.", getLocalBounds().reduced(10), juce::Justification::centredTop, 3);
        }
    }
    void mouseUp(const juce::MouseEvent& e) override {
        const int i = e.y / rowH;
        if (i < 0 || i >= (int)ids.size() || e.getDistanceFromDragStart() > 8) return;
        Track* t = session.project.findTrack(ids[(size_t)i]);
        if (!t) return;
        if (e.x < 62) {   // the dot arms / disarms
            session.checkpoint(t->armed ? "Disarm track" : "Arm track");
            t->armed = !t->armed;
            session.changed(true);
            haptic(1);
            if (t->armed && t->type == TrackType::Audio && !ctx().engine->inputEnabled())
                ctx().engine->enableInput([](bool ok) { ctx().toast(ok ? "Microphone ready" : "Microphone access is off. Turn it on in system settings."); });
        }
        if (onFocus) onFocus(t->id);
        repaint();
    }
private:
    Session& session;
};

class RecordScreen : public Screen, private juce::Timer {
public:
    RecordScreen() : arms(session()) {
        addAndMakeVisible(recB);
        recB.setOnColour(pal().record);
        recB.onClick = [this] { toggleRecord(); };
        addAndMakeVisible(stopB);
        stopB.onClick = [this] { if (engine().isRecording()) engine().stopRecording(); engine().stop(); };
        addAndMakeVisible(armView);
        armView.setViewedComponent(&arms, false);
        armView.setScrollBarsShown(true, false);
        armView.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
        arms.onFocus = [this](const juce::String& id) { focusId = id; session().selectTrack(id); refresh(); };

        booth.owner = this;
        addAndMakeVisible(boothView);
        boothView.setViewedComponent(&booth, false);
        boothView.setScrollBarsShown(true, false);
        boothView.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
        for (auto* c : std::initializer_list<juce::Component*> { &inputB, &meter, &monOff, &monDirect, &monThrough, &lowLatB, &recClean, &recFx, &chain,
                                                                 &enableIn, &countIn, &metroT, &quantT, &takeB, &latency })
            booth.addAndMakeVisible(c);
        meter.setHorizontal(true);
        meter.levelSource = [this] { return focusTrack() ? engine().trackInputLevel(focusId) : std::make_pair(0.f, 0.f); };
        inputB.onClick = [this] { inputMenu(); };
        for (auto* c : { &monOff, &monDirect, &monThrough }) { c->setRadioGroupId(41); c->setClickingTogglesState(true); }
        monOff.onClick = [this] { setMonitor(Track::MonitorOff); };
        monDirect.onClick = [this] { setMonitor(Track::MonitorDirect); };
        monThrough.onClick = [this] { setMonitor(Track::MonitorThrough); };
        for (auto* c : { &recClean, &recFx }) { c->setRadioGroupId(42); c->setClickingTogglesState(true); }
        recClean.onClick = [this] { setPrint(false); };
        recFx.onClick = [this] { setPrint(true); };
        lowLatB.setClickingTogglesState(true);
        lowLatB.onClick = [this] {
            juce::String msg;
            engine().setLowLatency(lowLatB.getToggleState(), msg);
            lowLatB.setToggleState(engine().isLowLatency(), juce::dontSendNotification);
            ctx().toast(msg);
            repaint();
        };
        chain.chainCategory = "Vocals";
        chain.emptyText = "No live effects yet. Load a vocal or instrument chain, or add effects one by one. You can hear them while you record and choose whether they are recorded.";
        chain.chain = [this]() -> std::vector<EffectData>* { auto* t = focusTrack(); return t ? &t->inputEffects : nullptr; };

        enableIn.onClick = [this] { engine().enableInput([this](bool ok) { ctx().toast(ok ? "Audio input on" : "Microphone access was denied. You can allow it in system settings."); refresh(); }); };
        countIn.setLabel("Count-in");
        countIn.setItems({ "Off", "1 bar", "2 bars" }, 1);
        countIn.onChange = [this](int i) { session().checkpoint("Count-in"); proj().countInBars = i; session().changed(); };
        metroT.onClick = [this] { proj().metronome = metroT.getToggleState(); engine().setMetronome(proj().metronome); session().changed(); };
        quantT.onClick = [this] { AppSettings::get().quantizeMidiRecording = quantT.getToggleState(); AppSettings::get().save(); };
        latency.setSliderStyle(juce::Slider::LinearHorizontal);
        latency.setRange(-150.0, 150.0, 1.0);
        latency.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 30);
        latency.setTextValueSuffix(" ms");
        latency.setTitle("Latency offset");
        latency.onValueChange = [this] { AppSettings::get().latencyOffsetMs = latency.getValue(); engine().latencyCompensationMs = latency.getValue(); AppSettings::get().save(); };
        takeB.onClick = [this] { takesMenu(); };

        addAndMakeVisible(keys);
        keys.setRange(48, 22);
        keys.onNoteOn = [this](int n, float v) { if (auto* t = armedInstrument()) engine().liveNoteOn(t->id, n, v); };
        keys.onNoteOff = [this](int n) { if (auto* t = armedInstrument()) engine().liveNoteOff(t->id, n); };
        engine().onRecordingFinished = [this] {
            if (AppSettings::get().quantizeMidiRecording)
                for (auto& t : proj().tracks) for (auto& c : t.clips) if (c.name == "Recorded") Theory::quantize(c.notes, juce::jmax(PPQ / 8, snapTicks(proj(), juce::jmax(5, proj().snap), c.start)), 1.f, 0.f, false, false);
            session().changed(true);
            ctx().toast("Recording added to the song");
        };
        startTimerHz(15);
    }
    juce::String title() const override { return "Record"; }
    Track* armedInstrument() {
        for (auto& t : proj().tracks) if (t.armed && t.hasInstrument()) return &t;
        auto* t = session().track();
        return t && t->hasInstrument() ? t : nullptr;
    }
    Track* focusTrack() {
        Track* t = proj().findTrack(focusId);
        return t && t->type == TrackType::Audio ? t : nullptr;
    }
    void chooseFocus() {
        Track* t = proj().findTrack(focusId);
        if (t && t->hasClips()) return;
        focusId.clear();
        if (auto* s = session().track()) if (s->hasClips()) { focusId = s->id; return; }
        for (auto& tr : proj().tracks) if (tr.armed && tr.type == TrackType::Audio) { focusId = tr.id; return; }
        for (auto& tr : proj().tracks) if (tr.type == TrackType::Audio) { focusId = tr.id; return; }
    }
    void onShow() override {
        if (auto* s = session().track()) if (s->hasClips()) focusId = s->id;
        refresh();
    }
    void refresh() override {
        chooseFocus();
        arms.focus = focusId;
        arms.rebuild();
        arms.setSize(juce::jmax(100, armView.getWidth() - 8), juce::jmax(1, (int)arms.ids.size()) * ArmList::rowH + 4);
        countIn.setSelected(juce::jlimit(0, 2, proj().countInBars));
        metroT.setToggleState(proj().metronome, juce::dontSendNotification);
        quantT.setToggleState(AppSettings::get().quantizeMidiRecording, juce::dontSendNotification);
        latency.setValue(AppSettings::get().latencyOffsetMs, juce::dontSendNotification);
        enableIn.setVisible(!engine().inputEnabled());
        lowLatB.setToggleState(engine().isLowLatency(), juce::dontSendNotification);
        Track* t = focusTrack();
        const bool audio = t != nullptr;
        for (auto* c : std::initializer_list<juce::Component*> { &inputB, &meter, &monOff, &monDirect, &monThrough, &lowLatB, &recClean, &recFx, &chain }) c->setVisible(audio);
        if (t) {
            inputB.setButtonText(inputLabel(*t));
            const int mode = t->monitorMode();
            monOff.setToggleState(mode == Track::MonitorOff, juce::dontSendNotification);
            monDirect.setToggleState(mode == Track::MonitorDirect, juce::dontSendNotification);
            monThrough.setToggleState(mode == Track::MonitorThrough, juce::dontSendNotification);
            recClean.setToggleState(!t->recordsWithEffects(), juce::dontSendNotification);
            recFx.setToggleState(t->recordsWithEffects(), juce::dontSendNotification);
            const auto sig = t->id + juce::String((juce::pointer_sized_int)t->input.get());
            if (sig != knobSig) {
                knobSig = sig;
                gainK = std::make_unique<Knob>(t->input, 0, juce::String(), t->id);
                monK = std::make_unique<Knob>(t->input, 2, juce::String(), t->id);
                for (auto* k : { gainK.get(), monK.get() }) { k->setColour(t->colour); booth.addAndMakeVisible(k); }
            }
            chain.trackId = t->id;
            chain.refresh();
        } else { gainK.reset(); monK.reset(); knobSig.clear(); }
        resized();
        repaint();
    }
    void timerCallback() override {
        recB.setToggleState(engine().isRecording(), juce::dontSendNotification);
        if (!isShowing()) return;
        repaint(statusArea);
        if (++tunerTick % 3 == 0) updateTuner();
    }
    void updateTuner() {
        auto ch = focusTrack() ? engine().channelFor(focusId) : nullptr;
        juce::String txt;
        if (ch) {
            std::vector<float> buf(ChannelProc::kRing);
            const int pos = ch->inRingPos.load(std::memory_order_acquire);
            for (int i = 0; i < ChannelProc::kRing; ++i) buf[(size_t)i] = ch->inRing[(size_t)((pos + i) & (ChannelProc::kRing - 1))].load(std::memory_order_relaxed);
            const float hz = detectPitchHz(buf, engine().sampleRate());
            if (hz > 0.f) {
                const float midi = 69.f + 12.f * std::log2(hz / 440.f);
                const int n = juce::roundToInt(midi);
                const int cents = juce::roundToInt((midi - (float)n) * 100.f);
                txt = Theory::noteName(n) + (cents == 0 ? juce::String("  in tune") : "  " + juce::String(cents > 0 ? "+" : "") + juce::String(cents) + " cents");
            }
        }
        if (txt != tunerText) { tunerText = txt; booth.repaint(tunerArea); }
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(pal().bg);
        auto s = statusArea;
        juce::String big = "Ready to record", small;
        juce::Colour c = pal().text;
        if (engine().isCountingIn()) { big = "Count-in..."; c = pal().warn; }
        else if (engine().isRecording()) { big = "Recording " + proj().formatPosition(engine().playheadTick()); c = pal().record; }
        int armedAudio = 0, armedMidi = 0;
        for (auto& t : proj().tracks) if (t.armed) (t.type == TrackType::Audio ? armedAudio : armedMidi)++;
        small = armedAudio + armedMidi == 0 ? "Tap a track's dot to arm it. Tap its name to set it up." :
                juce::String(armedAudio) + " audio and " + juce::String(armedMidi) + " MIDI track" + (armedAudio + armedMidi == 1 ? "" : "s") + " armed." +
                (proj().loopOn ? " Loop is on: each pass becomes a new take." : "");
        g.setColour(c);
        g.setFont(font(24.f, true));
        g.drawText(big, s.removeFromTop(34), juce::Justification::centredLeft);
        g.setColour(pal().dim);
        g.setFont(font(13.5f));
        g.drawFittedText(small, s, juce::Justification::topLeft, 2);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(10, 8);
        narrow = r.getWidth() < 700;
        auto top = r.removeFromTop(72);
        recB.setBounds(top.removeFromLeft(72).reduced(4));
        stopB.setBounds(top.removeFromLeft(60).reduced(8));
        top.removeFromLeft(10);
        statusArea = top;
        r.removeFromTop(6);
        const bool showKeys = focusTrack() == nullptr;
        keys.setVisible(showKeys);
        if (showKeys) { keys.setBounds(r.removeFromBottom(narrow ? 110 : 130)); r.removeFromBottom(8); }
        if (narrow) {
            const int n = juce::jmax(1, (int)arms.ids.size());
            armView.setBounds(r.removeFromTop(juce::jmin(n, 2) * ArmList::rowH + 4));
            r.removeFromTop(6);
        } else { armView.setBounds(r.removeFromLeft(juce::jmin(320, r.getWidth() / 3))); r.removeFromLeft(14); }
        arms.setSize(juce::jmax(100, armView.getWidth() - 8), juce::jmax(1, (int)arms.ids.size()) * ArmList::rowH + 4);
        boothView.setBounds(r);
        layoutBooth(juce::jmax(200, r.getWidth() - 10));
    }
    void layoutBooth(int w) {
        int y = 0;
        auto row = [&](int h) { auto rr = juce::Rectangle<int>(0, y, w, h); y += h; return rr; };
        if (focusTrack()) {
            headArea = row(30);
            auto in = row(100);
            const int ib = juce::jmin(170, w / 3);
            inputB.setBounds(in.removeFromLeft(ib).withSizeKeepingCentre(ib, 44));
            in.removeFromLeft(8);
            if (gainK) gainK->setBounds(in.removeFromLeft(80));
            in.removeFromLeft(8);
            auto mt = in.withTrimmedTop(20);
            meterArea = mt.removeFromTop(22);
            meter.setBounds(meterArea);
            tunerArea = mt.withTrimmedTop(6).removeFromTop(24);
            y += 6;
            monHead = row(26);
            auto chips = row(46);
            const int cw = (chips.getWidth() - (narrow ? 0 : 100)) / 3;
            monOff.setBounds(chips.removeFromLeft(cw).reduced(2, 3));
            monDirect.setBounds(chips.removeFromLeft(cw).reduced(2, 3));
            monThrough.setBounds(chips.removeFromLeft(cw).reduced(2, 3));
            monText = row(narrow ? 58 : 40);
            auto ml = row(100);
            if (monK) monK->setBounds(ml.removeFromLeft(80));
            ml.removeFromLeft(10);
            const int lw = juce::jmin(220, ml.getWidth());
            lowLatB.setBounds(ml.removeFromLeft(lw).withSizeKeepingCentre(lw, 44));
            y += 6;
            recHead = row(26);
            auto rc = row(46);
            recClean.setBounds(rc.removeFromLeft(rc.getWidth() / 2).reduced(2, 3));
            recFx.setBounds(rc.reduced(2, 3));
            recText = row(narrow ? 40 : 24);
            y += 10;
            chainHead = row(28);
            chain.setBounds(row(narrow ? 560 : 460));
            y += 14;
        } else {
            headArea = row(30);
            monText = row(60);
        }
        settingsHead = row(28);
        if (enableIn.isVisible()) { enableIn.setBounds(row(44)); y += 8; }
        auto s1 = row(46);
        countIn.setBounds(s1.removeFromLeft(s1.getWidth() / 2).reduced(0, 2).withTrimmedRight(4));
        takeB.setBounds(s1.reduced(0, 2));
        metroT.setBounds(row(40));
        quantT.setBounds(row(40));
        latHead = row(22);
        latency.setBounds(row(36));
        booth.setSize(w, y + 20);
    }
    struct Booth : juce::Component {
        RecordScreen* owner = nullptr;
        void paint(juce::Graphics& g) override { owner->paintBooth(g); }
    };
    void paintBooth(juce::Graphics& g) {
        const auto& p = pal();
        Track* t = focusTrack();
        auto label = [&](const juce::String& txt, juce::Rectangle<int> r, bool bold = true) { g.setColour(bold ? p.text : p.dim); g.setFont(font(bold ? 15.f : 12.5f, bold)); g.drawFittedText(txt, r, juce::Justification::centredLeft, 3); };
        if (t) {
            heading(g, "Booth: " + t->name, headArea, 18.f);
            g.setColour(p.dim); g.setFont(font(12.f));
            g.drawText("Input level", meterArea.translated(0, -18).withHeight(16), juce::Justification::centredLeft);
            g.setColour(tunerText.isEmpty() ? p.faint : p.text); g.setFont(font(14.f, true));
            g.drawText(tunerText.isEmpty() ? juce::String("Tuner: play or sing a note") : "Tuner: " + tunerText, tunerArea, juce::Justification::centredLeft);
            label("Hear yourself", monHead);
            juce::String mt;
            const double ms = engine().monitorLatencyMs();
            switch (t->monitorMode()) {
                case Track::MonitorOff: mt = "You won't hear the microphone. Good for avoiding echo, or when you only want to hear the song."; break;
                case Track::MonitorDirect: mt = "The app stays silent and your audio interface plays your voice straight to the headphones with zero delay. Turn on its \"direct monitor\" switch or knob."; break;
                default: mt = "You hear yourself with the effects below, " + (ms > 0.0 ? "about " + juce::String(ms, 1) + " ms late" : juce::String("with the device's delay")) + (engine().isLowLatency() ? "." : ". Low latency mode makes the delay as short as this device allows."); break;
            }
            label(mt, monText, false);
            label("Record", recHead);
            label(t->recordsWithEffects() ? "The effects are recorded into the take (stereo file). You can't remove them later." : "The take is recorded clean. Add the same chain on the track later to change it any time.", recText, false);
            label("Live effects (mix your sound before recording)", chainHead);
        } else {
            heading(g, "MIDI recording", headArea, 18.f);
            label("Arm an instrument track and play the keyboard below or a MIDI controller. Tap an audio track's name to set up its booth: input, hearing yourself and live effects.", monText, false);
        }
        label("Recording settings", settingsHead);
        g.setColour(p.dim); g.setFont(font(12.5f));
        g.drawText("Latency compensation (device reports " + juce::String(engine().reportedLatencyMs(), 1) + " ms). Device: " + engine().deviceName(), latHead, juce::Justification::centredLeft);
    }

private:
    void setMonitor(int mode) {
        Track* t = focusTrack();
        if (!t) return;
        session().checkpoint("Monitoring");
        t->input->set(1, (float)mode);
        session().changed(true);
        if (mode == Track::MonitorThrough) {
            ctx().toast("Use headphones so the speakers don't feed back into the mic");
            if (!engine().inputEnabled()) engine().enableInput([](bool ok) { if (!ok) ctx().toast("Microphone access is off"); });
        }
        refresh();
    }
    void setPrint(bool fx) {
        Track* t = focusTrack();
        if (!t) return;
        session().checkpoint("Record mode");
        t->input->set(3, fx ? 1.f : 0.f);
        session().changed();
        refresh();
    }
    void inputMenu() {
        Track* t = focusTrack();
        if (!t) return;
        int numIn = 2;
        if (auto* d = engine().deviceManager().getCurrentAudioDevice()) numIn = juce::jmax(1, d->getInputChannelNames().size());
        juce::PopupMenu m;
        for (int i = 0; i < juce::jmin(numIn, 16); ++i) m.addItem(1 + i, "In " + juce::String(i + 1) + " (mono)", true, !t->stereoInput && t->inputChannel == i);
        for (int i = 0; i + 1 < juce::jmin(numIn, 16); i += 2) m.addItem(100 + i, "In " + juce::String(i + 1) + "+" + juce::String(i + 2) + " (stereo)", true, t->stereoInput && t->inputChannel == i);
        showMenu(m, &inputB, [this](int r) {
            Track* tt = focusTrack();
            if (!tt || r <= 0) return;
            session().checkpoint("Recording input");
            tt->stereoInput = r >= 100;
            tt->inputChannel = r >= 100 ? r - 100 : r - 1;
            session().changed(true);
            refresh();
        });
    }
    void toggleRecord() {
        if (engine().isRecording()) { engine().stopRecording(); return; }
        bool needsInput = false;
        for (auto& t : proj().tracks) if (t.armed && t.type == TrackType::Audio) needsInput = true;
        auto start = [this] { juce::String err; if (!engine().startRecording(err)) ctx().toast(err); };
        if (needsInput && !engine().inputEnabled()) engine().enableInput([start](bool ok) { if (ok) start(); else ctx().toast("Microphone access is off"); });
        else start();
    }
    void takesMenu() {
        Track* t = session().track();
        if (!t || t->type != TrackType::Audio) for (auto& tr : proj().tracks) if (tr.type == TrackType::Audio && tr.armed) { t = &tr; break; }
        Clip* clip = nullptr;
        if (t) for (auto& c : t->clips) if (c.type == ClipType::Audio && c.audio.takes.size() > 0) clip = &c;
        if (!clip) { ctx().toast("Record on an audio track first; takes appear here"); return; }
        juce::PopupMenu m;
        m.addSectionHeader(clip->name);
        for (int i = 0; i < clip->audio.takes.size(); ++i) {
            const double off = clip->audio.takes[i].fromLastOccurrenceOf("|", false, false).getDoubleValue();
            m.addItem(i + 1, "Take " + juce::String(i + 1), true, std::abs(off - clip->audio.offsetSec) < 1e-4 && clip->audio.file == clip->audio.takes[i].upToLastOccurrenceOf("|", false, false));
        }
        const auto clipId = clip->id;
        showMenu(m, &takeB, [this, clipId](int r) {
            if (r <= 0) return;
            for (auto& t2 : proj().tracks) for (auto& c : t2.clips) if (c.id == clipId && r - 1 < c.audio.takes.size()) {
                session().checkpoint("Choose take");
                c.audio.file = c.audio.takes[r - 1].upToLastOccurrenceOf("|", false, false);
                c.audio.offsetSec = c.audio.takes[r - 1].fromLastOccurrenceOf("|", false, false).getDoubleValue();
                c.name = t2.name + " take " + juce::String(r) + "/" + juce::String(c.audio.takes.size());
                session().changed(true);
            }
        });
    }

    IconButton recB { "record", {}, "Record" }, stopB { "stop", {}, "Stop" };
    ArmList arms;
    juce::Viewport armView, boothView;
    Booth booth;
    FxChainEditor chain;
    Meter meter;
    std::unique_ptr<Knob> gainK, monK;
    juce::String knobSig, focusId, tunerText;
    juce::TextButton inputB, lowLatB { "Low latency mode" }, enableIn { "Turn on microphone / audio input" }, takeB { "Takes..." };
    Chip monOff { "Off" }, monDirect { "Direct (interface)" }, monThrough { "Through effects" }, recClean { "Clean" }, recFx { "With effects" };
    Picker countIn;
    juce::ToggleButton metroT { "Metronome" }, quantT { "Quantize MIDI after recording" };
    juce::Slider latency;
    Keyboard keys;
    juce::Rectangle<int> statusArea, headArea, meterArea, tunerArea, monHead, monText, recHead, recText, chainHead, settingsHead, latHead;
    int tunerTick = 0;
    bool narrow = false;
};

}  // namespace

std::unique_ptr<Screen> makeRecordScreen() { return std::make_unique<RecordScreen>(); }
