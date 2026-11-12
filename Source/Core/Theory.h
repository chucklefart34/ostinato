// Music theory and editing helpers: scales, chords, quantize, transpose, key & tempo detection.
#pragma once
#include "Project.h"
#include "Engine/Samples.h"

namespace Theory {
struct Scale { const char* name; const char* steps; };   // 12-char mask, '1' = in scale
const std::vector<Scale>& scales();                       // last entry = Custom
bool inScale(const Project& p, int pitch);
juce::String noteName(int pitch, bool withOctave = true);
const juce::StringArray& keyNames();

struct ChordType { const char* name; const char* symbol; std::vector<int> intervals; };
const std::vector<ChordType>& chords();
std::vector<int> chordPitches(int root, int chordIndex, int inversion = 0);
// Diatonic triad/seventh built on a scale degree (0-based) of the project key/scale.
std::vector<int> diatonicChord(const Project& p, int degree, int octave, bool seventh);
juce::String diatonicChordName(const Project& p, int degree, bool seventh);

// Editing operations on note lists (positions relative to clip start).
void quantize(std::vector<Note>& notes, int grid, float strength, float swing, bool ends, bool selectedOnly);
void humanize(std::vector<Note>& notes, int timingTicks, float velocity, bool selectedOnly, uint32_t seed);
void transpose(std::vector<Note>& notes, int semitones, bool selectedOnly);
void scaleVelocity(std::vector<Note>& notes, float factor, float offset, bool selectedOnly);
void legato(std::vector<Note>& notes, bool selectedOnly);

// Detection
struct KeyGuess { int key = 0; bool minor = false; float confidence = 0.f; };
KeyGuess detectKey(const std::vector<Note>& notes);                 // Krumhansl-Schmuckler
double detectBpm(const SampleData& s, double minBpm = 70.0, double maxBpm = 180.0);   // onset autocorrelation, 0 if unsure

class TapTempo {
public:
    // Returns the current estimate (0 until two taps).
    double tap(double nowMs);
private:
    std::vector<double> taps;
};
}  // namespace Theory
