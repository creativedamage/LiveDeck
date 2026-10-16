#include "EventLog.h"

namespace omr
{
static const char* levelName (Level l)
{
    switch (l) { case Level::warning: return "WARN "; case Level::error: return "ERROR"; case Level::info: return "INFO "; }
    return "INFO ";
}

void EventLog::post (Level l, const juce::String& text)
{
    LogEntry e { juce::Time::getCurrentTime(), l, text };
    juce::File f;
    {
        std::lock_guard<std::mutex> g (lock);
        entries.push_back (e);
        if (entries.size() > 20000) { entries.erase (entries.begin(), entries.begin() + 5000); base += 5000; }
        f = logFile;
    }
    if (f != juce::File())
        f.appendText (e.time.toISO8601 (true) + " " + levelName (l) + " " + text + "\n");
    DBG (levelName (l) << " " << text);
}

void EventLog::drainRealtime (double sampleRate)
{
    while (auto* e = rt.front())
    {
        const double secs = sampleRate > 0 ? (double) e->samplePos / sampleRate : 0.0;
        const auto at = juce::String (secs, 3) + " s";
        switch (e->type)
        {
            case RtEvent::overrun:
                post (Level::error, "Disk ring overrun on target " + juce::String (e->target == 0 ? "A" : "B") + ", track "
                                    + juce::String (e->track + 1) + ": " + juce::String (e->count) + " frames replaced by silence at " + at);
                break;
            case RtEvent::xrun:
                post (Level::warning, "Driver dropout: callback late by about " + juce::String (e->count) + " frames at " + at);
                break;
            case RtEvent::playUnderrun:
                post (Level::warning, "Playback underrun (disk too slow) on track " + juce::String (e->track + 1) + ": "
                                      + juce::String (e->count) + " frames of silence at " + at + ". Tracks stay aligned.");
                break;
            case RtEvent::gapQueueFull:
                post (Level::error, "Too many disk overruns to track on track " + juce::String (e->track + 1) + "; alignment may be lost after " + at);
                break;
        }
        rt.pop();
    }
}

std::vector<LogEntry> EventLog::entriesSince (size_t& sinceIndex) const
{
    std::lock_guard<std::mutex> g (lock);
    std::vector<LogEntry> out;
    const size_t start = sinceIndex < base ? 0 : std::min (sinceIndex - base, entries.size());
    out.assign (entries.begin() + (long) start, entries.end());
    sinceIndex = base + entries.size();
    return out;
}

void EventLog::setLogFile (const juce::File& f)
{
    std::lock_guard<std::mutex> g (lock);
    logFile = f;
}
} // namespace omr
