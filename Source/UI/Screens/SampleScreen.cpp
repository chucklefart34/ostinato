#include "Screens.h"
#include "Core/Paths.h"
#include "Core/Settings.h"
#include "Core/Theory.h"

using namespace UI;

namespace {

struct SampleItem { juce::String ref, name, folder; juce::File file; double seconds = 0.0; double rate = 0.0; int channels = 0; };

class SampleList : public juce::Component {
public:
    std::vector<SampleItem> items;
    juce::String selected;
    std::function<void(const SampleItem&)> onTap;
    std::function<void(const SampleItem&, juce::Component*)> onMenu;
    static constexpr int rowH = 60;
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        const auto favs = AppSettings::get().favoriteSamples;
        auto clip = g.getClipBounds();
        for (int i = juce::jmax(0, clip.getY() / rowH); i < juce::jmin((int)items.size(), clip.getBottom() / rowH + 1); ++i) {
            const auto& it = items[(size_t)i];
            auto r = juce::Rectangle<int>(0, i * rowH, getWidth(), rowH).reduced(2).toFloat();
            const bool sel = it.ref == selected;
            g.setColour(sel ? p.raised : p.surface);
            g.fillRoundedRectangle(r, rClip);
            auto wave = r.removeFromLeft(juce::jmin(150.f, r.getWidth() * 0.3f)).reduced(8.f, 10.f);
            g.setColour(p.bg);
            g.fillRoundedRectangle(wave, 4.f);
            if (auto s = SampleBank::get().request(it.ref)) drawWaveform(g, *s, wave.reduced(2.f), sel ? p.accent : p.dim);
            const bool fav = favs.contains(it.ref);
            drawIcon(g, fav ? "starF" : "star", r.removeFromRight(44.f).withSizeKeepingCentre(18.f, 18.f), fav ? p.accent : p.faint);
            auto t = r.reduced(10.f, 8.f);
            g.setColour(p.text);
            g.setFont(font(15.f, sel));
            g.drawFittedText(it.name, t.removeFromTop(t.getHeight() * 0.55f).toNearestInt(), juce::Justification::bottomLeft, 1);
            g.setColour(p.dim);
            g.setFont(font(12.f));
            juce::String meta = it.folder + ", " + (it.seconds < 10 ? juce::String(it.seconds, 2) + " s" : formatDuration(it.seconds));
            if (it.rate > 0) meta << ", " << juce::String(it.rate / 1000.0, 1) << " kHz" << (it.channels == 1 ? " mono" : "");
            g.drawFittedText(meta, t.toNearestInt(), juce::Justification::topLeft, 1);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        downIdx = e.y / rowH;
        lp.fire = [this](juce::Point<float>) { haptic(1); if (onMenu && downIdx >= 0 && downIdx < (int)items.size()) onMenu(items[(size_t)downIdx], this); };
        lp.down(e.position);
    }
    void mouseDrag(const juce::MouseEvent& e) override { lp.moved(e.position); }
    void mouseUp(const juce::MouseEvent& e) override {
        lp.up();
        if (lp.didFire() || e.getDistanceFromDragStart() > 8) return;
        const int i = e.y / rowH;
        if (i < 0 || i >= (int)items.size()) return;
        if (e.x > getWidth() - 48) {
            auto& favs = AppSettings::get().favoriteSamples;
            if (favs.contains(items[(size_t)i].ref)) favs.removeString(items[(size_t)i].ref); else favs.add(items[(size_t)i].ref);
            AppSettings::get().save();
            repaint();
            return;
        }
        if (e.mods.isPopupMenu()) { if (onMenu) onMenu(items[(size_t)i], this); return; }
        if (onTap) onTap(items[(size_t)i]);
    }
private:
    LongPress lp;
    int downIdx = -1;
};

class SampleScreen : public Screen {
public:
    SampleScreen() {
        for (auto* c : { &srcAll, &srcFav, &srcRecent, &srcPacks, &srcImported, &srcSong }) { addAndMakeVisible(c); c->setRadioGroupId(9); c->onClick = [this] { scan(); }; }
        srcAll.setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(search);
        search.setTextToShowWhenEmpty("Search samples", pal().dim);
        search.setFont(font(16.f));
        search.setIndents(12, 10);
        search.onTextChange = [this] { filter(); };
        search.setTitle("Search samples");
        addAndMakeVisible(category);
        category.setLabel("Folder");
        category.onChange = [this](int) { filter(); };
        addAndMakeVisible(sortB);
        sortB.setLabel("Sort");
        sortB.setItems({ "Name", "Shortest", "Longest" }, 0);
        sortB.onChange = [this](int) { filter(); };
        addAndMakeVisible(importB);
        importB.onClick = [this] { importFiles(); };
        addAndMakeVisible(listView);
        listView.setViewedComponent(&list, false);
        listView.setScrollBarsShown(true, false);
        listView.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
        list.onTap = [this](const SampleItem& it) {
            if (list.selected == it.ref && engine().isPreviewingSample()) { engine().previewSample(nullptr); return; }
            list.selected = it.ref;
            if (auto s = SampleBank::get().loadNow(it.ref)) engine().previewSample(s);
            list.repaint();
            actionB.setEnabled(true);
        };
        list.onMenu = [this](const SampleItem& it, juce::Component* c) { list.selected = it.ref; actionsMenu(it, c); };
        addAndMakeVisible(actionB);
        actionB.onClick = [this] { for (auto& it : all) if (it.ref == list.selected) { actionsMenu(it, &actionB); return; } };
        actionB.setEnabled(false);
    }
    juce::String title() const override { return "Samples"; }
    void onShow() override { scan(); }
    void onHide() override { engine().previewSample(nullptr); SampleBank::get().purgeUnused(); }
    void refresh() override { list.repaint(); }

