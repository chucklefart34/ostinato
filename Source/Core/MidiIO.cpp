#include "MidiIO.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace MidiIO {

static void addTempoTrack(const Project& p, juce::MidiMessageSequence& seq) {
    seq.addEvent(juce::MidiMessage::textMetaEvent(3, p.name));
    for (auto& t : p.tempo) {
        auto m = juce::MidiMessage::tempoMetaEvent((int)std::lround(60000000.0 / t.bpm));
        m.setTimeStamp(t.tick);
        seq.addEvent(m);
    }
    for (auto& ts : p.timeSig) {
        auto m = juce::MidiMessage::timeSignatureMetaEvent(ts.num, ts.den);
        m.setTimeStamp(p.barToTick(ts.bar));
        seq.addEvent(m);
    }
    auto ks = juce::MidiMessage::keySignatureMetaEvent(0, p.scale == 1);
    seq.addEvent(ks);
}

static void notesToSeq(const Project& p, const Track& t, juce::MidiMessageSequence& seq, int channel, int clipFilterStart = -1, const Clip* only = nullptr) {
    // reuse the engine's own note compilation semantics: loop repeats, patterns, swing, probability
    juce::ignoreUnused(clipFilterStart);
    for (auto& c : t.clips) {
        if (only && &c != only) continue;
        if (c.muted) continue;
        if (c.type == ClipType::Midi) {
            const int loopLen = std::max(1, c.contentLength - c.contentOffset);
            for (int rep = 0; rep * loopLen < c.length; ++rep)
                for (auto& n : c.notes) {
                    if (n.start < c.contentOffset || n.start >= c.contentLength) continue;
                    const int pos = (n.start - c.contentOffset) + rep * loopLen;
                    if (pos >= c.length) continue;
                    const int s = (only ? 0 : c.start) + pos;
                    const int e = std::min((only ? c.length : c.end()), s + n.length);
                    seq.addEvent(juce::MidiMessage::noteOn(channel, n.pitch, (juce::uint8)juce::jlimit(1, 127, (int)std::lround(n.velocity * 127))), s);
                    seq.addEvent(juce::MidiMessage::noteOff(channel, n.pitch), e);
                }
        } else if (c.type == ClipType::Pattern) {
            const Pattern* pat = p.findPattern(c.patternId);
            if (!pat) continue;
            const int plen = std::max(1, pat->lengthTicks());
            for (int rep = 0; rep * plen < c.length; ++rep)
                for (int pad = 0; pad < kDrumPads; ++pad)
                    for (int s = 0; s < pat->steps; ++s) {
                        const auto& st = pat->grid[(size_t)pad][(size_t)s];
                        if (!st.on) continue;
                        int pos = rep * plen + s * pat->stepTicks + ((s & 1) ? (int)(pat->swing * pat->stepTicks) : 0);
                        if (pos >= c.length) continue;
                        const int at = (only ? 0 : c.start) + pos;
                        seq.addEvent(juce::MidiMessage::noteOn(channel, 36 + pad, (juce::uint8)juce::jlimit(1, 127, (int)std::lround(st.velocity * 127))), at);
                        seq.addEvent(juce::MidiMessage::noteOff(channel, 36 + pad), at + pat->stepTicks / 2);
                    }
        }
    }
    seq.updateMatchedPairs();
}

bool exportFile(const Project& p, const juce::File& f, juce::String& error) {
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote(PPQ);
    juce::MidiMessageSequence tempo;
    addTempoTrack(p, tempo);
    mf.addTrack(tempo);
    int channel = 1, exported = 0;
    for (auto& t : p.tracks) {
        if (!t.hasInstrument()) continue;
        juce::MidiMessageSequence seq;
        seq.addEvent(juce::MidiMessage::textMetaEvent(3, t.name));
        const int ch = t.type == TrackType::Drum ? 10 : channel;
        notesToSeq(p, t, seq, ch);
        mf.addTrack(seq);
        ++exported;
        if (t.type != TrackType::Drum) { ++channel; if (channel == 10) ++channel; if (channel > 16) channel = 1; }
    }
    if (exported == 0) { error = "There are no instrument or drum tracks to export."; return false; }
    f.deleteFile();
    juce::FileOutputStream os(f);
    if (!os.openedOk() || !mf.writeTo(os, 1)) { error = "Could not write " + f.getFileName(); return false; }
    return true;
}

