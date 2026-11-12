#include "Screens.h"
#include "Core/MidiIO.h"
#include "Core/Paths.h"
#include "Core/Settings.h"

using namespace UI;

namespace {

class NewSongSheet : public juce::Component {
public:
    std::function<void(const juce::String&, const juce::String&, double, int, int)> onCreate;
    std::function<void()> onCancel;
    NewSongSheet() {
        addAndMakeVisible(name);
        name.setText("New song", false);
        name.setFont(font(18.f));
        name.setIndents(12, 10);
        name.setSelectAllWhenFocused(true);
        name.setTitle("Song name");
        addAndMakeVisible(templ);
        templ.setLabel("Template");
        auto t = ProjectStore::templateNames();
        templ.setItems(t, juce::jmax(0, t.indexOf(AppSettings::get().defaultTemplate)));
        addAndMakeVisible(bpm);
        bpm.setSliderStyle(juce::Slider::LinearHorizontal);
        bpm.setRange(20.0, 300.0, 1.0);
        bpm.setValue(AppSettings::get().defaultBpm);
        bpm.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 32);
        bpm.setTextValueSuffix(" BPM");
        bpm.setTitle("Tempo");
        addAndMakeVisible(sig);
        sig.setLabel("Time");
        sig.setItems({ "4/4", "3/4", "6/8", "2/4", "5/4", "7/8", "12/8" }, 0);
        addAndMakeVisible(create);
        create.setColour(juce::TextButton::buttonColourId, pal().accent);
        create.setColour(juce::TextButton::textColourOffId, pal().accentText);
        create.onClick = [this] {
            const auto s = sig.getText();
            if (onCreate) onCreate(name.getText().trim().isEmpty() ? juce::String("New song") : name.getText().trim(), templ.getText(), bpm.getValue(),
                                   s.upToFirstOccurrenceOf("/", false, false).getIntValue(), s.fromFirstOccurrenceOf("/", false, false).getIntValue());
        };
        addAndMakeVisible(cancel);
        cancel.onClick = [this] { if (onCancel) onCancel(); };
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black.withAlpha(0.5f));
        auto c = card().toFloat();
        g.setColour(pal().surface);
        g.fillRoundedRectangle(c, rSheet);
        heading(g, "New song", card().reduced(20).removeFromTop(30), 22.f);
        g.setColour(pal().dim);
        g.setFont(font(13.f));
        g.drawText("Tempo", bpm.getBounds().translated(0, -20).withHeight(18), juce::Justification::centredLeft);
    }
    juce::Rectangle<int> card() const { const int w = juce::jmin(getWidth() - 24, 480); return { (getWidth() - w) / 2, juce::jmax(12, getHeight() / 2 - 230), w, 420 }; }
    void resized() override {
        auto r = card().reduced(20);
        r.removeFromTop(44);
        name.setBounds(r.removeFromTop(50)); r.removeFromTop(12);
        templ.setBounds(r.removeFromTop(48)); r.removeFromTop(32);
        bpm.setBounds(r.removeFromTop(44)); r.removeFromTop(12);
        sig.setBounds(r.removeFromTop(48));
        auto row = r.removeFromBottom(touch + 4);
        cancel.setBounds(row.removeFromLeft(120));
        create.setBounds(row.removeFromRight(180));
    }
    void mouseUp(const juce::MouseEvent& e) override { if (!card().contains(e.getPosition()) && onCancel) onCancel(); }
    juce::TextEditor name;
    Picker templ, sig;
    juce::Slider bpm;
    juce::TextButton create { "Create song" }, cancel { "Cancel" };
};

class ProjectCard : public juce::Component {
public:
    ProjectInfo info;
    juce::Image thumb;
    bool current = false;
    std::function<void()> onOpen;
    std::function<void(juce::Component*)> onMenu;
    LongPress lp;
    explicit ProjectCard(const ProjectInfo& i) : info(i) {
        if (info.thumbnail().existsAsFile()) thumb = juce::ImageFileFormat::loadFrom(info.thumbnail());
        lp.fire = [this](juce::Point<float>) { haptic(1); if (onMenu) onMenu(this); };
        setTitle(info.name);
    }
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        auto r = getLocalBounds().toFloat().reduced(4.f);
        g.setColour(p.surface);
        g.fillRoundedRectangle(r, rControl);
        if (current) { g.setColour(p.accent); g.drawRoundedRectangle(r.reduced(1.f), rControl, 2.f); }
        auto img = r.removeFromTop(r.getHeight() - 58.f).reduced(8.f);
        if (thumb.isValid()) g.drawImage(thumb, img, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
        else { g.setColour(p.raised); g.fillRoundedRectangle(img, rClip); }
        auto t = r.reduced(12.f, 6.f).toNearestInt();
        g.setColour(p.text);
        g.setFont(font(16.f, true));
        g.drawFittedText(info.name, t.removeFromTop(22), juce::Justification::centredLeft, 1);
        g.setColour(p.dim);
        g.setFont(font(13.f));
        const auto when = juce::Time::fromISO8601(info.modified);
        juce::String meta = juce::String(info.bpm, 0) + " BPM, " + juce::String(info.sigNum) + "/" + juce::String(info.sigDen) + ", " + formatDuration(info.lengthSeconds);
        g.drawFittedText(meta, t.removeFromTop(18), juce::Justification::centredLeft, 1);
        g.drawFittedText(when.toMilliseconds() > 0 ? when.formatted("%d %b %Y, %H:%M") : juce::String(), t, juce::Justification::centredLeft, 1);
    }
    void mouseDown(const juce::MouseEvent& e) override { if (e.mods.isPopupMenu()) { if (onMenu) onMenu(this); return; } lp.down(e.position); }
    void mouseDrag(const juce::MouseEvent& e) override { lp.moved(e.position); }
    void mouseUp(const juce::MouseEvent& e) override {
        lp.up();
        if (lp.didFire() || e.mods.isPopupMenu() || e.getDistanceFromDragStart() > 10) return;
        if (onOpen) onOpen();
    }
};

