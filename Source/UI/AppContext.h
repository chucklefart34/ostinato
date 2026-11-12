// Shared access to the app's long-lived objects for UI components.
#pragma once
#include "Core/Session.h"
#include "Theme.h"

enum class ScreenId { Home = 0, Arrange, PianoRoll, Drums, Mixer, Instruments, Effects, Samples, Record, Export, Settings, Count };

struct AppContext {
    Session* session = nullptr;
    AudioEngine* engine = nullptr;
    std::function<void(const juce::String&)> toast;
    std::function<void(ScreenId)> go;
    std::function<void(ParamSetPtr, int, const juce::String& target, const juce::String& trackId)> automate;  // open lane for param
    std::function<void()> refreshAll;
    juce::Component* root = nullptr;
    // Called by knobs/faders around a gesture so it becomes one undo step (and can write automation).
    std::function<void(const juce::String& what)> beginEdit;
    std::function<void()> endEdit;
    std::function<void(const ParamSet*, int, float)> paramMoved;   // automation write hook
};
AppContext& ctx();

namespace UI {
void haptic(int strength = 0);
}
