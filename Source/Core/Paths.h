// Storage locations. On iOS, Documents is exposed in the Files app (FILE_SHARING_ENABLED),
// so projects, exports and imports can be moved in and out without a computer.
#pragma once
#include <juce_core/juce_core.h>

namespace Paths {
inline juce::File& overrideRoot() { static juce::File f; return f; }   // tests use a temp dir

inline juce::File root() {
    if (overrideRoot() != juce::File()) return overrideRoot();
   #if JUCE_IOS || JUCE_ANDROID
    auto base = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
   #else
    auto base = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Ostinato");
   #endif
    base.createDirectory();
    return base;
}
inline juce::File sub(const char* name) { auto d = root().getChildFile(name); d.createDirectory(); return d; }
inline juce::File projects() { return sub("Projects"); }
inline juce::File library() { return sub("Library"); }
inline juce::File imports() { return library().getChildFile("Imported"); }
inline juce::File exports() { return sub("Exports"); }
inline juce::File presets() { return sub("Presets"); }
inline juce::File templates() { return sub("Templates"); }
inline juce::File support() {
    auto d = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
    #if ! (JUCE_IOS || JUCE_ANDROID)
                 .getChildFile("Ostinato")
    #endif
        ;
    if (overrideRoot() != juce::File()) d = overrideRoot().getChildFile(".support");
    d.createDirectory();
    return d;
}
inline juce::File recovery() { auto d = support().getChildFile("Recovery"); d.createDirectory(); return d; }
inline juce::File settingsFile() { return support().getChildFile("settings.json"); }
}  // namespace Paths
