#include "RecordEngine.h"

namespace omr
{
RecordEngine::RecordEngine()
{
    for (auto& t : rtTargets) t.store (nullptr);
    owned = compileSnapshot (PatchState {}, generation++);
    current.store (owned.get());
}

RecordEngine::~RecordEngine()
{
    stopPlayback();
    if (isRecording()) stopRecording();
}

void RecordEngine::deviceStarting (double sampleRate, int blockSize)
{
    if (isRecording() && std::abs (sampleRate - sr) > 0.5)
    {
        events.post (Level::error, "Sample rate changed mid-take (" + juce::String (sr) + " -> " + juce::String (sampleRate) + "): stopping take");
        stopRecording();
    }
    sr = sampleRate;
    maxBlock = blockSize;
    lastTicks = 0;
    winN = 0;
    measuredRate = 0;
    deviceRunning = true;
}

void RecordEngine::deviceStopped()
{
    deviceRunning = false;
    if (isRecording())
        events.post (Level::error, "Audio device stopped during recording. Files stay open; audio resumes if the device returns.");
}

void RecordEngine::setPatch (const PatchState& patch)
{
    auto next = compileSnapshot (patch, generation++);
    current.store (next.get(), std::memory_order_release);
    retired.push_back ({ std::move (owned), callbackCount.load (std::memory_order_acquire) });
    owned = std::move (next);
    freeRetired (false);
}

void RecordEngine::freeRetired (bool all)
{
    const uint64_t now = callbackCount.load (std::memory_order_acquire);
    const bool running = deviceRunning.load();
    retired.erase (std::remove_if (retired.begin(), retired.end(),
                                   [&] (const Retired& r) { return all || ! running || now >= r.at + 2; }),
                   retired.end());
}

void RecordEngine::housekeeping()
{
    freeRetired (false);
    events.drainRealtime (sr);
}

void RecordEngine::waitForCallbacks (int count, int timeoutMs)
{
    const uint64_t target = callbackCount.load (std::memory_order_acquire) + (uint64_t) count;
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (deviceRunning.load() && callbackCount.load (std::memory_order_acquire) < target
           && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep (1);
}

double RecordEngine::secondsSinceLastCallback() const noexcept
{
    const auto t = lastCallbackTicks.load();
    if (t == 0) return 1e9;
    return juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t);
}

static TakeSpec makeTakeSpec (const SessionSettings& s, const PatchState& p, double sr, int take)
{
    TakeSpec spec;
    spec.take = take;
    spec.sessionName = s.name;
    spec.sampleRate = sr;
    spec.bitsPerSample = s.bitsPerSample;
    spec.ringFrames = (size_t) (sr * juce::jlimit (2.0, 120.0, s.ringSeconds));

    const auto now = juce::Time::getCurrentTime();
    spec.date = now.formatted ("%Y-%m-%d");
    spec.time = now.formatted ("%H:%M:%S");
    const double secsSinceMidnight = now.getHours() * 3600.0 + now.getMinutes() * 60.0 + now.getSeconds() + now.getMilliseconds() / 1000.0;
    spec.timeReference = (uint64_t) (secsSinceMidnight * sr);

    for (int i = 0; i < (int) p.tracks.size() && i < kMaxTracks; ++i)
        if (p.tracks[(size_t) i].armed && p.tracks[(size_t) i].input >= 0)
            spec.tracks.push_back ({ i, p.tracks[(size_t) i].name, p.tracks[(size_t) i].stereo ? 2 : 1, p.tracks[(size_t) i].id });
    return spec;
}

juce::Result RecordEngine::startRecording (SessionSettings& session, const PatchState& patch)
{
    if (isRecording()) return juce::Result::fail ("Already recording");
    if (isPlaying()) stopPlayback();
    if (session.primaryRoot == juce::File()) return juce::Result::fail ("Choose a session folder before recording");

    take = session.nextTake;
    const auto spec = makeTakeSpec (session, patch, sr, take);
    if (spec.tracks.empty()) return juce::Result::fail ("No armed tracks with an input");

    const auto check = validateTarget (session.primaryRoot, sr, session.bitsPerSample, (int) spec.tracks.size(), session.minFreeHours);
    if (! check.ok) return juce::Result::fail ("Primary target: " + check.message);
    if (check.message.isNotEmpty()) events.post (Level::warning, "Primary target: " + check.message);

    targets[0] = std::make_unique<RecordTarget> (0, session.primaryRoot, events);
    if (auto r = targets[0]->prepare (spec); r.failed()) { targets[0].reset(); return r; }

    targets[1].reset();
    if (session.secondaryRoot != juce::File())
    {
        const auto c2 = validateTarget (session.secondaryRoot, sr, session.bitsPerSample, (int) spec.tracks.size(), session.minFreeHours);
        if (! c2.ok)
            events.post (Level::error, "Secondary target unavailable (" + c2.message + "); recording to primary only");
        else
        {
            targets[1] = std::make_unique<RecordTarget> (1, session.secondaryRoot, events);
            if (auto r = targets[1]->prepare (spec); r.failed())
            {
                events.post (Level::error, "Secondary target: " + r.getErrorMessage() + "; recording to primary only");
                targets[1]->finish();
                targets[1].reset();
            }
        }
    }

    // Make sure the snapshot the audio thread reads reflects this exact patch.
    setPatch (patch);

    for (int i = 0; i < 2; ++i)
        if (targets[(size_t) i] != nullptr) targets[(size_t) i]->start();
    for (int i = 0; i < 2; ++i)
        rtTargets[(size_t) i].store (targets[(size_t) i].get(), std::memory_order_release);

    recFrames.store (0);
    recording.store (true, std::memory_order_release);
    session.nextTake = take + 1;

    events.post (Level::info, "Recording take " + juce::String (take) + ": " + juce::String ((int) spec.tracks.size()) + " tracks, "
                              + juce::String (sr / 1000.0, 1) + " kHz / " + juce::String (session.bitsPerSample) + "-bit"
                              + (targets[1] ? ", redundant to " + session.secondaryRoot.getFullPathName() : juce::String()));
    return juce::Result::ok();
}

void RecordEngine::stopRecording()
{
    if (! recording.exchange (false, std::memory_order_acq_rel)) return;

    waitForCallbacks (2, 1000); // after this, no callback is still inside push()
    for (auto& t : rtTargets) t.store (nullptr, std::memory_order_release);
    waitForCallbacks (1, 500);

    const double secs = (double) recFrames.load() / sr;
    for (auto& t : targets)
        if (t != nullptr) { t->flushOpenGaps(); t->finish(); }

    events.drainRealtime (sr);
    events.post (Level::info, "Stopped take " + juce::String (take) + " after " + juce::String (secs, 1) + " s");
}

juce::Result RecordEngine::startPlayback (const juce::File& root, int playTake, uint64_t fromFrame)
{
    const auto takes = scanTakes (root);
    const auto it = takes.find (playTake);
    if (it == takes.end()) return juce::Result::fail ("Take " + juce::String (playTake) + " not found in " + root.getFullPathName());
    return startPlayback (it->second, playTake, fromFrame);
}

juce::Result RecordEngine::startPlayback (const std::map<int, juce::File>& files, int playTake, uint64_t fromFrame)
{
    if (isRecording()) return juce::Result::fail ("Stop recording before playback");
    stopPlayback();
    auto p = std::make_unique<Player> (events);
    if (auto r = p->load (files, playTake, sr, fromFrame); r.failed()) return r;
    if (p->numTracks() == 0) return juce::Result::fail ("Take has no tracks");
    p->start();
    player = std::move (p);
    rtPlayer.store (player.get(), std::memory_order_release);
    events.post (Level::info, "Playing take " + juce::String (playTake) + " from " + juce::String ((double) fromFrame / sr, 1) + " s");
    return juce::Result::ok();
}

void RecordEngine::stopPlayback()
{
    if (rtPlayer.exchange (nullptr, std::memory_order_acq_rel) == nullptr) return;
    waitForCallbacks (2, 1000); // no callback is still reading from the player after this
    player->halt();
    if (player->underrunFrames() > 0)
        events.post (Level::warning, "Playback had " + juce::String ((juce::int64) player->underrunFrames()) + " frames of underrun (disk too slow)");
    // keep `player` alive so position()/length() still report where we stopped
}

MeterReading RecordEngine::readMeter (int track) noexcept
{
    auto& m = meters[(size_t) track];
    MeterReading r;
    r.peak = m.peak.exchange (0.0f, std::memory_order_relaxed);
    const float s = m.sumSq.exchange (0.0f, std::memory_order_relaxed);
    const uint32_t c = m.count.exchange (0, std::memory_order_relaxed);
    r.rms = c > 0 ? std::sqrt (s / (float) c) : 0.0f;
    r.clip = m.clip.load (std::memory_order_relaxed);
    return r;
}

void RecordEngine::process (const float* const* in, int numIn, float* const* out, int numOut, int n) noexcept
{
    const RoutingSnapshot* snap = current.load (std::memory_order_acquire);

    // ---- clock monitor + dropout detection (no syscalls beyond the monotonic clock read)
    const int64_t ticks = juce::Time::getHighResolutionTicks();
    lastCallbackTicks.store (ticks, std::memory_order_relaxed);
    if (lastTicks != 0)
    {
        const double dt = juce::Time::highResolutionTicksToSeconds (ticks - lastTicks);
        const double expected = (double) n / sr;
        if (callbackCount.load (std::memory_order_relaxed) > 50 && dt > expected * 2.5)
        {
            xruns.fetch_add (1, std::memory_order_relaxed);
            events.pushRealtime ({ RtEvent::xrun, 0, 0, recFrames.load (std::memory_order_relaxed), (uint32_t) ((dt - expected) * sr) });
        }
        // least-squares fit of samples vs. time over ~20 s windows
        const double x = juce::Time::highResolutionTicksToSeconds (ticks) - winStartTicks;
        sx += x; sy += winSamples; sxy += x * winSamples; sxx += x * x; ++winN;
        if (x >= 20.0 && winN > 100)
        {
            const double denom = winN * sxx - sx * sx;
            if (denom > 0) measuredRate.store ((winN * sxy - sx * sy) / denom, std::memory_order_relaxed);
            winN = 0;
        }
    }
    if (winN == 0) { winStartTicks = juce::Time::highResolutionTicksToSeconds (ticks); sx = sy = sxy = sxx = 0; winSamples = 0; }
    lastTicks = ticks;
    winSamples += n;

    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr) std::fill_n (out[ch], n, 0.0f);

