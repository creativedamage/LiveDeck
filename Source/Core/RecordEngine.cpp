#include "RecordEngine.h"

namespace omr
{
RecordEngine::RecordEngine()
{
    for (auto& v : playScratch) v.resize (16384);
    for (auto& v : playScratchR) v.resize (16384);
    for (auto& t : rtTargets) t.store (nullptr);
    owned = compileSnapshot (PatchState {}, generation++);
    current.store (owned.get());
}

RecordEngine::~RecordEngine()
{
    stopPlayback();
    if (isRecording()) stopRecording();
}

void RecordEngine::deviceStarting (int d, double sampleRate, int blockSize)
{
    if (d < 0 || d >= kMaxDevices) return;
    auto& ds = dev[(size_t) d];
    if (d == 0)
    {
        if (isRecording() && std::abs (sampleRate - sr) > 0.5)
        {
            events.post (Level::error, "Sample rate changed mid-take (" + juce::String (sr) + " -> " + juce::String (sampleRate) + "): stopping take");
            stopRecording();
        }
        sr = sampleRate;
        maxBlock = blockSize;
    }
    // Extra devices must run at the primary's rate: files carry one sample rate per take.
    ds.rateMismatch = d != 0 && std::abs (sampleRate - sr) > 0.5;
    if (ds.rateMismatch)
        events.post (Level::error, "Device " + juce::String (d + 1) + " runs at " + juce::String (sampleRate / 1000.0, 1) + " kHz but the primary runs at "
                                   + juce::String (sr / 1000.0, 1) + " kHz: its tracks are ignored until the rates match");
    ds.lastTicks = 0;
    ds.winN = 0;
    ds.measuredRate = 0;
    ds.running = true;
}

void RecordEngine::deviceStopped (int d)
{
    if (d < 0 || d >= kMaxDevices) return;
    dev[(size_t) d].running = false;
    if (isRecording())
        events.post (Level::error, juce::String (d == 0 ? "The primary audio device" : "Audio device " + juce::String (d + 1))
                                   + " stopped during recording. Files stay open; its tracks resume if the device returns.");
}

void RecordEngine::setPatch (const PatchState& patch)
{
    auto next = compileSnapshot (patch, generation++);
    if (next->crossDeviceOuts > 0)
        events.post (Level::warning, juce::String (next->crossDeviceOuts) + " track(s) send their direct out to a different device than their input. "
                                     "Live audio cannot cross devices without a resampler, so those outs stay silent (playback still uses them).");
    current.store (next.get(), std::memory_order_release);
    Retired r { std::move (owned), {} };
    for (int d = 0; d < kMaxDevices; ++d) r.at[(size_t) d] = dev[(size_t) d].callbackCount.load (std::memory_order_acquire);
    retired.push_back (std::move (r));
    owned = std::move (next);
    freeRetired (false);
}

void RecordEngine::freeRetired (bool all)
{
    retired.erase (std::remove_if (retired.begin(), retired.end(), [&] (const Retired& r)
    {
        if (all) return true;
        for (int d = 0; d < kMaxDevices; ++d)
        {
            const auto& ds = dev[(size_t) d];
            if (ds.running.load() && ds.callbackCount.load (std::memory_order_acquire) < r.at[(size_t) d] + 2) return false;
        }
        return true;
    }), retired.end());
}

void RecordEngine::housekeeping()
{
    freeRetired (false);
    events.drainRealtime (sr);
}

void RecordEngine::waitForCallbacks (int count, int timeoutMs)
{
    std::array<uint64_t, kMaxDevices> target {};
    for (int d = 0; d < kMaxDevices; ++d) target[(size_t) d] = dev[(size_t) d].callbackCount.load (std::memory_order_acquire) + (uint64_t) count;
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    for (;;)
    {
        bool done = true;
        for (int d = 0; d < kMaxDevices; ++d)
        {
            const auto& ds = dev[(size_t) d];
            if (ds.running.load() && ds.callbackCount.load (std::memory_order_acquire) < target[(size_t) d]) done = false;
        }
        if (done || juce::Time::getMillisecondCounter() >= deadline) return;
        juce::Thread::sleep (1);
    }
}

double RecordEngine::secondsSinceLastCallback (int d) const noexcept
{
    const auto t = dev[(size_t) d].lastCallbackTicks.load();
    if (t == 0) return 1e9;
    return juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t);
}

double RecordEngine::worstSecondsSinceCallback() const noexcept
{
    double worst = 0;
    for (int d = 0; d < kMaxDevices; ++d)
        if (dev[(size_t) d].running.load()) worst = juce::jmax (worst, secondsSinceLastCallback (d));
    return worst;
}

int RecordEngine::numRunningDevices() const noexcept
{
    int n = 0;
    for (const auto& ds : dev) n += ds.running.load() ? 1 : 0;
    return n;
}

uint32_t RecordEngine::xrunCount() const noexcept
{
    uint32_t n = 0;
    for (const auto& ds : dev) n += ds.xruns.load();
    return n;
}

double RecordEngine::driftPpm (int d) const noexcept
{
    const double a = dev[0].measuredRate.load(), b = dev[(size_t) d].measuredRate.load();
    return (a > 0 && b > 0) ? (b / a - 1.0) * 1.0e6 : 0.0;
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

    for (auto& ds : dev) ds.recFrames.store (0);
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

    const double secs = (double) dev[0].recFrames.load() / sr;
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
    std::map<int, Clip> clips;
    for (const auto& [row, f] : files) clips[row] = { f, 0, f.getFileNameWithoutExtension() };
    return startPlayback (clips, playTake, fromFrame);
}

