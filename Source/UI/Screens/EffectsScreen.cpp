#include "Screens.h"
#include "UI/FxChainEditor.h"
#include "Engine/Catalog.h"

using namespace UI;

namespace {

class EffectsScreen : public Screen {
public:
    EffectsScreen() {
        addAndMakeVisible(trackPicker);
        trackPicker.setLabel("Track");
        trackPicker.onChange = [this](int i) { if (i < trackIds.size()) { targetId = trackIds[i]; if (targetId != "master") session().selectTrack(targetId); editor.selectedFx.clear(); refresh(); } };
        addAndMakeVisible(editor);
        editor.automationPrefix = "fx:";
        editor.chain = [this]() -> std::vector<EffectData>* { auto* t = target(); return t ? &t->effects : nullptr; };
    }
    juce::String title() const override { return "Effects"; }
    void onShow() override { if (session().selectedTrack.isNotEmpty()) targetId = session().selectedTrack; refresh(); }
    Track* target() { return proj().findTrack(targetId.isEmpty() ? juce::String("master") : targetId); }
    void refresh() override {
        if (!target()) targetId = "master";
        trackIds.clear();
        juce::StringArray names;
        int sel = 0;
        for (auto& t : proj().tracks) { if (t.id == targetId) sel = trackIds.size(); trackIds.add(t.id); names.add(t.name); }
        if (targetId == "master") sel = trackIds.size();
        trackIds.add("master"); names.add("Master");
        trackPicker.setItems(names, sel);
        Track* t = target();
        editor.trackId = t->id;
        editor.returnTrack = t->type == TrackType::Return;
        editor.chainCategory = t->type == TrackType::Audio ? "Vocals" : "Instruments";
        editor.emptyText = "Add an effect to this track's chain, or load a chain preset. Effects run top to bottom, before the fader."
                           + juce::String(t->type == TrackType::Audio ? " Effects for the live microphone while recording are in the Record screen." : "");
        editor.refresh();
        resized();
        repaint();
    }
    void paint(juce::Graphics& g) override { g.fillAll(pal().bg); }
    void resized() override {
        auto r = getLocalBounds().reduced(8, 6);
        auto top = r.removeFromTop(48);
        trackPicker.setBounds(top.removeFromLeft(juce::jmin(320, top.getWidth())).reduced(0, 2));
        editor.setBounds(r);
    }
private:
    Picker trackPicker;
    juce::StringArray trackIds;
    juce::String targetId;
    FxChainEditor editor;
};

}  // namespace

std::unique_ptr<Screen> makeEffectsScreen() { return std::make_unique<EffectsScreen>(); }
