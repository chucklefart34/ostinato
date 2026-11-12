// Ostinato application entry point: window, startup, crash recovery, lifecycle (iOS/Android
// background/foreground, interruptions), and the headless --selftest / --screenshots modes.
#include <juce_gui_extra/juce_gui_extra.h>
#include "Core/Paths.h"
#include "Core/Session.h"
#include "Core/Settings.h"
#include "Tests/SelfTest.h"
#include "UI/MainComponent.h"
#include "UI/Screenshots.h"
int runUITest(const juce::File& dir);

class OstinatoApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Ostinato"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& cmd) override {
        auto args = juce::StringArray::fromTokens(cmd, true);
        auto argAfter = [&args](const char* flag) { const int i = args.indexOf(flag); return i >= 0 && i + 1 < args.size() ? args[i + 1].unquoted() : juce::String(); };
        // relative paths are relative to the current folder (juce::File needs absolute paths)
        auto fileAfter = [&argAfter](const char* flag) { const auto a = argAfter(flag); return a.isEmpty() ? juce::File() : juce::File::getCurrentWorkingDirectory().getChildFile(a); };
        if (args.contains("--selftest")) {
            setApplicationReturnValue(runSelfTests(fileAfter("--selftest")));
            quit();
            return;
        }
        if (args.contains("--uitest")) {
            setApplicationReturnValue(runUITest(fileAfter("--uitest")));
            quit();
            return;
        }
        if (args.contains("--screenshots")) {
            setApplicationReturnValue(runScreenshots(fileAfter("--screenshots")));
            quit();
            return;
        }
        AppSettings::get().load();
        UI::setDark(AppSettings::get().darkTheme);
        lookAndFeel = std::make_unique<UI::LookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
        engine = std::make_unique<AudioEngine>();
        engine->latencyCompensationMs = AppSettings::get().latencyOffsetMs;
        engine->midiClockSync = AppSettings::get().midiClockSync;
        engine->setBatterySaver(AppSettings::get().lowPowerWhenIdle);
        session = std::make_unique<Session>(*engine);
        ctx().session = session.get();
        ctx().engine = engine.get();
        SampleBank::get().onLoaded = [this] { if (engine) engine->markDirty(); if (window) window->repaint(); };
        engine->initialise(AppSettings::get().audioDeviceXml);
        // generate the royalty-free sample packs in the background on first run
        juce::Thread::launch([] { SampleBank::get().ensureBuiltinPacks(); });
        const bool crashed = ProjectStore::previousSessionCrashed();
        ProjectStore::markSessionOpen();
        window = std::make_unique<MainWindow>(*session, *engine);
        // reopen the most recent song so work continues where it stopped
        for (auto& path : AppSettings::get().recentProjects) {
            juce::File f(path);
            juce::String err;
            if (ProjectStore::projectFile(f).existsAsFile() && session->open(f, err)) break;
        }
        if (crashed) window->main->toast("Ostinato closed unexpectedly last time. Your autosaved work is on the Projects screen.");
        if (!AppSettings::get().tutorialDone) window->main->startTutorial();
    }

    void shutdown() override {
        if (session && session->hasProject() && session->isDirty()) { juce::String e; session->save(e); }
        if (engine) AppSettings::get().audioDeviceXml = engine->deviceStateXml();
        AppSettings::get().save();
        window.reset();
        if (engine) engine->shutdown();
        ctx().session = nullptr;
        ctx().engine = nullptr;
        session.reset();
        engine.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        ProjectStore::markSessionClosed();
    }

    // iOS/Android: app moved to background (also covers phone calls on most devices)
    void suspended() override {
        if (!engine || !session) return;
        if (engine->isRecording()) engine->stopRecording();
        if (!AppSettings::get().backgroundAudio) engine->stop();
        if (session->hasProject() && session->isDirty()) { juce::String e; session->save(e); }
        AppSettings::get().save();
    }
    void resumed() override { if (window) window->repaint(); }
    void systemRequestedQuit() override { quit(); }
    void memoryWarningReceived() override { SampleBank::get().purgeUnused(); }

    class MainWindow : public juce::DocumentWindow {
    public:
        MainWindow(Session& s, AudioEngine& e) : juce::DocumentWindow("Ostinato", UI::pal().bg, juce::DocumentWindow::allButtons) {
            main = new MainComponent(s, e);
            setUsingNativeTitleBar(true);
            setContentOwned(main, false);
           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen(true);
           #else
            setResizable(true, true);
            setResizeLimits(360, 560, 10000, 10000);
            centreWithSize(1280, 800);
           #endif
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        MainComponent* main = nullptr;
    };

private:
    std::unique_ptr<UI::LookAndFeel> lookAndFeel;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<Session> session;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION(OstinatoApp)
