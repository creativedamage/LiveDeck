#pragma once
// Multitrack playback (virtual soundcheck): the record path in reverse.
// A reader thread streams each track's BWF file into its own SPSC ring; the audio
// callback only pops samples from the rings. A slow disk causes an underrun
// (silence + a logged event), never a stalled callback.

#include "EventLog.h"
#include "PatchModel.h"
#include "SpscRing.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>

namespace omr
{
// Takes found in a session folder: take number -> (track index -> file). Primary files only.
std::map<int, std::map<int, juce::File>> scanTakes (const juce::File& sessionRoot);

// Files of a take mapped to the CURRENT rows of `patch`: uses the take manifest (track ids),
// so reordered/deleted tracks keep their own audio. Takes without a manifest map by position.
std::map<int, juce::File> resolveTakeFiles (const juce::File& sessionRoot, int take, const PatchState& patch);

// Takes recorded before manifests existed: write one now from the current (positional) layout,
// so the tracks keep their audio through later reorders. Returns true if a manifest was written.
bool writeManifestIfMissing (const juce::File& sessionRoot, int take, const PatchState& patch);

class Player : private juce::Thread
{
public:
    explicit Player (EventLog& log);
    ~Player() override;

    // Control thread. Opens every file of the take, pre-fills the rings from `startFrame`.
    juce::Result load (const juce::File& sessionRoot, int take, double deviceSampleRate, uint64_t startFrame); // files by position
    juce::Result load (const std::map<int, juce::File>& filesByRow, int take, double deviceSampleRate, uint64_t startFrame);
    void start();      // begins streaming; call after load()
    void halt();       // stops the reader thread

    // Audio thread.
    bool hasTrack (int track) const noexcept { return track >= 0 && track < kMaxTracks && tracks[(size_t) track] != nullptr; }
    // Fills dstL (and dstR for stereo files); returns the file's channel count (1 or 2).
    // Underruns produce zeros and are skipped later so tracks stay aligned.
    int readTrack (int track, float* dstL, float* dstR, int n, uint64_t posForEvents) noexcept;
    void advance (int n) noexcept;

    uint64_t position() const noexcept { return pos.load (std::memory_order_relaxed); }
    uint64_t length() const noexcept { return lengthFrames; }
    bool finished() const noexcept { return pos.load (std::memory_order_relaxed) >= lengthFrames; }
    int take() const noexcept { return takeNumber; }
    int numTracks() const noexcept { return (int) list.size(); }
    uint64_t underrunFrames() const noexcept { return underruns.load(); }

private:
    struct TrackIn
    {
        int track = 0;
        int channels = 1;
        std::vector<float> interleaved;
        std::unique_ptr<juce::AudioFormatReader> reader;
        std::unique_ptr<SpscRing<float>> ring;
        juce::AudioBuffer<float> chunk;
        uint64_t readPos = 0;   // next frame to read from the file
        uint64_t length = 0;
        uint64_t owed = 0;      // audio thread: frames lost to an underrun, skipped on the next read to stay aligned
    };

    void run() override;
    bool fillOnce (TrackIn& t);

    EventLog& log;
    juce::AudioFormatManager formats;
    std::array<std::unique_ptr<TrackIn>, kMaxTracks> tracks {};
    std::vector<TrackIn*> list;
    std::atomic<uint64_t> pos { 0 }, underruns { 0 };
    uint64_t lengthFrames = 0;
    int takeNumber = 0;
    bool underrunLogged = false;
};
} // namespace omr
