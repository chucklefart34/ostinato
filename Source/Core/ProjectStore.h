// Project files on disk: each project is a portable folder "<Name>.ostinato/" containing
// project.json, audio/ (recordings + imported audio) and thumbnail.png.
#pragma once
#include "Project.h"

struct ProjectInfo {
    juce::File folder;
    juce::String name, modified, created;
    double bpm = 120.0;
    int tracks = 0, bars = 0, key = 0, scale = 0;
    int sigNum = 4, sigDen = 4;
    double lengthSeconds = 0.0;
    juce::File thumbnail() const { return folder.getChildFile("thumbnail.png"); }
};

class ProjectStore {
public:
    static juce::File projectFile(const juce::File& folder) { return folder.getChildFile("project.json"); }
    static std::vector<ProjectInfo> list();                         // newest first
    static bool readInfo(const juce::File& folder, ProjectInfo& info);

    static juce::File uniqueFolder(const juce::String& name);
    static bool save(const Project& p, const juce::File& folder, juce::String& error);   // atomic write
    static bool load(Project& p, const juce::File& folder, juce::String& error);
    static bool rename(const juce::File& folder, const juce::String& newName, juce::File& newFolder, juce::String& error);
    static bool duplicate(const juce::File& folder, juce::File& copy, juce::String& error);
    static bool remove(const juce::File& folder);
    // Save As: copies audio that lives inside the old folder.
    static bool saveAs(const Project& p, const juce::File& oldFolder, const juce::String& newName, juce::File& newFolder, juce::String& error);
    // Zip a project for sharing / importing elsewhere.
    static bool exportZip(const juce::File& folder, const juce::File& zip, juce::String& error);
    static bool importZip(const juce::File& zip, juce::File& folder, juce::String& error);

    // Autosave + crash recovery
    static void writeRecovery(const Project& p, const juce::File& folder);
    static void clearRecovery();
    static bool recoveryAvailable(juce::String& projectName, juce::File& originalFolder, juce::Time& when);
    static bool loadRecovery(Project& p, juce::File& originalFolder, juce::String& error);
    static void markSessionOpen();
    static void markSessionClosed();
    static bool previousSessionCrashed();

    // Templates
    static juce::StringArray templateNames();                        // built-in + user
    static void buildTemplate(Project& p, const juce::String& name);
    static bool saveUserTemplate(const Project& p, const juce::String& name, juce::String& error);
};
