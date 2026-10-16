#include "RecordTarget.h"
#include "Session.h"

namespace omr
{
RecordTarget::RecordTarget (int idx, const juce::File& r, EventLog& l)
    : juce::Thread ("omr-disk-" + juce::String (idx == 0 ? "A" : "B")), index (idx), sessionRoot (r), log (l)
{
    byTrack.resize (kMaxTracks);
}

RecordTarget::~RecordTarget()
{
    active = false;
    if (! finished) finish();
}

juce::Result RecordTarget::prepare (const TakeSpec& s)
{
    spec = s;
    list.clear();
    for (auto& p : byTrack) p.reset();
    written = 0; dropped = 0; worstMs = 0; draining = false; failed = false;

    if (! SessionPaths::createTree (sessionRoot))
        return juce::Result::fail ("Cannot create session folders in " + sessionRoot.getFullPathName());

    for (const auto& tk : spec.tracks)
    {
        const int trackIndex = tk.index;
        const auto& name = tk.name;
        auto io = std::make_unique<TrackIO>();
        io->track = trackIndex;
        io->channels = juce::jlimit (1, 2, tk.channels);
        io->ring = std::make_unique<SpscRing<float>> (spec.ringFrames * (size_t) io->channels);
        io->gaps = std::make_unique<SpscQueue<Gap>> (4096);
        io->file = std::make_unique<BwfWriter>();
        io->peaks = std::make_shared<PeakStore>();

        BwfInfo info;
        info.sampleRate = (uint32_t) spec.sampleRate;
        info.bitsPerSample = (uint16_t) spec.bitsPerSample;
        info.isFloat = spec.bitsPerSample == 32;
        info.numChannels = (uint16_t) io->channels;
        info.timeReference = spec.timeReference;
        info.date = spec.date.toStdString();
        info.time = spec.time.toStdString();
        info.description = (spec.sessionName + " / " + name + " / take " + juce::String (spec.take)).toStdString();
        info.originatorRef = ("OMR" + juce::String (spec.take).paddedLeft ('0', 3) + label()).toStdString();
        info.trackName = name.toStdString();
        info.sceneName = spec.sessionName.toStdString();
        info.takeName = juce::String (spec.take).toStdString();

        const auto file = SessionPaths::audioDir (sessionRoot)
                              .getChildFile (SessionPaths::trackFileName (trackIndex, name, spec.take, index == 1));
        if (! io->file->open (file, info))
            return juce::Result::fail ("Cannot create " + file.getFullPathName() + ": " + io->file->lastError());

        list.push_back (io.get());
        byTrack[(size_t) trackIndex] = std::move (io);
    }

    // Take manifest: which track (by permanent id) owns which file, so audio follows tracks
    // through later reorders and deletions.
    {
        juce::Array<juce::var> entries;
        for (const auto& tk : spec.tracks)
        {
            juce::DynamicObject::Ptr e = new juce::DynamicObject();
            e->setProperty ("id", tk.id);
            e->setProperty ("row", tk.index);
            e->setProperty ("name", tk.name);
            e->setProperty ("channels", tk.channels);
            e->setProperty ("file", byTrack[(size_t) tk.index]->file->getFile().getFileName());
            entries.add (juce::var (e.get()));
        }
        juce::DynamicObject::Ptr m = new juce::DynamicObject();
        m->setProperty ("take", spec.take);
        m->setProperty ("tracks", entries);
        SessionPaths::takeManifest (sessionRoot, spec.take).replaceWithText (juce::JSON::toString (juce::var (m.get())), false, false, "\n");
    }

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "start");
    o->setProperty ("take", spec.take);
    o->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
    o->setProperty ("sampleRate", spec.sampleRate);
    o->setProperty ("bits", spec.bitsPerSample);
    o->setProperty ("tracks", (int) list.size());
    journal (juce::var (o.get()));
    finished = false;
    return juce::Result::ok();
}

void RecordTarget::start()
{
    active = true;
    startThread (juce::Thread::Priority::high);
}

void RecordTarget::push (int track, const float* left, const float* right, size_t n, uint64_t takeFramePos) noexcept
{
    auto* io = byTrack[(size_t) track].get();
    if (io == nullptr) return;
    const size_t ch = (size_t) io->channels;

    // Write whole frames only, so a stereo file can never get out of L/R step.
    size_t framesWritten = 0;
    if (ch == 1)
    {
        framesWritten = io->ring->write (left, n);
    }
    else
    {
        if (right == nullptr) right = left;
        const size_t fit = std::min (n, (io->ring->capacity() - io->ring->fill()) / 2);
        float tmp[512];
        while (framesWritten < fit)
        {
            const size_t k = std::min<size_t> (256, fit - framesWritten);
            for (size_t i = 0; i < k; ++i) { tmp[2 * i] = left[framesWritten + i]; tmp[2 * i + 1] = right[framesWritten + i]; }
            io->ring->write (tmp, 2 * k);
            framesWritten += k;
        }
    }

    Gap& og = io->open;
    if (framesWritten < n)
    {
        const size_t pos = io->ring->writePosition();
        const size_t lostFrames = n - framesWritten;
        const size_t lost = lostFrames * ch;   // gaps are counted in samples
        dropped.fetch_add (lostFrames, std::memory_order_relaxed);
        if (og.remaining > 0 && og.pos == pos)
        {
            og.remaining += lost; // still inside the same gap
        }
        else
        {
            if (og.remaining > 0 && ! io->gaps->push (og))
                log.pushRealtime ({ RtEvent::gapQueueFull, (uint8_t) index, (uint16_t) track, takeFramePos, 0 });
            og = { pos, lost };
            log.pushRealtime ({ RtEvent::overrun, (uint8_t) index, (uint16_t) track, takeFramePos + framesWritten, (uint32_t) lostFrames });
        }
    }
    else if (og.remaining > 0)
    {
        if (! io->gaps->push (og))
            log.pushRealtime ({ RtEvent::gapQueueFull, (uint8_t) index, (uint16_t) track, takeFramePos, 0 });
        og = {};
    }
}

