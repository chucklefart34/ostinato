// The open document: project + folder + undo history + autosave + selection, wired to the engine.
#pragma once
#include "ProjectStore.h"
#include "Engine/AudioEngine.h"

class Session : private juce::Timer {
public:
    explicit Session(AudioEngine& e);
    ~Session() override;

    Project project;
    juce::File folder;
    AudioEngine& engine;

    // --- document lifecycle
    void newProject(const juce::String& name, const juce::String& templ, double bpm, int num, int den);
    bool open(const juce::File& folder, juce::String& error);
    bool save(juce::String& error);
    bool saveAs(const juce::String& name, juce::String& error);
    bool isDirty() const { return dirty; }
    bool hasProject() const { return folder != juce::File(); }
    std::function<void(const juce::File& folder)> makeThumbnail;   // provided by the UI

    // --- edits
    // Call checkpoint() *before* changing the project; then changed().
    void checkpoint(const juce::String& description);
    void changed(bool structural = false);  // structural = tracks/effects/routing (UI rebuilds lists)
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    juce::String undoName() const { return undoStack.empty() ? juce::String() : undoStack.back().name; }
    juce::String redoName() const { return redoStack.empty() ? juce::String() : redoStack.back().name; }
    void undo();
    void redo();
    juce::StringArray history() const;

    // --- selection / editing context
    juce::String selectedTrack, editClipId, currentPattern;
    void selectTrack(const juce::String& id);
    Track* track() { return project.findTrack(selectedTrack); }
    Clip* editClip(Track** owner = nullptr);
    Pattern* pattern();

    // --- notifications
    struct Listener {
        virtual ~Listener() = default;
        virtual void projectChanged(bool structural) { juce::ignoreUnused(structural); }
        virtual void projectReplaced() {}      // new/open/undo: rebind everything
        virtual void selectionChanged() {}
    };
    void addListener(Listener* l) { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }
    void notifySelection() { listeners.call([](Listener& l) { l.selectionChanged(); }); }

    // statistics for diagnostics
    size_t undoBytes() const;
    juce::Time lastSave, lastAutosave;
    juce::String lastAutosaveError;

private:
    void timerCallback() override;
    void replaced();
    void restore(const juce::String& json);
    struct State { juce::String name, json; };
    std::vector<State> undoStack, redoStack;
    bool dirty = false;
    int secondsSinceAutosave = 0, secondsSinceRecovery = 0;
    juce::ListenerList<Listener> listeners;
};
