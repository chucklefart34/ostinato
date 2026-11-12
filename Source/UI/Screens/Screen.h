#pragma once
#include "UI/Widgets.h"

class Screen : public juce::Component, public Session::Listener {
public:
    Screen() { if (ctx().session) ctx().session->addListener(this); }
    ~Screen() override { if (ctx().session) ctx().session->removeListener(this); }
    virtual juce::String title() const = 0;
    virtual void onShow() { refresh(); }
    virtual void onHide() {}
    virtual void refresh() { repaint(); }
    virtual bool handleKey(const juce::KeyPress&) { return false; }
    void projectReplaced() override { if (isShowing()) refresh(); else stale = true; }
    void projectChanged(bool) override { if (isShowing()) refresh(); else stale = true; }
    void selectionChanged() override { if (isShowing()) refresh(); else stale = true; }
    void visibilityChanged() override { if (isShowing() && stale) { stale = false; refresh(); } }

protected:
    Session& session() { return *ctx().session; }
    Project& proj() { return ctx().session->project; }
    AudioEngine& engine() { return *ctx().engine; }
    // Wrap a model change as one undoable step.
    void edit(const juce::String& what, const std::function<void()>& fn, bool structural = false) {
        session().checkpoint(what);
        fn();
        session().changed(structural);
    }
    void paintBackground(juce::Graphics& g) { g.fillAll(UI::pal().bg); }
    bool stale = false;
};