juce::Result RecordEngine::startPlayback (const std::map<int, Clip>& files, int playTake, uint64_t fromFrame)
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

void RecordEngine::process (int d, const float* const* in, int numIn, float* const* out, int numOut, int n) noexcept
{
    auto& ds = dev[(size_t) d];
    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr) std::fill_n (out[ch], n, 0.0f);
    if (ds.rateMismatch.load (std::memory_order_relaxed)) { ds.callbackCount.fetch_add (1, std::memory_order_release); return; }

    const RoutingSnapshot* snap = current.load (std::memory_order_acquire);

    // ---- clock monitor + dropout detection for THIS device (only the monotonic clock is read)
    const int64_t ticks = juce::Time::getHighResolutionTicks();
    ds.lastCallbackTicks.store (ticks, std::memory_order_relaxed);
    if (ds.lastTicks != 0)
    {
        const double dt = juce::Time::highResolutionTicksToSeconds (ticks - ds.lastTicks);
        const double expected = (double) n / sr;
        if (ds.callbackCount.load (std::memory_order_relaxed) > 50 && dt > expected * 2.5)
        {
            ds.xruns.fetch_add (1, std::memory_order_relaxed);
            events.pushRealtime ({ RtEvent::xrun, (uint8_t) d, 0, ds.recFrames.load (std::memory_order_relaxed), (uint32_t) ((dt - expected) * sr) }, d);
        }
        // least-squares fit of samples vs. time over ~20 s windows
        const double x = juce::Time::highResolutionTicksToSeconds (ticks) - ds.winStartTicks;
        ds.sx += x; ds.sy += ds.winSamples; ds.sxy += x * ds.winSamples; ds.sxx += x * x; ++ds.winN;
        if (x >= 20.0 && ds.winN > 100)
        {
            const double denom = ds.winN * ds.sxx - ds.sx * ds.sx;
            if (denom > 0) ds.measuredRate.store ((ds.winN * ds.sxy - ds.sx * ds.sy) / denom, std::memory_order_relaxed);
            ds.winN = 0;
        }
    }
    if (ds.winN == 0) { ds.winStartTicks = juce::Time::highResolutionTicksToSeconds (ticks); ds.sx = ds.sy = ds.sxy = ds.sxx = 0; ds.winSamples = 0; }
    ds.lastTicks = ticks;
    ds.winSamples += n;

    const bool rec = recording.load (std::memory_order_acquire);
    RecordTarget* tA = rec ? rtTargets[0].load (std::memory_order_acquire) : nullptr;
    RecordTarget* tB = rec ? rtTargets[1].load (std::memory_order_acquire) : nullptr;
    if (tA != nullptr && ! tA->active.load (std::memory_order_relaxed)) tA = nullptr;
    if (tB != nullptr && ! tB->active.load (std::memory_order_relaxed)) tB = nullptr;
    const uint64_t pos = ds.recFrames.load (std::memory_order_relaxed);
    Player* pl = rec ? nullptr : rtPlayer.load (std::memory_order_acquire);

    const bool monHere = snap->monitorDev == d;
    const bool monL = monHere && snap->monitorLeft >= 0 && snap->monitorLeft < numOut && out[snap->monitorLeft] != nullptr;
    const bool monR = monHere && snap->monitorRight >= 0 && snap->monitorRight < numOut && out[snap->monitorRight] != nullptr;
    constexpr float kPanCentre = 0.70710678f;
    auto& scratchL = playScratch[(size_t) d];
    auto& scratchR = playScratchR[(size_t) d];

    for (int t = 0; t < snap->numTracks; ++t)
    {
        const TrackRT& tr = snap->tracks[(size_t) t];
        const float* srcL = nullptr;
        const float* srcR = nullptr;   // non-null only for a stereo track
        if (pl != nullptr)
        {
            // Played-back audio is rendered by the device it is sent to: its direct outs' device,
            // else the monitor's device. (A track's ring can only be read by one device.)
            const int owner = tr.numOuts > 0 ? tr.outDev : snap->monitorDev;
            if (owner != d || ! pl->hasTrack (t) || n > (int) scratchL.size()) continue;
            const int ch = pl->readTrack (t, scratchL.data(), scratchR.data(), n, pl->position (d), d);
            srcL = scratchL.data();
            if (ch == 2) srcR = scratchR.data();
        }
        else
        {
            if (tr.inDev != d || tr.input < 0 || tr.input >= numIn || in[tr.input] == nullptr) continue;
            srcL = in[tr.input];
            if (tr.input2 >= 0 && tr.input2 < numIn && in[tr.input2] != nullptr) srcR = in[tr.input2];
        }

        meters[(size_t) t].update (srcL, n);
        if (srcR != nullptr) meters[(size_t) t].update (srcR, n);

        if (tr.armed && pl == nullptr)
        {
            if (tA != nullptr) tA->push (t, srcL, srcR, (size_t) n, pos, d);
            if (tB != nullptr) tB->push (t, srcL, srcR, (size_t) n, pos, d);
        }

        if (tr.mute) continue;

        // Direct outs (only on this device): mono out or stereo pair (o, o+1).
        //   mono track -> mono out: as is      mono track -> pair: same signal to both
        //   stereo track -> pair: L/R          stereo track -> mono out: (L+R)/2
        if (tr.outDev == d)
        {
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

    if (rec) ds.recFrames.store (pos + (uint64_t) n, std::memory_order_relaxed);
    if (pl != nullptr) pl->advance (d, n);
    ds.callbackCount.fetch_add (1, std::memory_order_release);
}
} // namespace omr
