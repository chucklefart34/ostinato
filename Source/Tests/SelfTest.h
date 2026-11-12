#pragma once
#include <juce_core/juce_core.h>

// Headless verification suite: renders real audio through the engine and measures it.
// Run with:  Ostinato --selftest [report.txt]
int runSelfTests(const juce::File& report);
