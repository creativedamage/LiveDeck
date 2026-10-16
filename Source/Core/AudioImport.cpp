#include "AudioImport.h"

namespace omr
{
static constexpr int kBlock = 65536;   // source frames per read

int ImportPlan::numTracks() const
{
    int n = 0;
    for (const auto& s : sources) n += (int) s.outputs.size();
    return n;
}

bool isImportableFile (const juce::File& f)
{
    return f.existsAsFile() && f.hasFileExtension ("wav;wave;bwf;aif;aiff;aifc");
}

ImportPlan planImport (const juce::StringArray& paths)
{
    ImportPlan plan;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    juce::Array<juce::File> files;
    for (const auto& p : paths)
    {
        const juce::File f (p);
        if (f.isDirectory())   // a dropped folder imports the audio files directly inside it, by name
        {
            auto inside = f.findChildFiles (juce::File::findFiles, false);
            inside.sort();
            for (const auto& c : inside) if (isImportableFile (c)) files.add (c);
        }
        else files.add (f);
    }

    for (const auto& f : files)
    {
        if (! isImportableFile (f)) { plan.skipped.add (f.getFileName() + ": not a WAV or AIFF file"); continue; }
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) { plan.skipped.add (f.getFileName() + ": unreadable or unsupported encoding"); continue; }
        if (r->lengthInSamples <= 0 || r->numChannels < 1) { plan.skipped.add (f.getFileName() + ": no audio"); continue; }

        ImportSource s;
        s.file = f;
        s.sampleRate = r->sampleRate;
        s.lengthFrames = r->lengthInSamples;
        s.numChannels = (int) r->numChannels;
        const auto base = f.getFileNameWithoutExtension().trim();
        if (s.numChannels == 1)      s.outputs.push_back ({ { 0 }, base });
        else if (s.numChannels == 2) s.outputs.push_back ({ { 0, 1 }, base });
        else for (int c = 0; c < s.numChannels; ++c) s.outputs.push_back ({ { c }, base + " " + juce::String (c + 1) });
        plan.sources.push_back (std::move (s));
    }
    return plan;
}

namespace
{
// Measures the sinc interpolator's delay (in input frames) at this ratio: resamples a smooth,
// band-limited test signal and finds the shift that best matches it (coarse, then fine search).
// When downsampling, the probe also goes through the anti-alias filter, so its delay is included.
void setLowpass (std::array<juce::IIRFilter, 4>& f, double inRate, double outRate)
{
    for (auto& x : f) x.setCoefficients (juce::IIRCoefficients::makeLowPass (inRate, outRate * 0.45, 0.7071));
}

double measureDelay (double inRate, double outRate)
{
    const double ratio = inRate / outRate;
    auto sig = [] (double t) { return std::sin (0.0188496 * t) + 0.5 * std::sin (0.0446106 * t + 1.0); };  // 0.003 and 0.0071 cycles/frame
    juce::WindowedSincInterpolator probe;
    std::vector<float> in (8192), out (2048);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (float) sig ((double) i);
    if (ratio > 1.0001)
    {
        std::array<juce::IIRFilter, 4> lp;
        setLowpass (lp, inRate, outRate);
        for (auto& f : lp) f.processSamples (in.data(), (int) in.size());
    }
    const int n = juce::jmin ((int) out.size(), (int) ((double) (in.size() - 300) / ratio));
    probe.process (ratio, in.data(), out.data(), n);

    const int k0 = juce::jmax (1, (int) std::ceil (300.0 / ratio)), k1 = juce::jmin (n, k0 + 200);
    auto err = [&] (double delay)
    {
        double e = 0;
        for (int k = k0; k < k1; ++k) { const double d = out[(size_t) k] - sig (k * ratio - delay); e += d * d; }
        return e;
    };
    double best = 100.0, bestErr = err (best);
    for (double step : { 0.05, 0.002 })
    {
        const double centre = best;
        for (double d = centre - step * 100; d <= centre + step * 100; d += step)
            if (const double e = err (d); e < bestErr) { bestErr = e; best = d; }
    }
    return best;
}

// Streams one channel through an anti-alias filter (when downsampling) and a windowed-sinc resampler.
// The very first output uses a slightly different step so that, after `skip` outputs are
// dropped, output frame n lands exactly on input position n * ratio (no fractional delay).
struct ChannelResampler
{
    double ratio = 1.0;                  // input frames per output frame
    double firstStep = 1.0;
    int skip = 0;
    bool started = false;
    juce::WindowedSincInterpolator interp;
    std::array<juce::IIRFilter, 4> lowpass;
    bool filtering = false;
    std::vector<float> fifo;