void RecordTarget::flushOpenGaps() noexcept
{
    for (auto* io : list)
        if (io->open.remaining > 0) { io->gaps->push (io->open); io->open = {}; }
}

size_t RecordTarget::readWithGaps (TrackIO& io, float* dst, size_t max) noexcept
{
    size_t done = 0;
    while (done < max)
    {
        Gap* g = io.gaps->front();
        const size_t rp = io.ring->readPosition();
        if (g != nullptr && g->pos == rp)
        {
            const size_t z = std::min (g->remaining, max - done);
            std::fill_n (dst + done, z, 0.0f);
            done += z;
            g->remaining -= z;
            if (g->remaining == 0) io.gaps->pop();
            continue;
        }
        const size_t limit = g != nullptr ? std::min (max - done, g->pos - rp) : max - done;
        const size_t r = io.ring->read (dst + done, limit);
        if (r == 0) break;
        done += r;
    }
    return done;
}

void RecordTarget::run()
{
    constexpr size_t kChunk = 65536; // samples per write call (even, so always whole stereo frames)
    std::vector<float> buf (kChunk);
    double lastSync = juce::Time::getMillisecondCounterHiRes();
    const size_t bytesPerSample = (size_t) spec.bitsPerSample / 8;

    for (;;)
    {
        const bool drain = draining.load();
        bool didWork = false;

        if (const int stall = testStallMs.load()) juce::Thread::sleep (stall);

        for (auto* io : list)
        {
            size_t ready = io->ring->fill();
            if (const Gap* g = io->gaps->front()) ready += g->remaining;
            if (ready == 0 || (ready < kChunk / 4 && ! drain)) continue;

            const size_t n = readWithGaps (*io, buf.data(), kChunk);
            if (n == 0) continue;

            const double t0 = juce::Time::getMillisecondCounterHiRes();
            const size_t frames = n / (size_t) io->channels;
            if (! io->file->appendFrames (buf.data(), frames)) { fail ("write", io->file.get()); return; }
            io->peaks->append (buf.data(), frames, io->channels);
            const float ms = (float) (juce::Time::getMillisecondCounterHiRes() - t0);
            if (ms > worstMs.load()) worstMs = ms;
            written.fetch_add (n * bytesPerSample);
            didWork = true;
        }

        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now - lastSync >= 1000.0)
        {
            syncAll();
            if (failed) return;
            lastSync = now;
        }

        if (! didWork)
        {
            if (drain)
            {
                bool empty = true;
                for (auto* io : list) empty &= io->ring->fill() == 0 && io->gaps->empty();
                if (empty) return;
            }
            if (threadShouldExit() && ! drain) return;
            wait (5);
        }
    }
}

void RecordTarget::syncAll()
{
    juce::Array<juce::var> files;
    for (auto* io : list)
    {
        if (! io->file->sync()) { fail ("sync", io->file.get()); return; }
        io->file->updateHeader(); // persisted by the next sync; recovery uses file length anyway
        files.add (juce::var ((juce::int64) io->file->framesWritten()));
    }
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "commit");
    o->setProperty ("frames", files);
    journal (juce::var (o.get()));
}

void RecordTarget::fail (const juce::String& what, const BwfWriter* w)
{
    active = false; // the audio thread stops feeding this target immediately
    failed = true;
    log.post (Level::error, "Target " + label() + " FAILED (" + what + " " + (w != nullptr ? w->getFile().getFileName() : juce::String())
                            + ": " + (w != nullptr ? w->lastError() : juce::String()) + "). Recording continues on the other target if present.");
}

void RecordTarget::finish()
{
    active = false;
    draining = true;
    if (isThreadRunning() && ! waitForThreadToExit (120000))
    {
        log.post (Level::error, "Target " + label() + ": writer did not drain within 120 s; forcing stop");
        stopThread (2000);
    }

    juce::Array<juce::var> frames;
    for (auto* io : list)
    {
        if (! io->file->close() && ! failed)
            log.post (Level::error, "Target " + label() + ": could not finalise " + io->file->getFile().getFileName() + ": " + io->file->lastError());
        frames.add (juce::var ((juce::int64) io->file->framesWritten()));
        io->peaks->finish();
        io->peaks->save (WaveformCache::cacheFileFor (io->file->getFile()));
    }

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "stop");
    o->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
    o->setProperty ("frames", frames);
    o->setProperty ("failed", failed.load());
    o->setProperty ("droppedFrames", (juce::int64) dropped.load());
    journal (juce::var (o.get()));
    finished = true;
}

float RecordTarget::maxRingFill() const noexcept
{
    float m = 0;
    for (auto* io : list) m = std::max (m, io->ring->fillFraction());
    return m;
}

juce::StringArray RecordTarget::filePaths() const
{
    juce::StringArray a;
    for (auto* io : list) a.add (io->file->getFile().getFullPathName());
    return a;
}

std::shared_ptr<PeakStore> RecordTarget::peaksFor (int track) const
{
    auto* io = byTrack[(size_t) track].get();
    return io != nullptr ? io->peaks : nullptr;
}

juce::File RecordTarget::fileFor (int track) const
{
    auto* io = byTrack[(size_t) track].get();
    return io != nullptr ? io->file->getFile() : juce::File();
}

void RecordTarget::journal (const juce::var& v)
{
    SessionPaths::journalFile (sessionRoot).appendText (juce::JSON::toString (v, true) + "\n");
}
} // namespace omr
