// Apple-only services: AAC encoding (ExtAudioFile) and haptic feedback (UIKit).
// NOTE: this file is compiled only on macOS/iOS and could not be built on the Linux
// machine used for development; it uses only long-stable Apple APIs.
#include "Platform/PlatformAudio.h"   // not Exporter.h: the project's Marker type clashes with CarbonCore's
#include <AudioToolbox/AudioToolbox.h>
#if JUCE_IOS
 #import <UIKit/UIKit.h>
#endif

namespace PlatformAudio {

bool encodeAAC(const juce::AudioBuffer<float>& audio, double sampleRate, int kbps, const juce::File& out, juce::String& error) {
    const auto path = out.withFileExtension("m4a");
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.getFullPathName().toRawUTF8(),
                                                            (CFIndex)strlen(path.getFullPathName().toRawUTF8()), false);
    AudioStreamBasicDescription dst {};
    dst.mFormatID = kAudioFormatMPEG4AAC;
    dst.mSampleRate = sampleRate;
    dst.mChannelsPerFrame = 2;
    ExtAudioFileRef file = nullptr;
    OSStatus st = ExtAudioFileCreateWithURL(url, kAudioFileM4AType, &dst, nullptr, kAudioFileFlags_EraseFile, &file);
    CFRelease(url);
    if (st != noErr || !file) { error = "The AAC encoder is not available (" + juce::String((int)st) + ")."; return false; }

    AudioStreamBasicDescription src {};
    src.mFormatID = kAudioFormatLinearPCM;
    src.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    src.mSampleRate = sampleRate;
    src.mChannelsPerFrame = 2;
    src.mBitsPerChannel = 32;
    src.mBytesPerFrame = 8;
    src.mFramesPerPacket = 1;
    src.mBytesPerPacket = 8;
    st = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(src), &src);
    if (st == noErr) {
        AudioConverterRef conv = nullptr;
        UInt32 sz = sizeof(conv);
        if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_AudioConverter, &sz, &conv) == noErr && conv) {
            UInt32 bitrate = (UInt32)kbps * 1000;
            AudioConverterSetProperty(conv, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate);
            CFArrayRef cfg = nullptr;
            ExtAudioFileSetProperty(file, kExtAudioFileProperty_ConverterConfig, sizeof(cfg), &cfg);
        }
    }
    const int n = audio.getNumSamples();
    std::vector<float> inter(4096 * 2);
    for (int pos = 0; pos < n && st == noErr; pos += 4096) {
        const int len = std::min(4096, n - pos);
        for (int i = 0; i < len; ++i) {
            inter[(size_t)i * 2] = audio.getSample(0, pos + i);
            inter[(size_t)i * 2 + 1] = audio.getSample(1, pos + i);
        }
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 2;
        abl.mBuffers[0].mDataByteSize = (UInt32)(len * 8);
        abl.mBuffers[0].mData = inter.data();
        st = ExtAudioFileWrite(file, (UInt32)len, &abl);
    }
    ExtAudioFileDispose(file);
    if (st != noErr) { error = "AAC encoding failed (" + juce::String((int)st) + ")."; return false; }
    if (path != out) path.moveFileTo(out);
    return true;
}

void haptic(int strength) {
   #if JUCE_IOS
    dispatch_async(dispatch_get_main_queue(), ^{
        UIImpactFeedbackStyle style = strength >= 2 ? UIImpactFeedbackStyleHeavy : (strength == 1 ? UIImpactFeedbackStyleMedium : UIImpactFeedbackStyleLight);
        UIImpactFeedbackGenerator* g = [[UIImpactFeedbackGenerator alloc] initWithStyle:style];
        [g impactOccurred];
    });
   #else
    juce::ignoreUnused(strength);
   #endif
}

bool hapticsAvailable() {
   #if JUCE_IOS
    return true;
   #else
    return false;
   #endif
}

}  // namespace PlatformAudio