class HomeScreen : public Screen {
public:
    HomeScreen() {
        addAndMakeVisible(newSong);
        newSong.setColour(juce::TextButton::buttonColourId, pal().accent);
        newSong.setColour(juce::TextButton::textColourOffId, pal().accentText);
        newSong.onClick = [this] { showNewSong(); };
        addAndMakeVisible(importB);
        importB.onClick = [this] { importProject(); };
        addAndMakeVisible(scroller);
        scroller.setViewedComponent(&grid, false);
        addChildComponent(recoveryBanner);
        recoveryBanner.onClick = [this] { recover(); };
        recoveryBanner.setColour(juce::TextButton::buttonColourId, pal().warn.withAlpha(0.25f));
    }
    juce::String title() const override { return "Projects"; }

    void refresh() override {
        cards.clear();
        grid.removeAllChildren();
        auto list = ProjectStore::list();
        for (auto& info : list) {
            auto* c = cards.add(new ProjectCard(info));
            c->current = session().hasProject() && info.folder == session().folder;
            c->onOpen = [this, f = info.folder] { open(f); };
            c->onMenu = [this, info](juce::Component* t) { cardMenu(info, t); };
            grid.addAndMakeVisible(c);
        }
        juce::String rname; juce::File rf; juce::Time when;
        const bool rec = ProjectStore::recoveryAvailable(rname, rf, when) && !(session().hasProject() && session().isDirty());
        recoveryBanner.setVisible(rec);
        if (rec) recoveryBanner.setButtonText("Unsaved changes to \"" + rname + "\" from " + when.formatted("%H:%M") + " were recovered. Tap to restore.");
        resized();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        paintBackground(g);
        auto r = getLocalBounds().reduced(16, 12);
        g.setColour(pal().text);
        g.setFont(font(28.f, true));
        g.drawText("Your songs", r.removeFromTop(40), juce::Justification::centredLeft);
        if (cards.isEmpty()) {
            g.setColour(pal().dim);
            g.setFont(font(16.f));
            g.drawFittedText("No songs yet. Start one with \"New song\": pick a template, set the tempo, and you're making music.",
                             getLocalBounds().reduced(32).withTrimmedTop(140), juce::Justification::centredTop, 4);
        }
    }

