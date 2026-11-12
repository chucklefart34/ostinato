// Root component: navigation (bottom tabs on phones, side rail when wide), transport bar,
// screen host, toasts, keyboard shortcuts, automation write, first-run tutorial.
#pragma once
#include "Screens/Screen.h"

class TransportBar;
class NavBar;
class Tutorial;

class MainComponent : public juce::Component, public Session::Listener, private juce::Timer {
public:
    MainComponent(Session& s, AudioEngine& e);
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void parentHierarchyChanged() override;

    void go(ScreenId id);
    ScreenId current() const { return currentId; }
    Screen* screen(ScreenId id) { return screens[(size_t)id].get(); }
    void toast(const juce::String& text);
    void applyTheme();
    bool isCompact() const { return compact; }
    void startTutorial();
    void save();
    void undo();
    void redo();
    bool automationWrite = false;

    // test hooks
    juce::Image snapshot(ScreenId id, int w, int h);

    void projectReplaced() override;
    void projectChanged(bool) override;

private:
    void timerCallback() override;
    void makeThumbnail(const juce::File& folder);
    Session& session;
    AudioEngine& engine;
    std::array<std::unique_ptr<Screen>, (size_t)ScreenId::Count> screens;
    ScreenId currentId = ScreenId::Home;
    std::unique_ptr<TransportBar> transport;
    std::unique_ptr<NavBar> nav;
    std::unique_ptr<Tutorial> tutorial;
    juce::String toastText;
    juce::uint32 toastUntil = 0;
    bool compact = false;
    juce::BorderSize<int> safe;
    std::unique_ptr<juce::TooltipWindow> tooltips;
    juce::String editName;
    bool editOpen = false;
    juce::uint32 lastLowPower = 0;
};