bool exportClip(const Project& p, const Clip& c, const juce::File& f, juce::String& error) {
    const Track* owner = nullptr;
    for (auto& t : p.tracks) for (auto& cc : t.clips) if (&cc == &c) owner = &t;
    if (!owner) { error = "Clip not found."; return false; }
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote(PPQ);
    juce::MidiMessageSequence tempo;
    addTempoTrack(p, tempo);
    mf.addTrack(tempo);
    juce::MidiMessageSequence seq;
    notesToSeq(p, *owner, seq, owner->type == TrackType::Drum ? 10 : 1, -1, &c);
    mf.addTrack(seq);
    f.deleteFile();
    juce::FileOutputStream os(f);
    if (!os.openedOk() || !mf.writeTo(os, 1)) { error = "Could not write " + f.getFileName(); return false; }
    return true;
}

bool importFile(Project& p, const juce::File& f, juce::String& error, int& added) {
    added = 0;
    juce::FileInputStream in(f);
    juce::MidiFile mf;
    if (!in.openedOk() || !mf.readFrom(in, true)) { error = "Not a valid MIDI file."; return false; }
    const int tpq = mf.getTimeFormat();
    if (tpq <= 0) { error = "SMPTE-timed MIDI files are not supported."; return false; }
    const double scale = (double)PPQ / tpq;
    const bool emptyProject = p.songEndTick() == 0;
    // tempo & meter
    juce::MidiMessageSequence tempos, sigs;
    mf.findAllTempoEvents(tempos);
    mf.findAllTimeSigEvents(sigs);
    if (emptyProject) {
        if (tempos.getNumEvents() > 0) {
            p.tempo.clear();
            for (int i = 0; i < tempos.getNumEvents(); ++i) {
                const auto& m = tempos.getEventPointer(i)->message;
                const double bpm = juce::jlimit(20.0, 300.0, 60.0 / m.getTempoSecondsPerQuarterNote());
                const int tick = (int)std::lround(m.getTimeStamp() * scale);
                if (!p.tempo.empty() && p.tempo.back().tick == tick) p.tempo.back().bpm = bpm;
                else p.tempo.push_back({ tick, bpm });
            }
            if (p.tempo.empty() || p.tempo.front().tick != 0) p.tempo.insert(p.tempo.begin(), TempoPoint { 0, p.tempo.empty() ? 120.0 : p.tempo.front().bpm });
        }
        if (sigs.getNumEvents() > 0) {
            p.timeSig.clear();
            for (int i = 0; i < sigs.getNumEvents(); ++i) {
                int num = 4, den = 4;
                const auto& m = sigs.getEventPointer(i)->message;
                m.getTimeSignatureInfo(num, den);
                const int tick = (int)std::lround(m.getTimeStamp() * scale);
                const int bar = p.timeSig.empty() ? 0 : p.tickToBar(tick);
                if (!p.timeSig.empty() && p.timeSig.back().bar == bar) { p.timeSig.back().num = num; p.timeSig.back().den = den; }
                else p.timeSig.push_back({ bar, juce::jlimit(1, 32, num), juce::jlimit(1, 32, den) });
            }
            if (p.timeSig.empty() || p.timeSig.front().bar != 0) p.timeSig.insert(p.timeSig.begin(), TimeSigPoint {});
        }
    }
    // notes: split per (track, channel)
    for (int ti = 0; ti < mf.getNumTracks(); ++ti) {
        auto seq = *mf.getTrack(ti);
        seq.updateMatchedPairs();
        juce::String trackName;
        for (int i = 0; i < seq.getNumEvents(); ++i) {
            const auto& m = seq.getEventPointer(i)->message;
            if (m.isTrackNameEvent()) { trackName = m.getTextFromTextMetaEvent(); break; }
        }
        std::map<int, std::vector<Note>> byChannel;
        std::map<int, int> program;
        for (int i = 0; i < seq.getNumEvents(); ++i) {
            auto* e = seq.getEventPointer(i);
            const auto& m = e->message;
            if (m.isProgramChange()) program[m.getChannel()] = m.getProgramChangeNumber();
            if (!m.isNoteOn()) continue;
            const int start = (int)std::lround(m.getTimeStamp() * scale);
            int end = start + PPQ / 4;
            if (e->noteOffObject) end = (int)std::lround(e->noteOffObject->message.getTimeStamp() * scale);
            byChannel[m.getChannel()].push_back({ start, std::max(1, end - start), m.getNoteNumber(), m.getFloatVelocity() });
        }
        for (auto& [ch, notes] : byChannel) {
            if (notes.empty()) continue;
            const bool drums = ch == 10;
            juce::String preset = "Grand Piano";
            if (!drums && program.count(ch)) {
                const int pg = program[ch];
                if (pg >= 24 && pg < 32) preset = pg < 26 ? "Acoustic Guitar" : "Electric Guitar";
                else if (pg >= 32 && pg < 40) preset = pg < 38 ? "Bass Guitar" : "Bass Synth";
                else if (pg >= 40 && pg < 52) preset = "Strings";
                else if (pg >= 52 && pg < 56) preset = "Choir";
                else if (pg >= 56 && pg < 64) preset = "Brass";
                else if (pg >= 16 && pg < 24) preset = "Organ";
                else if (pg >= 4 && pg < 6) preset = "Electric Piano";
                else if (pg >= 8 && pg < 16) preset = pg == 14 ? "Bells" : "Mallets";
                else if (pg >= 80 && pg < 88) preset = "Lead Synth";
                else if (pg >= 88 && pg < 96) preset = "Pad Synth";
            }
            Track& t = p.addTrack(drums ? TrackType::Drum : TrackType::Instrument, {}, drums ? juce::String("Studio Kit") : preset);
            if (trackName.isNotEmpty()) t.name = trackName + (byChannel.size() > 1 ? " Ch" + juce::String(ch) : juce::String());
            if (drums) {
                // map General MIDI drum notes onto the 16 pads
                static const std::map<int, int> gm = { { 35, 0 }, { 36, 0 }, { 38, 1 }, { 40, 1 }, { 39, 2 }, { 42, 3 }, { 44, 10 }, { 46, 4 },
                                                       { 41, 5 }, { 43, 5 }, { 45, 6 }, { 47, 6 }, { 48, 7 }, { 50, 7 }, { 49, 8 }, { 57, 8 },
                                                       { 51, 9 }, { 59, 9 }, { 53, 11 }, { 37, 12 }, { 54, 13 }, { 63, 14 }, { 64, 14 }, { 81, 15 } };
                for (auto& n : notes) { auto it = gm.find(n.pitch); n.pitch = 36 + (it != gm.end() ? it->second : (n.pitch % 16)); }
            }
            int end = 0;
            for (auto& n : notes) end = std::max(end, n.start + n.length);
            const int bar = p.ticksPerBarAt(0);
            Clip c = p.makeMidiClip(0, std::max(bar, ((end + bar - 1) / bar) * bar));
            c.notes = notes;
            c.name = f.getFileNameWithoutExtension();
            t.clips.push_back(c);
            ++added;
        }
    }
    if (added == 0) { error = "The MIDI file contains no notes."; return false; }
    return true;
}

}  // namespace MidiIO
