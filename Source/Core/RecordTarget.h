#pragma once
// One storage target (primary or secondary): its own SPSC rings, its own writer thread,
// its own journal. A target that stalls or fails never affects the other target or
// the audio callback.

#include "BwfWriter.h"
#include "EventLog.h"
#include "PeakStore.h"
#include "PatchModel.h"
#include "SpscRing.h"
#include <juce_core/juce_core.h>

namespace omr
{
struct TakeSpec
{
    int take = 1;
    juce::String sessionName;
    double sampleRate = 48000.0;
    int bitsPerSample = 24;
    uint64_t timeReference = 0;
    juce::String date, time;
    struct Track { int index = 0; juce::String name; int channels = 1; int id = 0; };
    std::vector<Track> tracks;  // armed tracks
    size_t ringFrames = 48000 * 20;
};

class RecordTarget : private juce::Thread
{
public:
    RecordTarget (int index, const juce::File& sessionRoot, EventLog& log);
    ~RecordTarget() override;

    // Control thread, before the audio thread can see this target.
    juce::Result prepare (const TakeSpec& spec);
    void start();

    // Audio thread. Never blocks: if the ring is full the frames are dropped and a Gap is recorded,
    // which the writer turns into silence so every file stays sample-aligned.
    // `right` is used only by stereo tracks (nullptr = repeat left).
    void push (int track, const float* left, const float* right, size_t n, uint64_t takeFramePos, int device = 0) noexcept;

    // Control thread, after the audio thread has stopped calling push().
    void flushOpenGaps() noexcept;
    void finish();          // drain rings, close files, write journal "stop"

    // Status (any thread).
    std::atomic<bool> active { false };
    std::atomic<bool> failed { false };
    std::atomic<int> testStallMs { 0 };          // fault injection for tests
    float maxRingFill() const noexcept;           // 0..1 across this target's tracks
    uint64_t bytesWritten() const noexcept { return written.load(); }
    float worstWriteMs() const noexcept { return worstMs.load(); }
    uint64_t droppedFrames() const noexcept { return dropped.load(); }   // summed over tracks
    juce::String label() const { return index == 0 ? "A" : "B"; }
    const juce::File& root() const noexcept { return sessionRoot; }
    juce::StringArray filePaths() const;
    // Live waveform + file for a recording track (null if the track is not recording here).
    std::shared_ptr<PeakStore> peaksFor (int track) const;
    juce::File fileFor (int track) const;

private:
    struct TrackIO
    {
        int track = 0;
        int channels = 1;         // ring holds interleaved samples; gaps are in samples, always whole frames
        std::unique_ptr<SpscRing<float>> ring;
        std::unique_ptr<SpscQueue<Gap>> gaps;
        Gap open;                 // producer-side, not yet published
        std::unique_ptr<BwfWriter> file;
        std::shared_ptr<PeakStore> peaks;   // live waveform, written by this target's writer thread
    };

    void run() override;
    size_t readWithGaps (TrackIO& io, float* dst, size_t max) noexcept;
    void syncAll();
    void fail (const juce::String& what, const BwfWriter* w);
    void journal (const juce::var& v);

    const int index;
    const juce::File sessionRoot;
    EventLog& log;
    TakeSpec spec;
    std::vector<std::unique_ptr<TrackIO>> byTrack;  // kMaxTracks slots, null = not recording
    std::vector<TrackIO*> list;
    std::atomic<bool> draining { false };
    std::atomic<uint64_t> written { 0 }, dropped { 0 };
    std::atomic<float> worstMs { 0 };
    bool finished = true;
};
} // namespace omr