    const bool rec = recording.load (std::memory_order_acquire);
    RecordTarget* tA = rec ? rtTargets[0].load (std::memory_order_acquire) : nullptr;
    RecordTarget* tB = rec ? rtTargets[1].load (std::memory_order_acquire) : nullptr;
    if (tA != nullptr && ! tA->active.load (std::memory_order_relaxed)) tA = nullptr;
    if (tB != nullptr && ! tB->active.load (std::memory_order_relaxed)) tB = nullptr;
    const uint64_t pos = recFrames.load (std::memory_order_relaxed);
    Player* pl = rec ? nullptr : rtPlayer.load (std::memory_order_acquire);

    const bool monL = snap->monitorLeft >= 0 && snap->monitorLeft < numOut && out[snap->monitorLeft] != nullptr;
    const bool monR = snap->monitorRight >= 0 && snap->monitorRight < numOut && out[snap->monitorRight] != nullptr;
    constexpr float kPanCentre = 0.70710678f;

    for (int t = 0; t < snap->numTracks; ++t)
    {
        const TrackRT& tr = snap->tracks[(size_t) t];
        const float* srcL = nullptr;
        const float* srcR = nullptr;   // non-null only for a stereo track
        if (pl != nullptr)
        {
            if (! pl->hasTrack (t) || n > (int) playScratch.size()) continue;
            const int ch = pl->readTrack (t, playScratch.data(), playScratchR.data(), n, pl->position());
            srcL = playScratch.data();
            if (ch == 2) srcR = playScratchR.data();
        }
        else
        {
            if (tr.input < 0 || tr.input >= numIn || in[tr.input] == nullptr) continue;
            srcL = in[tr.input];
            if (tr.input2 >= 0 && tr.input2 < numIn && in[tr.input2] != nullptr) srcR = in[tr.input2];
        }

        meters[(size_t) t].update (srcL, n);
        if (srcR != nullptr) meters[(size_t) t].update (srcR, n);

        if (tr.armed)
        {
            if (tA != nullptr) tA->push (t, srcL, srcR, (size_t) n, pos);
            if (tB != nullptr) tB->push (t, srcL, srcR, (size_t) n, pos);
        }

        if (tr.mute) continue;

        // Direct outs: mono out or stereo pair (o, o+1).
        //   mono track -> mono out: as is      mono track -> pair: same signal to both
        //   stereo track -> pair: L/R          stereo track -> mono out: (L+R)/2
        for (int k = 0; k < tr.numOuts; ++k)
        {
            const int o = tr.outs[(size_t) k];
            if (o < 0 || o >= numOut || out[o] == nullptr) continue;
            float* oR = (tr.outStereo && o + 1 < numOut) ? out[o + 1] : nullptr;
            if (oR != nullptr)
            {
                const float* r = srcR != nullptr ? srcR : srcL;
                for (int i = 0; i < n; ++i) { out[o][i] += srcL[i] * tr.gain; oR[i] += r[i] * tr.gain; }
            }
            else if (srcR != nullptr)
                for (int i = 0; i < n; ++i) out[o][i] += (srcL[i] + srcR[i]) * 0.5f * tr.gain;
            else
                for (int i = 0; i < n; ++i) out[o][i] += srcL[i] * tr.gain;
        }

        if ((monL || monR) && (! snap->anySolo || tr.solo))
        {
            // mono tracks are centre-panned at -3 dB, stereo tracks keep their image
            const float g = tr.gain * snap->monitorGain * (srcR != nullptr ? 1.0f : kPanCentre);
            const float* r = srcR != nullptr ? srcR : srcL;
            if (monL) for (int i = 0; i < n; ++i) out[snap->monitorLeft][i] += srcL[i] * g;
            if (monR && snap->monitorRight != snap->monitorLeft)
                for (int i = 0; i < n; ++i) out[snap->monitorRight][i] += r[i] * g;
        }
    }

    if (rec) recFrames.store (pos + (uint64_t) n, std::memory_order_relaxed);
    if (pl != nullptr) pl->advance (n);
    callbackCount.fetch_add (1, std::memory_order_release);
}
} // namespace omr
