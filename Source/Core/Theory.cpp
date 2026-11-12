#include "Theory.h"
#include <cmath>
#include <numeric>

namespace Theory {

const std::vector<Scale>& scales() {
    static const std::vector<Scale> s = {
        { "Major", "101011010101" }, { "Minor", "101101011010" }, { "Harmonic Minor", "101101011001" },
        { "Melodic Minor", "101101010101" }, { "Major Pentatonic", "101010010100" }, { "Minor Pentatonic", "100101010010" },
        { "Blues", "100101110010" }, { "Dorian", "101101010110" }, { "Phrygian", "110101011010" }, { "Lydian", "101010110101" },
        { "Mixolydian", "101011010110" }, { "Locrian", "110101101010" }, { "Chromatic", "111111111111" }, { "Custom", "" },
    };
    return s;
}

bool inScale(const Project& p, int pitch) {
    const auto& sc = scales();
    const int idx = juce::jlimit(0, (int)sc.size() - 1, p.scale);
    const juce::String mask = sc[(size_t)idx].steps[0] ? juce::String(sc[(size_t)idx].steps) : p.customScale;
    const int deg = ((pitch - p.key) % 12 + 12) % 12;
    return mask.length() == 12 && mask[deg] == '1';
}

const juce::StringArray& keyNames() {
    static const juce::StringArray k { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return k;
}

juce::String noteName(int pitch, bool oct) {
    pitch = juce::jlimit(0, 127, pitch);
    return keyNames()[pitch % 12] + (oct ? juce::String(pitch / 12 - 1) : juce::String());
}

const std::vector<ChordType>& chords() {
    static const std::vector<ChordType> c = {
        { "Major", "", { 0, 4, 7 } }, { "Minor", "m", { 0, 3, 7 } }, { "Diminished", "dim", { 0, 3, 6 } }, { "Augmented", "aug", { 0, 4, 8 } },
        { "Major 7th", "maj7", { 0, 4, 7, 11 } }, { "Minor 7th", "m7", { 0, 3, 7, 10 } }, { "Dominant 7th", "7", { 0, 4, 7, 10 } },
        { "Diminished 7th", "dim7", { 0, 3, 6, 9 } }, { "Half-diminished", "m7b5", { 0, 3, 6, 10 } },
        { "Major 9th", "maj9", { 0, 4, 7, 11, 14 } }, { "Minor 9th", "m9", { 0, 3, 7, 10, 14 } }, { "Dominant 9th", "9", { 0, 4, 7, 10, 14 } },
        { "Suspended 2nd", "sus2", { 0, 2, 7 } }, { "Suspended 4th", "sus4", { 0, 5, 7 } }, { "7sus4", "7sus4", { 0, 5, 7, 10 } },
        { "Add 9", "add9", { 0, 4, 7, 14 } }, { "Power (5th)", "5", { 0, 7, 12 } },
    };
    return c;
}

std::vector<int> chordPitches(int root, int idx, int inversion) {
    const auto& c = chords()[(size_t)juce::jlimit(0, (int)chords().size() - 1, idx)];
    std::vector<int> v;
    for (int i : c.intervals) v.push_back(root + i);
    for (int k = 0; k < inversion && !v.empty(); ++k) { v.push_back(v.front() + 12); v.erase(v.begin()); }
    for (auto& x : v) x = juce::jlimit(0, 127, x);
    return v;
}

static std::vector<int> scaleDegrees(const Project& p) {
    const auto& sc = scales();
    const int idx = juce::jlimit(0, (int)sc.size() - 1, p.scale);
    const juce::String mask = sc[(size_t)idx].steps[0] ? juce::String(sc[(size_t)idx].steps) : p.customScale;
    std::vector<int> d;
    for (int i = 0; i < 12 && i < mask.length(); ++i) if (mask[i] == '1') d.push_back(i);
    if (d.empty()) d = { 0, 2, 4, 5, 7, 9, 11 };
    return d;
}

std::vector<int> diatonicChord(const Project& p, int degree, int octave, bool seventh) {
    const auto d = scaleDegrees(p);
    const int n = (int)d.size();
    std::vector<int> out;
    for (int k = 0; k < (seventh ? 4 : 3); ++k) {
        const int idx = degree + k * 2;
        const int pc = d[(size_t)(idx % n)] + 12 * (idx / n);
        out.push_back(juce::jlimit(0, 127, 12 * (octave + 1) + p.key + pc));
    }
    return out;
}

juce::String diatonicChordName(const Project& p, int degree, bool seventh) {
    auto v = diatonicChord(p, degree, 4, seventh);
    if (v.size() < 3) return "?";
    const int a = v[1] - v[0], b = v[2] - v[0];
    juce::String q = (a == 4 && b == 7) ? "" : (a == 3 && b == 7) ? "m" : (a == 3 && b == 6) ? "dim" : (a == 4 && b == 8) ? "aug" : "?";
    if (seventh && v.size() > 3) {
        const int s = v[3] - v[0];
        if (q == "" && s == 11) q = "maj7"; else if (q == "" && s == 10) q = "7"; else if (q == "m" && s == 10) q = "m7";
        else if (q == "dim" && s == 10) q = "m7b5"; else if (q == "dim" && s == 9) q = "dim7"; else q << "7";
    }
    static const char* roman[] = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII" };
    return noteName(v[0], false) + q + " (" + roman[juce::jlimit(0, 11, degree)] + ")";
}

void quantize(std::vector<Note>& notes, int grid, float strength, float swing, bool ends, bool selOnly) {
    if (grid <= 1) return;
    for (auto& n : notes) {
        if (selOnly && !n.selected) continue;
        const int idx = (int)std::floor((n.start + grid * 0.5) / grid);
        int target = idx * grid;
        if (idx & 1) target += (int)(swing * grid * 0.5f);
        const int ns = n.start + (int)std::lround((target - n.start) * strength);
        if (ends) {
            const int end = n.start + n.length;
            const int qe = (int)std::lround((double)end / grid) * grid;
            const int ne = end + (int)std::lround((qe - end) * strength);
            n.length = std::max(grid / 4, ne - ns);
        }
        n.start = std::max(0, ns);
    }
}

void humanize(std::vector<Note>& notes, int timing, float vel, bool selOnly, uint32_t seed) {
    juce::Random r((juce::int64)seed);
    for (auto& n : notes) {
        if (selOnly && !n.selected) continue;
        n.start = std::max(0, n.start + (int)std::lround((r.nextFloat() * 2.f - 1.f) * timing));
        n.velocity = juce::jlimit(0.05f, 1.f, n.velocity + (r.nextFloat() * 2.f - 1.f) * vel);
    }
}

void transpose(std::vector<Note>& notes, int st, bool selOnly) {
    for (auto& n : notes) if (!selOnly || n.selected) n.pitch = juce::jlimit(0, 127, n.pitch + st);
}

void scaleVelocity(std::vector<Note>& notes, float f, float off, bool selOnly) {
    for (auto& n : notes) if (!selOnly || n.selected) n.velocity = juce::jlimit(0.02f, 1.f, n.velocity * f + off);
}

void legato(std::vector<Note>& notes, bool selOnly) {
    std::vector<Note*> v;
    for (auto& n : notes) if (!selOnly || n.selected) v.push_back(&n);
    std::sort(v.begin(), v.end(), [](Note* a, Note* b) { return a->start < b->start; });
    for (size_t i = 0; i + 1 < v.size(); ++i) {
        size_t j = i + 1;
        while (j < v.size() && v[j]->start == v[i]->start) ++j;
        if (j < v.size()) v[i]->length = std::max(1, v[j]->start - v[i]->start);
    }
}

KeyGuess detectKey(const std::vector<Note>& notes) {
    static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    double hist[12] = {};
    for (auto& n : notes) hist[n.pitch % 12] += (double)n.length * (0.5 + n.velocity);
    double total = 0; for (double h : hist) total += h;
    KeyGuess g;
    if (total <= 0) return g;
    auto corr = [&](const double* prof, int key) {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += hist[i]; my += prof[i]; }
        mx /= 12; my /= 12;
        double num = 0, dx = 0, dy = 0;
        for (int i = 0; i < 12; ++i) {
            const double x = hist[(i + key) % 12] - mx, y = prof[i] - my;
            num += x * y; dx += x * x; dy += y * y;
        }
        return num / std::sqrt(std::max(1e-12, dx * dy));
    };
    double best = -2, second = -2;
    for (int k = 0; k < 12; ++k)
        for (int m = 0; m < 2; ++m) {
            const double c = corr(m ? mnr : maj, k);
            if (c > best) { second = best; best = c; g.key = k; g.minor = m == 1; }
            else if (c > second) second = c;
        }
    g.confidence = (float)juce::jlimit(0.0, 1.0, (best - second) * 5.0 + best * 0.5);
    return g;
}

double detectBpm(const SampleData& s, double minBpm, double maxBpm) {
    // onset strength envelope at ~172 Hz frame rate, then autocorrelation over the tempo range
    const int hop = std::max(1, (int)(s.sampleRate / 172.0));
    const int frames = s.frames() / hop;
    if (frames < 172 * 3) return 0.0;
    std::vector<float> env((size_t)frames), onset((size_t)frames);
    for (int f = 0; f < frames; ++f) {
        double e = 0;
        for (int c = 0; c < s.buffer.getNumChannels(); ++c) {
            const float* d = s.buffer.getReadPointer(c, f * hop);
            for (int i = 0; i < hop; ++i) e += d[i] * d[i];
        }
        env[(size_t)f] = (float)std::log1p(1000.0 * std::sqrt(e / hop));
    }
    for (int f = 1; f < frames; ++f) onset[(size_t)f] = std::max(0.f, env[(size_t)f] - env[(size_t)f - 1]);
    const double fps = s.sampleRate / hop;
    const int lagMin = (int)(fps * 60.0 / maxBpm), lagMax = (int)(fps * 60.0 / minBpm);
    double bestScore = 0, bestLag = 0, mean = 0;
    std::vector<double> score((size_t)lagMax + 2, 0.0);
    for (int lag = lagMin; lag <= lagMax && lag < frames / 2; ++lag) {
        double acc = 0;
        for (int f = lag; f < frames; ++f) acc += onset[(size_t)f] * onset[(size_t)(f - lag)];
        // reward lags whose double also correlates (beat-level periodicity)
        if (lag * 2 < frames) for (int f = lag * 2; f < frames; f += 3) acc += 0.5 * onset[(size_t)f] * onset[(size_t)(f - lag * 2)] * 3;
        score[(size_t)lag] = acc;
        mean += acc;
        if (acc > bestScore) { bestScore = acc; bestLag = lag; }
    }
    mean /= std::max(1, lagMax - lagMin + 1);
    if (bestLag <= 0 || bestScore < mean * 1.15) return 0.0;
    // parabolic interpolation for sub-frame accuracy
    const int L = (int)bestLag;
    double lag = L;
    if (L > lagMin && L < lagMax) {
        const double a = score[(size_t)L - 1], b = score[(size_t)L], c = score[(size_t)L + 1];
        const double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) lag = L + 0.5 * (a - c) / den;
    }
    return std::round(60.0 * fps / lag * 10.0) / 10.0;
}

double TapTempo::tap(double now) {
    if (!taps.empty() && now - taps.back() > 2000.0) taps.clear();
    taps.push_back(now);
    if (taps.size() > 8) taps.erase(taps.begin());
    if (taps.size() < 2) return 0.0;
    const double avg = (taps.back() - taps.front()) / (double)(taps.size() - 1);
    return juce::jlimit(20.0, 300.0, 60000.0 / avg);
}

}  // namespace Theory