    void resized() override {
        auto r = getLocalBounds().reduced(16, 12);
        auto top = r.removeFromTop(40);
        newSong.setBounds(top.removeFromRight(140).reduced(0, 0));
        top.removeFromRight(8);
        importB.setBounds(top.removeFromRight(top.getWidth() > 300 ? 150 : 48));
        importB.setButtonText(top.getWidth() > 150 ? "Import" : "+");
        r.removeFromTop(10);
        if (recoveryBanner.isVisible()) { recoveryBanner.setBounds(r.removeFromTop(52)); r.removeFromTop(8); }
        scroller.setBounds(r);
        const int w = r.getWidth() - 8;
        const int cols = juce::jmax(1, w / 230);
        const int cw = w / cols, ch = juce::jmin(260, cw * 3 / 4 + 70);
        for (int i = 0; i < cards.size(); ++i) cards[i]->setBounds((i % cols) * cw, (i / cols) * ch, cw, ch);
        grid.setSize(w, ((cards.size() + cols - 1) / cols) * ch + 20);
        if (sheet) sheet->setBounds(getLocalBounds());
    }

private:
    void open(const juce::File& f) {
        if (session().hasProject() && session().folder == f) { ctx().go(ScreenId::Arrange); return; }
        auto doOpen = [this, f] {
            juce::String err;
            if (session().hasProject() && session().isDirty()) { juce::String e2; session().save(e2); }
            if (session().open(f, err)) { ProjectStore::clearRecovery(); ctx().go(ScreenId::Arrange); }
            else info("Couldn't open the song", err);
        };
        doOpen();
    }
    void showNewSong() {
        sheet = std::make_unique<NewSongSheet>();
        sheet->onCancel = [this] { juce::MessageManager::callAsync([this] { sheet.reset(); }); };
        sheet->onCreate = [this](const juce::String& n, const juce::String& t, double bpm, int num, int den) {
            if (session().hasProject() && session().isDirty()) { juce::String e; session().save(e); }
            session().newProject(n, t, bpm, num, den);
            juce::MessageManager::callAsync([this] { sheet.reset(); ctx().go(ScreenId::Arrange); });
        };
        addAndMakeVisible(*sheet);
        sheet->setBounds(getLocalBounds());
    }
    void cardMenu(const ProjectInfo& info, juce::Component* t) {
        juce::PopupMenu m;
        m.addSectionHeader(info.name);
        m.addItem(1, "Open");
        m.addItem(2, "Rename...");
        m.addItem(3, "Duplicate");
        m.addItem(4, "Share project (.zip)");
        m.addItem(5, "Save as template...");
        m.addSeparator();
        m.addItem(6, "Delete...", true, false);
        showMenu(m, t, [this, info](int r) {
            juce::String err;
            if (r == 1) open(info.folder);
            else if (r == 2) askText("Rename song", {}, info.name, "Rename", [this, info](const juce::String& n) {
                juce::String e; juce::File nf;
                const bool isOpen = session().hasProject() && session().folder == info.folder;
                if (isOpen) { session().project.name = n; juce::String e2; session().save(e2); }
                if (ProjectStore::rename(info.folder, n, nf, e)) { if (isOpen) session().open(nf, e); refresh(); ctx().toast("Renamed"); }
                else ctx().toast(e);
            });
            else if (r == 3) { juce::File copy; if (ProjectStore::duplicate(info.folder, copy, err)) { refresh(); ctx().toast("Duplicated"); } else ctx().toast(err); }
            else if (r == 4) shareZip(info);
            else if (r == 5) askText("Save as template", "Tracks, instruments and effects are kept; audio clips are left out.", info.name, "Save", [info](const juce::String& n) {
                Project p; juce::String e;
                if (ProjectStore::load(p, info.folder, e) && ProjectStore::saveUserTemplate(p, n, e)) ctx().toast("Template saved"); else ctx().toast(e);
            });
            else if (r == 6) confirm("Delete \"" + info.name + "\"?", "The song and its recordings will be removed from this device. This can't be undone.", "Delete", [this, info] {
                if (session().hasProject() && session().folder == info.folder) { session().folder = juce::File(); session().project = Project(); ctx().engine->setProject(nullptr); }
                ProjectStore::remove(info.folder);
                refresh();
                ctx().toast("Deleted");
            }, true);
        });
    }
    void shareZip(const ProjectInfo& info) {
        const auto zip = Paths::exports().getChildFile(juce::File::createLegalFileName(info.name) + ".ostinato.zip");
        juce::String err;
        if (!ProjectStore::exportZip(info.folder, zip, err)) { ctx().toast(err); return; }
       #if JUCE_IOS || JUCE_ANDROID
        shareBox = juce::ContentSharer::shareFilesScoped({ juce::URL(zip) }, [](bool, const juce::String&) {}, this);
       #else
        zip.revealToUser();
        ctx().toast("Saved " + zip.getFileName() + " in Exports");
       #endif
    }
    void importProject() {
        chooser = std::make_unique<juce::FileChooser>("Import a song (.zip project or MIDI file)", Paths::root(), "*.zip;*.mid;*.midi");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            juce::String err;
            if (f.hasFileExtension("mid;midi")) {
                session().newProject(f.getFileNameWithoutExtension(), "Empty", 120.0, 4, 4);
                session().project.tracks.clear();
                int added = 0;
                if (MidiIO::importFile(session().project, f, err, added)) { session().changed(true); session().engine.rebuild(); ctx().go(ScreenId::Arrange); ctx().toast("Imported " + juce::String(added) + " tracks"); }
                else ctx().toast(err);
                return;
            }
            juce::File folder;
            if (ProjectStore::importZip(f, folder, err)) { refresh(); open(folder); }
            else ctx().toast(err);
        });
    }
    void recover() {
        Project p; juce::File folder; juce::String err;
        if (!ProjectStore::loadRecovery(p, folder, err)) { ctx().toast(err); return; }
        if (!folder.isDirectory()) folder = ProjectStore::uniqueFolder(p.name);
        ProjectStore::save(p, folder, err);
        ProjectStore::clearRecovery();
        open(folder);
        ctx().toast("Recovered \"" + p.name + "\"");
    }

    juce::TextButton newSong { "New song" }, importB { "Import" }, recoveryBanner;
    ScreenUtil::Scroller scroller;
    juce::Component grid;
    juce::OwnedArray<ProjectCard> cards;
    std::unique_ptr<NewSongSheet> sheet;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::ScopedMessageBox shareBox;
};
}  // namespace

std::unique_ptr<Screen> makeHomeScreen() { return std::make_unique<HomeScreen>(); }
