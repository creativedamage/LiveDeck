#pragma once
// Event console backend.
//  - Non-real-time threads call post() (mutex; never used on the audio thread).
//  - The audio thread pushes fixed-size RtEvent records into a lock-free queue;
//    the UI thread converts them to text in drainRealtime().

#include "SpscRing.h"
#include <juce_core/juce_core.h>
#include <mutex>

namespace omr
{
enum class Level { info, warning, error };

struct LogEntry
{
    juce::Time time;
    Level level = Level::info;
    juce::String text;
};

struct RtEvent
{
    enum Type : uint8_t { overrun, xrun, gapQueueFull, playUnderrun } type = overrun;
    uint8_t target = 0;
    uint16_t track = 0;
    uint64_t samplePos = 0;
    uint32_t count = 0;
};

class EventLog
{
public:
    static constexpr int kProducers = 8;   // one lock-free queue per audio device thread
    EventLog() { for (auto& q : rt) q = std::make_unique<SpscQueue<RtEvent>> (1024); }

    void post (Level l, const juce::String& text);           // any non-RT thread
    // Audio threads only; `producer` = the device index whose callback is calling.
    bool pushRealtime (const RtEvent& e, int producer = 0) noexcept { return rt[(size_t) juce::jlimit (0, kProducers - 1, producer)]->push (e); }

    // UI thread: convert pending RT events, then copy entries newer than `sinceIndex`.
    void drainRealtime (double sampleRate);
    std::vector<LogEntry> entriesSince (size_t& sinceIndex) const;

    void setLogFile (const juce::File& f);

private:
    mutable std::mutex lock;
    std::vector<LogEntry> entries;
    size_t base = 0; // global index of entries[0]
    std::array<std::unique_ptr<SpscQueue<RtEvent>>, kProducers> rt;
    juce::File logFile;
};
} // namespace omr
