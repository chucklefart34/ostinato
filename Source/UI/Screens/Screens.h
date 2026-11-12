#pragma once
#include "Screen.h"
#include <memory>

std::unique_ptr<Screen> makeHomeScreen();
std::unique_ptr<Screen> makeArrangeScreen();
std::unique_ptr<Screen> makePianoRollScreen();
std::unique_ptr<Screen> makeDrumScreen();
std::unique_ptr<Screen> makeMixerScreen();
std::unique_ptr<Screen> makeInstrumentScreen();
std::unique_ptr<Screen> makeEffectsScreen();
std::unique_ptr<Screen> makeSampleScreen();
std::unique_ptr<Screen> makeRecordScreen();
std::unique_ptr<Screen> makeExportScreen();
std::unique_ptr<Screen> makeSettingsScreen();

// Shared helpers between screens
namespace ScreenUtil {
// A vertically scrolling container that works with one-finger drags on touch screens.
class Scroller : public juce::Viewport {
public:
    Scroller() { setScrollBarsShown(true, false); setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover); setScrollBarThickness(6); }
};
// Adds a track of a given type with sensible defaults; returns its id.
juce::String addTrack(Session& s, TrackType type, const juce::String& preset = {});
void trackMenu(Session& s, const juce::String& trackId, juce::Component* target);
void addTrackMenu(Session& s, juce::Component* target);
juce::String laneLabel(const Track& t, const juce::String& target);
}
