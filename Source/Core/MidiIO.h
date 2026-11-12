// Standard MIDI File import/export.
#pragma once
#include "Project.h"

namespace MidiIO {
// Exports instrument + drum tracks as a type-1 SMF with tempo and time signature maps.
bool exportFile(const Project& p, const juce::File& f, juce::String& error);
// Imports into `p` (adds tracks; sets tempo/time signature when the project is empty).
bool importFile(Project& p, const juce::File& f, juce::String& error, int& tracksAdded);
// Single-clip helpers used by the piano roll.
bool exportClip(const Project& p, const Clip& c, const juce::File& f, juce::String& error);
}
