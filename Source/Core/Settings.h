// App-wide preferences (not per project).
#pragma once
#include <juce_core/juce_core.h>

struct AppSettings {
    bool darkTheme = true;
    float uiScale = 1.f;
    bool haptics = true;
    bool tooltips = true;
    bool tutorialDone = false;
    int autosaveSeconds = 60;
    double defaultBpm = 120.0;
    juce::String defaultTemplate = "Empty";
    double latencyOffsetMs = 0.0;
    bool quantizeMidiRecording = false;
    bool followPlayhead = true;
    bool midiClockSync = false;
    bool lowPowerWhenIdle = true;
    bool backgroundAudio = false;
    float metronomeVolume = 0.5f;
    juce::String audioDeviceXml;
    juce::StringArray recentProjects;      // folder paths, newest first
    juce::StringArray favoriteSamples;     // sample refs
    juce::StringArray recentSamples;
    juce::StringArray favoriteInstruments; // preset names

    void load();
    void save() const;
    void addRecentProject(const juce::String& path);
    void addRecentSample(const juce::String& ref);
    static AppSettings& get();
};
