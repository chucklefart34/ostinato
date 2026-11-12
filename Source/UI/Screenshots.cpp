#include "Screenshots.h"
#include "MainComponent.h"
#include "Core/Paths.h"
#include "Core/Settings.h"
#include "Engine/Catalog.h"
#include "Engine/Exporter.h"

#if JUCE_MODAL_LOOPS_PERMITTED
int runScreenshots(const juce::File& dir) {
    dir.createDirectory();
    const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ostinato-shots");
    tmp.deleteRecursively();
    Paths::overrideRoot() = tmp;
    AppSettings::get().tutorialDone = true;
    UI::LookAndFeel lf;
    juce::LookAndFeel::setDefaultLookAndFeel(&lf);
    AudioEngine engine;
    engine.initialise();
    Session session(engine);
    ctx().session = &session;
    ctx().engine = &engine;
    SampleBank::get().ensureBuiltinPacks();
    // a believable song to look at
    session.newProject("Night Drive", "Beat Starter", 96.0, 4, 4);
    auto& p = session.project;
    p.markers.push_back({ 0, "Intro", true, UI::pal().accent });
    p.markers.push_back({ PPQ * 16, "Verse", true, juce::Colour(0xff48dbfb) });
    p.loopOn = true; p.loopStart = 0; p.loopEnd = PPQ * 16;
    for (int rep = 1; rep < 3; ++rep)
        for (auto& t : p.tracks) { std::vector<Clip> add; for (auto& c : t.clips) if (c.start < PPQ * 16) { Clip d = c; d.id = p.newId("c"); d.start += c.length * rep; add.push_back(d); } for (auto& d : add) t.clips.push_back(d); }
    auto& lead = p.addTrack(TrackType::Instrument, {}, "Studio Supersaw Lead");
    Clip lc = p.makeMidiClip(PPQ * 8, PPQ * 8);
    const int mel[] = { 72, 74, 75, 79, 77, 75, 74, 72, 70, 72, 74, 75 };
    for (int i = 0; i < 12; ++i) lc.notes.push_back({ i * PPQ / 2 + (i > 5 ? PPQ * 2 : 0), PPQ / 2 - 40, mel[i], 0.6f + 0.03f * i });
    lc.name = "Hook";
    lead.clips.push_back(lc);
    lead.effects.push_back(p.makeEffect("delay"));
    lead.effects.push_back(p.makeEffect("chorus"));
    AutomationLane al; al.target = "ch:vol"; al.points = { { 0, 0.4f, 1 }, { PPQ * 8, 0.85f, 0 }, { PPQ * 16, 0.7f, 2 } };
    lead.automation.push_back(al);
    auto& vox = p.addTrack(TrackType::Audio);
    vox.name = "Vocals";
    Clip ac; ac.id = p.newId("c"); ac.type = ClipType::Audio; ac.name = "Air Texture"; ac.audio.file = "lib:Packs/Ambient Textures/Air Texture.wav"; ac.start = PPQ * 4; ac.length = PPQ * 12;
    ac.audio.fadeInSec = 1.0;
    vox.clips.push_back(ac);
    vox.armed = true;
    vox.input->set(1, (float)Track::MonitorThrough);
    for (auto& c : Catalog::chainPresets())
        if (juce::String(c.name) == "Pop Vocal")
            for (auto& [type, values] : c.effects) { auto fx = p.makeEffect(type); Catalog::applyValues(*fx.params, values); vox.inputEffects.push_back(fx); }
    const auto voxId = vox.id;
    const auto leadId = p.tracks[p.tracks.size() - 2].id;
    p.master.effects.push_back(p.makeEffect("limiter"));
    Catalog::applyValues(*p.master.effects.back().params, "gain=2,ceiling=-1");
    session.selectTrack(p.tracks[2].id);
    session.editClipId = p.tracks[2].clips[0].id;
    session.changed(true);
    SampleBank::get().loadNow(ac.audio.file);
    engine.rebuild();

    MainComponent main(session, engine);
    struct Size { const char* name; int w, h; };
    const Size sizes[] = { { "phone", 390, 844 }, { "phone-landscape", 844, 390 }, { "desktop", 1280, 800 } };
    int written = 0;
    auto pump = [](int ms) { const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms; while (juce::Time::getMillisecondCounter() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); };
    pump(300);
    engine.seek(PPQ * 6);
    for (int theme = 0; theme < 2; ++theme) {
        AppSettings::get().darkTheme = theme == 0;
        main.applyTheme();
        for (auto& sz : sizes) {
            if (theme == 1 && juce::String(sz.name) != "phone") continue;
            for (int i = 0; i < (int)ScreenId::Count; ++i) {
                main.setSize(sz.w, sz.h);
                const auto sid = (ScreenId)i;
                session.selectTrack(sid == ScreenId::Record ? voxId : (sid == ScreenId::Instruments ? leadId : p.tracks[2].id));
                if (sid != ScreenId::Instruments && sid != ScreenId::Record) session.editClipId = p.tracks[2].clips[0].id;
                main.go(sid);
                pump(60);
                auto img = main.createComponentSnapshot(main.getLocalBounds(), true, 1.f);
                const auto f = dir.getChildFile(juce::String(sz.name) + (theme ? "-light-" : "-") + juce::String(i).paddedLeft('0', 2) + "-" + main.screen((ScreenId)i)->title().replaceCharacter(' ', '_') + ".png");
                f.deleteFile();
                juce::FileOutputStream os(f);
                if (os.openedOk() && juce::PNGImageFormat().writeImageToStream(img, os)) ++written;
            }
        }
    }
    std::printf("wrote %d screenshots to %s\n", written, dir.getFullPathName().toRawUTF8());
    // render the demo song the screenshots show, through the normal export path
    {
        ExportSettings es;
        es.format = ExportSettings::WAV; es.sampleRate = 44100.0; es.bitDepth = 16;
        es.destination = dir.getChildFile("demo-song.wav");
        Exporter ex(session.project, es);
        ex.run();
        std::printf("demo export: %s (%.1f s, peak %.1f dB) %s\n", es.destination.getFullPathName().toRawUTF8(), ex.result.seconds, ex.result.peakDb, ex.result.error.toRawUTF8());
    }
    engine.shutdown();
    ctx().session = nullptr; ctx().engine = nullptr;
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    Paths::overrideRoot() = juce::File();
    return written > 0 ? 0 : 1;
}
#else
int runScreenshots(const juce::File& dir) { juce::ignoreUnused(dir); return 1; }
#endif