    void scan() {
        all.clear();
        juce::StringArray folders { "All folders" };
        auto addDir = [this, &folders](const juce::File& dir, const juce::String& refPrefix, const juce::String& label) {
            for (auto& f : dir.findChildFiles(juce::File::findFiles, true, "*")) {
                if (!SampleBank::supportedExtensions().contains(f.getFileExtension().substring(1).toLowerCase())) continue;
                SampleItem it;
                it.file = f;
                it.ref = refPrefix + f.getRelativePathFrom(dir).replaceCharacter('\\', '/');
                it.name = f.getFileNameWithoutExtension();
                it.folder = label.isNotEmpty() ? label : f.getParentDirectory().getFileName();
                if (std::unique_ptr<juce::AudioFormatReader> r { SampleBank::get().formats().createReaderFor(f) }) { it.seconds = r->lengthInSamples / juce::jmax(1.0, r->sampleRate); it.rate = r->sampleRate; it.channels = (int)r->numChannels; }
                else continue;
                if (!folders.contains(it.folder)) folders.add(it.folder);
                all.push_back(it);
            }
        };
        const bool everything = srcAll.getToggleState();
        if (everything || srcPacks.getToggleState() || srcFav.getToggleState() || srcRecent.getToggleState()) addDir(Paths::library().getChildFile("Packs"), "lib:Packs/", {});
        if (everything || srcImported.getToggleState() || srcFav.getToggleState() || srcRecent.getToggleState()) addDir(Paths::imports(), "lib:Imported/", "Imported");
        if ((everything || srcSong.getToggleState() || srcFav.getToggleState() || srcRecent.getToggleState()) && session().hasProject()) addDir(session().folder.getChildFile("audio"), "audio/", "This song");
        if (srcFav.getToggleState()) { const auto f = AppSettings::get().favoriteSamples; all.erase(std::remove_if(all.begin(), all.end(), [&f](auto& i) { return !f.contains(i.ref); }), all.end()); }
        if (srcRecent.getToggleState()) {
            const auto r = AppSettings::get().recentSamples;
            all.erase(std::remove_if(all.begin(), all.end(), [&r](auto& i) { return !r.contains(i.ref); }), all.end());
            std::sort(all.begin(), all.end(), [&r](auto& a, auto& b) { return r.indexOf(a.ref) < r.indexOf(b.ref); });
        }
        category.setItems(folders, 0);
        filter();
        if (!SampleBank::get().libraryDir().getChildFile("Packs/.generated-v1").existsAsFile() && everything)
            juce::Timer::callAfterDelay(1500, [sp = juce::Component::SafePointer<SampleScreen>(this)] { if (sp && sp->isShowing()) sp->scan(); });   // packs still being generated
    }
    void filter() {
        list.items.clear();
        const auto q = search.getText().trim().toLowerCase();
        const auto folder = category.getSelected() > 0 ? category.getText() : juce::String();
        for (auto& it : all) {
            if (folder.isNotEmpty() && it.folder != folder) continue;
            if (q.isNotEmpty() && !it.name.toLowerCase().contains(q) && !it.folder.toLowerCase().contains(q)) continue;
            list.items.push_back(it);
        }
        if (!srcRecent.getToggleState()) {
            const int s = sortB.getSelected();
            std::sort(list.items.begin(), list.items.end(), [s](auto& a, auto& b) {
                if (s == 1) return a.seconds < b.seconds;
                if (s == 2) return a.seconds > b.seconds;
                return a.folder == b.folder ? a.name.compareNatural(b.name) < 0 : a.folder < b.folder;
            });
        }
        list.setSize(juce::jmax(100, listView.getWidth() - 8), (int)list.items.size() * SampleList::rowH + 8);
        list.repaint();
        repaint();
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(pal().bg);
        if (list.items.empty()) {
            g.setColour(pal().dim); g.setFont(font(15.f));
            g.drawFittedText(srcFav.getToggleState() ? "Tap the star on any sample to add it to your favorites." :
                             search.getText().isNotEmpty() ? "No samples match \"" + search.getText() + "\"." :
                             "No samples here yet. Tap Import to add WAV, AIFF, FLAC, OGG or MP3 files.", listView.getBounds().reduced(20), juce::Justification::centredTop, 3);
        }
    }
    void resized() override {
        auto r = getLocalBounds().reduced(8, 6);
        const bool narrow = r.getWidth() < 640;
        auto chips = r.removeFromTop(narrow ? 84 : 44);
        const int per = narrow ? 3 : 6;
        const int cw = chips.getWidth() / per;
        int i = 0;
        for (auto* c : { &srcAll, &srcFav, &srcRecent, &srcPacks, &srcImported, &srcSong }) { c->setBounds(chips.getX() + (i % per) * cw, chips.getY() + (i / per) * 42, cw, 40); ++i; }
        r.removeFromTop(6);
        auto row = r.removeFromTop(48);
        importB.setBounds(row.removeFromRight(narrow ? touch : 120).reduced(0, 2));
        importB.setCaption(narrow ? juce::String() : "Import");
        if (narrow) {
            search.setBounds(row.reduced(0, 2).withTrimmedRight(6));
            auto row2 = r.removeFromTop(50).withTrimmedTop(4);
            category.setBounds(row2.removeFromLeft(row2.getWidth() / 2).reduced(0, 2).withTrimmedRight(4));
            sortB.setBounds(row2.reduced(0, 2));
        } else {
            sortB.setBounds(row.removeFromRight(170).reduced(4, 2));
            category.setBounds(row.removeFromRight(230).reduced(4, 2));
            search.setBounds(row.reduced(0, 2));
        }
        r.removeFromTop(6);
        actionB.setBounds(r.removeFromBottom(50).withTrimmedTop(6));
        listView.setBounds(r);
        list.setSize(juce::jmax(100, r.getWidth() - 8), (int)list.items.size() * SampleList::rowH + 8);
    }
private:
    void importFiles() {
        chooser = std::make_unique<juce::FileChooser>("Import audio files", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems, [this](const juce::FileChooser& fc) {
            int ok = 0;
            juce::String lastErr;
            Paths::imports().createDirectory();
            for (auto& u : fc.getURLResults()) {
                auto f = u.getLocalFile();
                juce::File dest = Paths::imports().getNonexistentChildFile(f.getFileNameWithoutExtension(), f.getFileExtension());
                bool copied = false;
                if (f.existsAsFile()) copied = f.copyFileTo(dest);
                else if (auto in = u.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress))) {   // iOS document provider
                    dest = Paths::imports().getNonexistentChildFile(u.getFileName().upToLastOccurrenceOf(".", false, false), "." + u.getFileName().fromLastOccurrenceOf(".", false, false));
                    juce::FileOutputStream os(dest);
                    copied = os.openedOk() && os.writeFromInputStream(*in, -1) > 0;
                }
                juce::String err;
                if (copied && SampleBank::decodeFile(dest, err)) ++ok;
                else { dest.deleteFile(); lastErr = err.isNotEmpty() ? err : "Couldn't copy " + f.getFileName(); }
            }
            if (ok > 0) { srcImported.setToggleState(true, juce::sendNotification); ctx().toast("Imported " + juce::String(ok) + " file" + (ok > 1 ? "s" : "")); }
            else if (lastErr.isNotEmpty()) ctx().toast(lastErr);
        });
    }
    void actionsMenu(const SampleItem& it, juce::Component* target) {
        juce::PopupMenu m, padsMenu;
        Track* drums = session().track();
        if (!drums || drums->type != TrackType::Drum) { drums = nullptr; for (auto& t : proj().tracks) if (t.type == TrackType::Drum) { drums = &t; break; } }
        if (drums) for (int i = 0; i < 16; ++i) padsMenu.addItem(100 + i, "Pad " + juce::String(i + 1) + ": " + drums->inst.pads[(size_t)i].name);
        m.addSectionHeader(it.name);
        m.addItem(1, "Add to song at playhead");
        m.addSubMenu(drums ? "Use on drum pad (" + drums->name + ")" : juce::String("Use on drum pad"), padsMenu, drums != nullptr);
        m.addItem(2, "Load into sampler");
        m.addItem(3, "Detect tempo (BPM)");
        m.addItem(4, AppSettings::get().favoriteSamples.contains(it.ref) ? "Remove from favorites" : "Add to favorites");
        if (it.ref.startsWith("lib:Imported/")) m.addItem(5, "Delete from library...");
       #if ! (JUCE_IOS || JUCE_ANDROID)
        m.addItem(6, "Show file");
       #endif
        showMenu(m, target, [this, it](int r) {
            AppSettings::get().addRecentSample(it.ref);
            if (r == 1) addToSong(it);
            else if (r >= 100 && r < 116) {
                Track* d = session().track();
                if (!d || d->type != TrackType::Drum) for (auto& t : proj().tracks) if (t.type == TrackType::Drum) { d = &t; break; }
                if (!d) return;
                session().checkpoint("Pad sample");
                d->inst.pads[(size_t)(r - 100)].sample = it.ref;
                d->inst.pads[(size_t)(r - 100)].name = it.name;
                d->inst.preset = "Custom kit";
                session().changed(true);
                ctx().toast(it.name + " is on pad " + juce::String(r - 99));
            } else if (r == 2) {
                Track* t = session().track();
                if (!t || t->inst.engine != "sampler") { ScreenUtil::addTrack(session(), TrackType::Instrument, "Sampler"); t = session().track(); }
                if (!t) return;
                session().checkpoint("Load sample");
                t->inst.sample = it.ref;
                t->inst.sampleRoot = 60;
                t->name = it.name;
                session().changed(true);
                ctx().go(ScreenId::Instruments);
            } else if (r == 3) {
                auto s = SampleBank::get().loadNow(it.ref);
                const double bpm = s ? Theory::detectBpm(*s) : 0.0;
                if (bpm <= 0) { ctx().toast("No steady beat found in " + it.name); return; }
                confirm(it.name + ": " + juce::String(bpm, 1) + " BPM", "Set the song tempo to " + juce::String(bpm, 1) + " BPM?", "Set tempo", [this, bpm] {
                    session().checkpoint("Tempo from sample"); proj().tempo.front().bpm = bpm; session().changed();
                });
            } else if (r == 4) {
                auto& favs = AppSettings::get().favoriteSamples;
                if (favs.contains(it.ref)) favs.removeString(it.ref); else favs.add(it.ref);
                list.repaint();
            } else if (r == 5) confirm("Delete " + it.name + "?", "Songs that use this sample will show it as missing.", "Delete", [this, it] { it.file.deleteFile(); scan(); }, true);
            else if (r == 6) it.file.revealToUser();
            AppSettings::get().save();
        });
    }
    void addToSong(const SampleItem& it) {
        auto smp = SampleBank::get().loadNow(it.ref);
        if (!smp) { ctx().toast("Couldn't load " + it.name); return; }
        Track* t = session().track();
        session().checkpoint("Add sample to song");
        if (!t || t->type != TrackType::Audio) { ScreenUtil::addTrack(session(), TrackType::Audio); t = session().track(); }
        if (!t) return;
        const int at = snapTick(proj(), (int)engine().playheadTick(), proj().snap, false);
        Clip c; c.id = proj().newId("c"); c.type = ClipType::Audio; c.audio.file = it.ref; c.name = it.name; c.start = at;
        c.length = juce::jmax(PPQ / 4, (int)std::ceil(proj().secondsToTicks(proj().ticksToSeconds(at) + smp->seconds())) - at);
        t->clips.push_back(c);
        session().changed(true);
        ctx().toast(it.name + " added to " + t->name);
    }

    Chip srcAll { "All" }, srcFav { "Favorites" }, srcRecent { "Recent" }, srcPacks { "Packs" }, srcImported { "Imported" }, srcSong { "This song" };
    juce::TextEditor search;
    Picker category, sortB;
    IconButton importB { "import", "Import", "Import audio files" };
    juce::TextButton actionB { "Use selected sample..." };
    juce::Viewport listView;
    SampleList list;
    std::vector<SampleItem> all;
    std::unique_ptr<juce::FileChooser> chooser;
};

}  // namespace

std::unique_ptr<Screen> makeSampleScreen() { return std::make_unique<SampleScreen>(); }
