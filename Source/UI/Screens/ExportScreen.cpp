#include "Screens.h"
#include "Core/MidiIO.h"
#include "Core/Paths.h"
#include "Engine/Exporter.h"

using namespace UI;

namespace {

class ExportScreen : public Screen, private juce::Timer {
public:
    ExportScreen() {
        addAndMakeVisible(presets);
        presets.setLabel("Preset");
        presets.setItems(ExportSettings::presetNames(), 0);
        presets.onChange = [this](int i) { applyPreset(i); };
        addAndMakeVisible(format);
        format.setLabel("Format");
        juce::StringArray fmts { "WAV", "FLAC", "OGG Vorbis" };
        if (ExportSettings::formatAvailable(ExportSettings::AAC)) fmts.add("AAC (.m4a)");
        format.setItems(fmts, 0);
        format.onChange = [this](int) { updateEnabled(); };
        addAndMakeVisible(rate);
        rate.setLabel("Sample rate");
        rate.setItems({ "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz" }, 1);
        addAndMakeVisible(depth);
        depth.setLabel("Bit depth");
        depth.setItems({ "16-bit (dithered)", "24-bit", "32-bit float" }, 1);
        addAndMakeVisible(quality);
        quality.setLabel("Quality");
        quality.setItems(juce::OggVorbisAudioFormat().getQualityOptions(), 8);
        addAndMakeVisible(range);
        range.setLabel("Range");
        range.setItems({ "Whole song", "Loop region" }, 0);
        addAndMakeVisible(name);
        name.setFont(font(16.f));
        name.setIndents(12, 10);
        name.setTitle("File name");
        addAndMakeVisible(exportB);
        exportB.setColour(juce::TextButton::buttonColourId, pal().accent);
        exportB.setColour(juce::TextButton::textColourOffId, pal().accentText);
        exportB.onClick = [this] { start(); };
        addChildComponent(cancelB);
        cancelB.onClick = [this] { if (job) { job->signalThreadShouldExit(); } };
        addChildComponent(shareB);
        shareB.onClick = [this] { share(lastFile); };
        addChildComponent(playB);
        playB.onClick = [this] {
            if (engine().isPreviewingSample()) { engine().previewSample(nullptr); playB.setButtonText("Play export"); return; }
            juce::String err;
            if (auto s = SampleBank::decodeFile(lastFile, err)) { engine().previewSample(s); playB.setButtonText("Stop playback"); }
        };
        addAndMakeVisible(midiB);
        midiB.onClick = [this] { exportMidi(); };
        addAndMakeVisible(projB);
        projB.onClick = [this] { shareProject(); };
        addAndMakeVisible(folderB);
        folderB.onClick = [this] {
           #if JUCE_IOS || JUCE_ANDROID
            ctx().toast("Exports are in the Files app: On My iPhone > Ostinato > Exports");
           #else
            Paths::exports().revealToUser();
           #endif
        };
        applyPreset(0);
    }
    juce::String title() const override { return "Export"; }
    void onShow() override { name.setText(juce::File::createLegalFileName(proj().name), false); refresh(); }
    void refresh() override { updateEnabled(); repaint(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(pal().bg);
        auto r = textArea;
        heading(g, "Export \"" + proj().name + "\"", r.removeFromTop(30), 22.f);
        g.setColour(pal().dim);
        g.setFont(font(14.f));
        const double sec = proj().ticksToSeconds(proj().songEndTick());
        g.drawFittedText("Renders every track with its effects, automation and the master chain, faster than real time. Reverb and delay tails are included. Song length "
                         + formatDuration(sec) + ". MP3 export isn't available in this version: WAV, FLAC and OGG play almost everywhere"
                         + (ExportSettings::formatAvailable(ExportSettings::AAC) ? juce::String(", and AAC works with Apple apps.") : juce::String("."))
                         , r, juce::Justification::topLeft, 4);
        if (job) {
            auto p = progressArea.toFloat();
            g.setColour(pal().raised); g.fillRoundedRectangle(p, 8.f);
            g.setColour(pal().accent); g.fillRoundedRectangle(p.withWidth(p.getWidth() * juce::jlimit(0.f, 1.f, job->progress.load())), 8.f);
            g.setColour(pal().text); g.setFont(font(13.f, true));
            g.drawText("Rendering " + juce::String(juce::roundToInt(job->progress.load() * 100)) + "%", p.toNearestInt(), juce::Justification::centred);
        } else if (resultText.isNotEmpty()) {
            g.setColour(resultOk ? pal().text : pal().record);
            g.setFont(font(15.f, true));
            g.drawFittedText(resultText, progressArea.withHeight(60), juce::Justification::topLeft, 3);
        }
    }
    void resized() override {
        auto r = getLocalBounds().reduced(14, 10);
        auto col = r.withWidth(juce::jmin(560, r.getWidth()));
        textArea = col.removeFromTop(118);
        auto field = [&col](juce::Component& c) { c.setBounds(col.removeFromTop(48).reduced(0, 2)); col.removeFromTop(4); };
        field(presets);
        auto row = col.removeFromTop(48);
        format.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(0, 2).withTrimmedRight(4));
        rate.setBounds(row.reduced(0, 2));
        col.removeFromTop(4);
        row = col.removeFromTop(48);
        depth.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(0, 2).withTrimmedRight(4));
        quality.setBounds(depth.getBounds().withX(depth.getX()));
        range.setBounds(row.reduced(0, 2));
        col.removeFromTop(4);
        field(name);
        col.removeFromTop(6);
        auto btns = col.removeFromTop(50);
        exportB.setBounds(btns.removeFromLeft(btns.getWidth() / 2).withTrimmedRight(4));
        cancelB.setBounds(exportB.getBounds());
        shareB.setBounds(btns);
        col.removeFromTop(10);
        progressArea = col.removeFromTop(36);
        col.removeFromTop(30);
        playB.setBounds(col.removeFromTop(46).reduced(0, 2));
        col.removeFromTop(16);
        auto more = col.removeFromTop(46);
        const int w = more.getWidth() / 3;
        midiB.setBounds(more.removeFromLeft(w).reduced(2));
        projB.setBounds(more.removeFromLeft(w).reduced(2));
        folderB.setBounds(more.reduced(2));
    }
private:
    void applyPreset(int i) {
        auto s = ExportSettings::preset(i);
        format.setSelected((int)s.format);
        rate.setSelected(s.sampleRate == 44100 ? 0 : 1);
        depth.setSelected(s.bitDepth == 16 ? 0 : s.bitDepth == 24 ? 1 : 2);
        quality.setSelected(s.oggQuality);
        updateEnabled();
    }
    void updateEnabled() {
        const int f = format.getSelected();
        depth.setVisible(f == 0 || f == 1);
        quality.setVisible(f == 2);
        exportB.setEnabled(!job && proj().songEndTick() > 0);
    }
    ExportSettings settings() {
        ExportSettings s;
        s.format = (ExportSettings::Format)format.getSelected();
        const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
        s.sampleRate = rates[juce::jlimit(0, 3, rate.getSelected())];
        s.bitDepth = depth.getSelected() == 0 ? 16 : depth.getSelected() == 1 ? 24 : 32;
        if (s.format == ExportSettings::FLAC && s.bitDepth == 32) s.bitDepth = 24;
        s.oggQuality = quality.getSelected();
        s.loopRangeOnly = range.getSelected() == 1;
        auto base = juce::File::createLegalFileName(name.getText().trim().isEmpty() ? proj().name : name.getText().trim());
        s.destination = Paths::exports().getChildFile(base + s.extension());
        return s;
    }
    void start() {
        if (job) return;
        auto s = settings();
        if (s.loopRangeOnly && !proj().loopOn) { ctx().toast("Turn the loop on and set its region to export only that part"); return; }
        auto go = [this, s] {
            resultText.clear();
            shareB.setVisible(false);
            playB.setVisible(false);
            job = std::make_unique<Exporter>(proj(), s);
            job->onDone = [this](const ExportResult& r) { finished(r); };
            exportB.setVisible(false);
            cancelB.setVisible(true);
            job->startThread(juce::Thread::Priority::normal);
            startTimerHz(15);
        };
        if (s.destination.existsAsFile()) confirm("Replace " + s.destination.getFileName() + "?", "A file with this name is already in Exports.", "Replace", go);
        else go();
    }
    void timerCallback() override { repaint(); }
    void finished(const ExportResult& r) {
        stopTimer();
        cancelB.setVisible(false);
        exportB.setVisible(true);
        if (r.ok) {
            lastFile = r.file;
            resultOk = true;
            resultText = "Exported " + r.file.getFileName() + " (" + formatDuration(r.seconds) + ", " + juce::File::descriptionOfSizeInBytes(r.file.getSize()) + ")";
            if (r.peakDb > 0.f) { resultText << ". Peaks reach +" << juce::String(r.peakDb, 1) << " dB and will clip: lower the master fader or add a limiter."; resultOk = false; }
            shareB.setVisible(true);
            playB.setVisible(true);
            haptic(2);
        } else {
            resultOk = false;
            resultText = r.cancelled ? juce::String("Export cancelled. No file was written.") : "Export failed: " + r.error;
        }
        job.reset();
        updateEnabled();
        repaint();
    }
    void share(const juce::File& f) {
        if (!f.existsAsFile()) return;
       #if JUCE_IOS || JUCE_ANDROID
        shareBox = juce::ContentSharer::shareFilesScoped({ juce::URL(f) }, [](bool, const juce::String&) {}, this);
       #else
        chooser = std::make_unique<juce::FileChooser>("Save a copy", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(f.getFileName()), "*" + f.getFileExtension());
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [f](const juce::FileChooser& fc) {
            auto dest = fc.getResult();
            if (dest == juce::File()) return;
            if (f.copyFileTo(dest)) ctx().toast("Saved to " + dest.getParentDirectory().getFileName()); else ctx().toast("Couldn't save there");
        });
       #endif
    }
    void exportMidi() {
        const auto f = Paths::exports().getChildFile(juce::File::createLegalFileName(proj().name) + ".mid");
        juce::String err;
        if (MidiIO::exportFile(proj(), f, err)) { lastFile = f; ctx().toast("Saved " + f.getFileName() + " in Exports"); share(f); }
        else ctx().toast(err);
    }
    void shareProject() {
        juce::String err;
        session().save(err);
        const auto zip = Paths::exports().getChildFile(juce::File::createLegalFileName(proj().name) + ".ostinato.zip");
        if (ProjectStore::exportZip(session().folder, zip, err)) { ctx().toast("Project saved as " + zip.getFileName()); share(zip); }
        else ctx().toast(err);
    }

    Picker presets, format, rate, depth, quality, range;
    juce::TextEditor name;
    juce::TextButton exportB { "Export" }, cancelB { "Cancel" }, shareB { "Share / save a copy" }, playB { "Play export" }, midiB { "MIDI file" }, projB { "Project (.zip)" }, folderB { "Exports folder" };
    std::unique_ptr<Exporter> job;
    juce::File lastFile;
    juce::String resultText;
    bool resultOk = true;
    juce::Rectangle<int> textArea, progressArea;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::ScopedMessageBox shareBox;
};

}  // namespace

std::unique_ptr<Screen> makeExportScreen() { return std::make_unique<ExportScreen>(); }