    void prepare (double inRate, double outRate, double delay)
    {
        ratio = inRate / outRate;
        skip = (int) std::floor ((delay + ratio) / ratio);
        firstStep = delay + ratio - skip * ratio;
        if (firstStep < 1.0e-6) { --skip; firstStep += ratio; }
        filtering = ratio > 1.0001;
        if (filtering) setLowpass (lowpass, inRate, outRate);
    }

    void push (const float* in, int n)
    {
        const auto at = fifo.size();
        fifo.insert (fifo.end(), in, in + n);
        if (filtering) for (auto& f : lowpass) f.processSamples (fifo.data() + at, n);
    }

    // Produces as many output frames as the queued input allows.
    int pull (std::vector<float>& out)
    {
        out.clear();
        int avail = (int) fifo.size();
        size_t consumed = 0;
        if (! started)
        {
            if (avail < (int) std::ceil (firstStep) + 2) return 0;
            float first = 0;
            consumed += (size_t) interp.process (firstStep, fifo.data(), &first, 1);
            out.push_back (first);
            started = true;
        }
        const int left = avail - (int) consumed;
        const int numOut = juce::jmax (0, (int) std::floor ((left - 2) / ratio));
        if (numOut > 0)
        {
            const auto at = out.size();
            out.resize (at + (size_t) numOut);
            consumed += (size_t) interp.process (ratio, fifo.data() + consumed, out.data() + at, numOut);
        }
        fifo.erase (fifo.begin(), fifo.begin() + (std::ptrdiff_t) juce::jmin (consumed, fifo.size()));
        return (int) out.size();
    }
};
} // namespace

