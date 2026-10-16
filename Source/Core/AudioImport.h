#pragma once
// Imports audio files dropped on the timeline into a take, as new tracks.
//
//   mono file        -> one mono track
//   stereo file      -> one stereo track
//   polyphonic file  -> one mono track per channel ("Show 1", "Show 2"...)
//
// Every file is converted to the session's format (BWF, 24-bit PCM or 32-bit float) at the
// device's sample rate, so the take plays back exactly like a recorded one. A drop position
// after zero is written as leading silence, keeping every file of the take sample-aligned.
// Runs on a worker thread; the source files are never modified.

#include "BwfWriter.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <functional>

namespace omr
{
struct ImportOutput
{
    std::vector<int> sourceChannels;   // 1 (mono) or 2 (stereo) channels of the source file
    juce::String trackName;
};

struct ImportSource
{
    juce::File file;
    double sampleRate = 0;
    int64_t lengthFrames = 0;
    int numChannels = 0;
    std::vector<ImportOutput> outputs;
};

struct ImportPlan
{
    std::vector<ImportSource> sources;
    juce::StringArray skipped;   // "name.mp3: not a WAV or AIFF file"
    int numTracks() const;
};

bool isImportableFile (const juce::File& f);   // .wav .wave .bwf .aif .aiff
ImportPlan planImport (const juce::StringArray& paths);

struct ImportResult
{
    bool ok = false;
    juce::String error;
    std::vector<juce::File> written;   // one per output, in order
    int64_t framesWritten = 0;         // per file, including the leading silence
};

// Converts one source into its outputs (one pass over the source).
// `progress` receives 0..1 for this source; `cancel` aborts and deletes partial files.
ImportResult importSource (const ImportSource& src, const std::vector<juce::File>& destinations,
                           double targetRate, int bitsPerSample, int64_t offsetFrames,
                           const std::atomic<bool>* cancel = nullptr,
                           const std::function<void (double)>& progress = {});
} // namespace omr
