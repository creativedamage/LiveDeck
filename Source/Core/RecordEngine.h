#pragma once
// The real-time heart: one process() per device callback. It only reads an immutable
// RoutingSnapshot, copies samples into SPSC rings, updates atomic meters and builds
// direct outs + the monitor mix. Everything that can block lives elsewhere.

#include "EventLog.h"
#include "PatchModel.h"
#include "Player.h"
#include "RecordTarget.h"
#include "Session.h"

namespace omr
{
struct TrackMeter
{
    std::atomic<float> peak { 0 }, sumSq { 0 };
    std::atomic<uint32_t> count { 0 };
    std::atomic<bool> clip { false };

    void update (const float* x, int n) noexcept
    {
        float p = 0, s = 0;
        for (int i = 0; i < n; ++i) { const float a = std::abs (x[i]); p = std::max (p, a); s += x[i] * x[i]; }
        if (p > peak.load (std::memory_order_relaxed)) peak.store (p, std::memory_order_relaxed);
        sumSq.store (sumSq.load (std::memory_order_relaxed) + s, std::memory_order_relaxed);
        count.store (count.load (std::memory_order_relaxed) + (uint32_t) n, std::memory_order_relaxed);
        if (p >= 0.999f) clip.store (true, std::memory_order_relaxed);
    }
};

struct MeterReading { float peak = 0, rms = 0; bool clip = false; };

class RecordEngine
{
public:
    RecordEngine();
    ~RecordEngine();

    // ---- control / message thread
    void deviceStarting (double sampleRate, int blockSize);
    void deviceStopped();
    void setPatch (const PatchState& patch);
    juce::Result startRecording (SessionSettings& session, const PatchState& patch);
    void stopRecording();
    bool isRecording() const noexcept { return recording.load (std::memory_order_acquire); }

    // Multitrack playback (virtual soundcheck). Each track with a file in the take plays
    // through that track's direct outs and the monitor bus instead of its live input.
    juce::Result startPlayback (const juce::File& sessionRoot, int take, uint64_t fromFrame);           // files by position
    juce::Result startPlayback (const std::map<int, juce::File>& filesByRow, int take, uint64_t fromFrame); // files by current row
    void stopPlayback();
    bool isPlaying() const noexcept { return rtPlayer.load (std::memory_order_acquire) != nullptr; }
    bool playbackFinished() const noexcept { auto* p = player.get(); return p != nullptr && isPlaying() && p->finished(); }
    uint64_t playbackPosition() const noexcept { return player != nullptr ? player->position() : 0; }
    uint64_t playbackLength() const noexcept { return player != nullptr ? player->length() : 0; }

    // ---- audio thread
    void process (const float* const* in, int numIn, float* const* out, int numOut, int n) noexcept;

    // ---- telemetry (UI thread)
    MeterReading readMeter (int track) noexcept;     // returns and resets the peak/RMS window
    void clearClip (int track) noexcept { meters[(size_t) track].clip = false; }
    uint64_t recordedFrames() const noexcept { return recFrames.load (std::memory_order_relaxed); }
    double sampleRate() const noexcept { return sr; }
    RecordTarget* target (int i) const noexcept { return targets[(size_t) i].get(); }
    double measuredSampleRate() const noexcept { return measuredRate.load(); }
    uint32_t xrunCount() const noexcept { return xruns.load(); }
    double secondsSinceLastCallback() const noexcept;
    bool isDeviceRunning() const noexcept { return deviceRunning.load(); }
    EventLog& log() noexcept { return events; }
    int currentTake() const noexcept { return take; }

    // Called on the message thread periodically: frees retired snapshots, drains RT events.
    void housekeeping();

private:
    void waitForCallbacks (int count, int timeoutMs);
    void freeRetired (bool all);

    EventLog events;
    double sr = 48000.0;
    int maxBlock = 512;
    int take = 0;

    std::atomic<RoutingSnapshot*> current { nullptr };
    std::unique_ptr<RoutingSnapshot> owned;                     // the one `current` points at
    struct Retired { std::unique_ptr<RoutingSnapshot> snap; uint64_t at; };
    std::vector<Retired> retired;
    uint32_t generation = 0;

    std::atomic<uint64_t> callbackCount { 0 };
    std::atomic<bool> deviceRunning { false };
    std::atomic<bool> recording { false };
    std::atomic<uint64_t> recFrames { 0 };

    std::array<std::unique_ptr<RecordTarget>, 2> targets;
    std::array<std::atomic<RecordTarget*>, 2> rtTargets {};
    std::array<TrackMeter, kMaxTracks> meters;

    std::unique_ptr<Player> player;
    std::atomic<Player*> rtPlayer { nullptr };
    std::vector<float> playScratch = std::vector<float> (16384), playScratchR = std::vector<float> (16384);

    // clock monitor (audio thread writes, UI reads)
    int64_t lastTicks = 0;
    double winStartTicks = 0, sx = 0, sy = 0, sxy = 0, sxx = 0, winSamples = 0;
    int winN = 0;
    std::atomic<double> measuredRate { 0 };
    std::atomic<uint32_t> xruns { 0 };
    std::atomic<int64_t> lastCallbackTicks { 0 };
};
} // namespace omr
