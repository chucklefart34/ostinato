// Effect chain editor shared by the Effects screen (track inserts) and the recording booth
// (live input chain): list with bypass / reorder, add menu with "My effects", chain presets,
// per-effect presets, rename, and an editor that understands the Effect Designer.
#pragma once
#include "Widgets.h"

namespace UI {

juce::File fxPresetDir(const juce::String& type);    // saved single-effect presets ("My effects")
juce::File chainPresetDir();                          // saved whole chains ("My chains")
juce::String effectDisplayName(const EffectData& e);
// Replaces `chain` with a factory or saved chain. Returns false if the preset is unknown.
bool applyChainPreset(Project& p, std::vector<EffectData>& chain, const juce::String& name);

class FxChainEditor : public juce::Component, private juce::Timer {
public:
    FxChainEditor();
    ~FxChainEditor() override;
    // Where the chain lives. Called on every use, so it survives undo / project reloads.
    std::function<std::vector<EffectData>*()> chain;
    juce::String trackId;            // knob colour, automation lanes
    juce::String automationPrefix;   // "fx:" for track inserts; empty = not automatable (input chain)
    juce::String emptyText = "Add an effect to this chain. Effects run top to bottom.";
    juce::String chainCategory;      // preferred chain preset category ("Vocals" / "Instruments"), empty = all
    bool returnTrack = false;        // reverbs/delays added at 100% wet
    juce::String selectedFx;

    void refresh();
    void resized() override;
    void paint(juce::Graphics&) override;
    void showChainMenu(juce::Component* target);   // presets / save / clear for the whole chain
    juce::Component* chainButton() { return &chainB; }

private:
    class List;
    void timerCallback() override;
    std::vector<EffectData>* get() { return chain ? chain() : nullptr; }
    EffectData* selected();
    void addMenu(juce::Component* c);
    void fxMenu(int i, juce::Component* c);
    void presetMenu();
    void configurePanel(const EffectData* fx);

    std::unique_ptr<List> list;
    juce::Viewport listView, editView;
    ParamPanel panel;
    juce::TextButton presetB { "Presets" }, chainB { "Chain presets" };
    juce::Rectangle<int> titleArea, meterArea;
    juce::String lastSig;
    std::array<int, 6> designerTypes { -1, -1, -1, -1, -1, -1 };   // to notice a module change
    bool narrow = false;
};

}  // namespace UI
