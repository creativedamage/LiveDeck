#pragma once
// Crash recovery: rebuilds WAV/RF64 headers from the real file length.
// Files are written with keep-size preallocation, so the length of a crashed
// file is exactly the audio that reached the disk.

#include <juce_core/juce_core.h>

namespace omr
{
struct RecoveredFile
{
    juce::File file;
    uint64_t frames = 0;
    bool repaired = false;   // header was stale and has been rewritten
    bool ok = false;
    juce::String message;
};

RecoveredFile repairWavFile (const juce::File& wav);

// Repairs every .wav under <sessionRoot>/Audio and appends a "recovered" entry to the journal.
juce::Array<RecoveredFile> recoverSessionFolder (const juce::File& sessionRoot);

// True when the journal shows a take that started but never logged a clean stop.
bool journalHasUncleanTake (const juce::File& sessionRoot);
} // namespace omr
