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

// One track's audio in a take: a file that starts `offset` frames after the start of the take
// (recorded files start at 0; dropped / moved files can start later).
struct Clip
{
    juce::File file;
    uint64_t offset = 0;
    juce::String name;   // what to show on the clip: the imported file's name, else the file's own name
};

// Clips of a take mapped to the CURRENT rows of `patch` (via the manifest's track ids).
std::map<int, Clip> resolveTakeClips (const juce::File& sessionRoot, int take, const PatchState& patch);

// Files of a take mapped to the CURRENT rows of `patch`: uses the take manifest (track ids),
// so reordered/deleted tracks keep their own audio. Takes without a manifest map by position.
std::map<int, juce::File> resolveTakeFiles (const juce::File& sessionRoot, int take, const PatchState& patch);

// Takes recorded before manifests existed: write one now from the current (positional) layout,
// so the tracks keep their audio through later reorders. Returns true if a manifest was written.
bool writeManifestIfMissing (const juce::File& sessionRoot, int take, const PatchState& patch);

// Adds files to a take's manifest (creating it if needed). Used by audio import.
struct ManifestEntry { int id = 0, row = 0, channels = 1; juce::String name; juce::File file; uint64_t offset = 0; juce::String source; };
bool appendToManifest (const juce::File& sessionRoot, int take, const std::vector<ManifestEntry>& entries);

// Clip editing (message thread). All edits only rewrite the small take manifest: audio files are never
// changed or deleted, so every edit can be undone by restoring the previous manifest text.
juce::String readManifestText (const juce::File& sessionRoot, int take);            // "" if none
bool restoreManifestText (const juce::File& sessionRoot, int take, const juce::String& text);   // "" deletes it
// Puts `e` on track e.id, replacing whatever that track had in this take.
bool setClip (const juce::File& sessionRoot, int take, const ManifestEntry& e);
// Moves track fromId's clip to track toId at newOffset. If toId already has a clip: swap = it moves
// to fromId (keeping its own offset), otherwise it is unlinked from the take.
bool moveClip (const juce::File& sessionRoot, int take, int fromId, int toId, uint64_t newOffset, bool swap);
bool removeClip (const juce::File& sessionRoot, int take, int trackId);   // unlinks; the file stays on disk
// Several clips at once: unlink them, or move them all by the same number of frames (never before 0).
bool removeClips (const juce::File& sessionRoot, int take, const std::vector<int>& trackIds);
bool shiftClips (const juce::File& sessionRoot, int take, const std::vector<int>& trackIds, juce::int64 deltaFrames);

// Length of a take in frames: the end of its last clip.
uint64_t takeLength (const std::map<int, juce::File>& files);
uint64_t takeLength (const std::map<int, Clip>& clips);

class Player : private juce::Thread
{
public:
    explicit Player (EventLog& log);
    ~Player() override;

    // Control thread. Opens every file of the take, pre-fills the rings from `startFrame`.
    juce::Result load (const juce::File& sessionRoot, int take, double deviceSampleRate, uint64_t startFrame); // files by position
    juce::Result load (const std::map<int, juce::File>& filesByRow, int take, double deviceSampleRate, uint64_t startFrame);
    juce::Result load (const std::map<int, Clip>& clipsByRow, int take, double deviceSampleRate, uint64_t startFrame);
    void start();      // begins streaming; call after load()
    void halt();       // stops the reader thread

    // Audio thread.
    bool hasTrack (int track) const noexcept { return track >= 0 && track < kMaxTracks && tracks[(size_t) track] != nullptr; }
    // Fills dstL (and dstR for stereo files); returns the file's channel count (1 or 2).
    // Underruns produce zeros and are skipped later so tracks stay aligned.
    // `device` = the calling device: each device advances its own position (they may drift).
    int readTrack (int track, float* dstL, float* dstR, int n, uint64_t posForEvents, int device = 0) noexcept;
    void advance (int device, int n) noexcept;
    void advance (int n) noexcept { advance (0, n); }

    uint64_t position (int device = 0) const noexcept { return pos[(size_t) device].load (std::memory_order_relaxed); }
    uint64_t length() const noexcept { return lengthFrames; }
    bool finished() const noexcept { return position (0) >= lengthFrames; }   // the primary device is the clock
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
        uint64_t readPos = 0;   // next take frame to stream (frames before `offset` are silence)
        uint64_t offset = 0;    // where the file starts in the take
        uint64_t length = 0;    // offset + file length
        uint64_t owed = 0;      // audio thread: frames lost to an underrun, skipped on the next read to stay aligned
    };

    void run() override;
    bool fillOnce (TrackIn& t);

    EventLog& log;
    juce::AudioFormatManager formats;
    std::array<std::unique_ptr<TrackIn>, kMaxTracks> tracks {};
    std::vector<TrackIn*> list;
    std::array<std::atomic<uint64_t>, kMaxDevices> pos {};
    std::atomic<uint64_t> underruns { 0 };
    uint64_t lengthFrames = 0;
    int takeNumber = 0;
    std::atomic<bool> underrunLogged { false };
};
} // namespace omr