ImportResult importSource (const ImportSource& src, const std::vector<juce::File>& destinations,
                           double targetRate, int bitsPerSample, int64_t offsetFrames,
                           const std::atomic<bool>* cancel, const std::function<void (double)>& progress)
{
    ImportResult res;
    if (destinations.size() != src.outputs.size()) { res.error = "Internal error: destination count"; return res; }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (src.file));
    if (reader == nullptr) { res.error = "Cannot read " + src.file.getFileName(); return res; }

    const int numCh = (int) reader->numChannels;
    const bool resample = std::abs (reader->sampleRate - targetRate) > 0.5;
    const double ratio = reader->sampleRate / targetRate;
    const int64_t outLength = resample ? (int64_t) std::llround ((double) reader->lengthInSamples / ratio) : reader->lengthInSamples;

    // Writers
    std::vector<std::unique_ptr<BwfWriter>> writers;
    const auto now = juce::Time::getCurrentTime();
    for (size_t i = 0; i < destinations.size(); ++i)
    {
        BwfInfo info;
        info.sampleRate = (uint32_t) std::llround (targetRate);
        info.bitsPerSample = (uint16_t) (bitsPerSample == 32 ? 32 : 24);
        info.isFloat = bitsPerSample == 32;
        info.numChannels = (uint16_t) src.outputs[i].sourceChannels.size();
        info.description = ("Imported from " + src.file.getFileName()).toStdString();
        info.originatorRef = "LiveDeck import";
        info.date = now.formatted ("%Y-%m-%d").toStdString();
        info.time = now.formatted ("%H:%M:%S").toStdString();
        info.trackName = src.outputs[i].trackName.toStdString();
        auto w = std::make_unique<BwfWriter>();
        if (! w->open (destinations[i], info))
        {
            res.error = "Cannot create " + destinations[i].getFileName() + ": " + w->lastError();
            for (auto& o : writers) { o->close(); o->getFile().deleteFile(); }
            return res;
        }
        writers.push_back (std::move (w));
    }

    auto abort = [&] (const juce::String& why)
    {
        res.error = why;
        for (auto& w : writers) { w->close(); w->getFile().deleteFile(); }
        return res;
    };

    // Per-output interleaved scratch.
    std::vector<std::vector<float>> inter (writers.size());
    auto writeFrames = [&] (const std::vector<const float*>& chans, int n) -> bool
    {
        for (size_t i = 0; i < writers.size(); ++i)
        {
            const auto& sc = src.outputs[i].sourceChannels;
            auto& buf = inter[i];
            if (sc.size() == 1) { if (! writers[i]->appendFrames (chans[(size_t) sc[0]], (size_t) n)) return false; continue; }
            buf.resize ((size_t) n * 2);
            for (int k = 0; k < n; ++k) { buf[(size_t) k * 2] = chans[(size_t) sc[0]][k]; buf[(size_t) k * 2 + 1] = chans[(size_t) sc[1]][k]; }
            if (! writers[i]->appendFrames (buf.data(), (size_t) n)) return false;
        }
        return true;
    };

    // Leading silence (drop position)
    {
        std::vector<float> zeros ((size_t) kBlock, 0.0f);
        std::vector<const float*> z ((size_t) numCh, zeros.data());
        for (int64_t left = juce::jmax<int64_t> (0, offsetFrames); left > 0;)
        {
            const int n = (int) juce::jmin<int64_t> (left, kBlock);
            if (! writeFrames (z, n)) return abort ("Write failed (disk full?)");
            left -= n;
        }
    }

    juce::AudioBuffer<float> block (numCh, kBlock);
    std::vector<ChannelResampler> rs ((size_t) (resample ? numCh : 0));
    const double delay = resample ? measureDelay (reader->sampleRate, targetRate) : 0.0;
    for (auto& r : rs) r.prepare (reader->sampleRate, targetRate, delay);
    std::vector<std::vector<float>> outs ((size_t) numCh);
    int64_t toSkip = resample ? rs.front().skip : 0;
    int64_t produced = 0, readPos = 0;
    int padBlocks = 0;

    while (produced < outLength)
    {
        if (cancel != nullptr && cancel->load()) return abort ("Cancelled");

        const int n = (int) juce::jmin<int64_t> (kBlock, reader->lengthInSamples - readPos);
        if (n > 0)
        {
            if (! reader->read (&block, 0, n, readPos, true, true)) return abort ("Read error in " + src.file.getFileName());
            readPos += n;
        }

        if (! resample)
        {
            std::vector<const float*> chans ((size_t) numCh);
            for (int c = 0; c < numCh; ++c) chans[(size_t) c] = block.getReadPointer (c);
            if (! writeFrames (chans, n)) return abort ("Write failed (disk full?)");
            produced += n;
        }
        else
        {
            if (n <= 0)
            {
                // Flush the resampler's latency with silence.
                if (++padBlocks > 4) break;
                block.clear();
            }
            const int fed = n > 0 ? n : 1024;
            int got = 0;
            for (int c = 0; c < numCh; ++c)
            {
                rs[(size_t) c].push (block.getReadPointer (c), fed);
                got = rs[(size_t) c].pull (outs[(size_t) c]);
            }
            int start = 0;
            if (toSkip > 0) { start = (int) juce::jmin<int64_t> (toSkip, got); toSkip -= start; }
            const int count = (int) juce::jmin<int64_t> (got - start, outLength - produced);
            if (count > 0)
            {
                std::vector<const float*> chans ((size_t) numCh);
                for (int c = 0; c < numCh; ++c) chans[(size_t) c] = outs[(size_t) c].data() + start;
                if (! writeFrames (chans, count)) return abort ("Write failed (disk full?)");
                produced += count;
            }
        }
        if (progress) progress ((double) produced / (double) juce::jmax<int64_t> (1, outLength));
    }

    for (auto& w : writers)
        if (! w->close()) return abort ("Cannot finish " + w->getFile().getFileName());
    for (auto& w : writers) res.written.push_back (w->getFile());
    res.framesWritten = juce::jmax<int64_t> (0, offsetFrames) + produced;
    res.ok = true;
    return res;
}
} // namespace omr
