#include "PeakStore.h"
#include "BwfWriter.h"
#include <cmath>
#include <cstring>

namespace omr
{
static uint8_t quantise (float peak)
{
    // ceil so that any non-silent bucket is visible
    return (uint8_t) juce::jlimit (0, 255, (int) std::ceil (std::min (1.0f, peak) * 255.0f));
}

void PeakStore::pushLevel (Chan& c, int level, uint8_t v)
{
    c.lv[level].push_back (v);
    if (level + 1 >= kLevels) return;
    c.upAcc[level] = std::max (c.upAcc[level], v);
    if (++c.upN[level] == kRatio)
    {
        const uint8_t up = c.upAcc[level];
        c.upAcc[level] = 0;
        c.upN[level] = 0;
        pushLevel (c, level + 1, up);
    }
}

void PeakStore::append (const float* x, size_t n, int channels)
{
    std::lock_guard<std::mutex> g (lock);
    if (total == 0 && accN == 0) numCh = juce::jlimit (1, kMaxChannels, channels);
    const size_t stride = (size_t) juce::jmax (1, channels);
    for (size_t i = 0; i < n; ++i)
    {
        for (size_t c = 0; c < stride; ++c)
        {
            auto& ch = chans[(size_t) juce::jmin ((int) c, numCh - 1)];
            ch.acc = std::max (ch.acc, std::abs (x[i * stride + c]));
        }
        if (++accN == kFramesPerPeak)
        {
            for (int c = 0; c < numCh; ++c) { pushLevel (chans[(size_t) c], 0, quantise (chans[(size_t) c].acc)); chans[(size_t) c].acc = 0.0f; }
            accN = 0;
        }
    }
    total += n;
}

void PeakStore::finish()
{
    std::lock_guard<std::mutex> g (lock);
    for (int c = 0; c < numCh; ++c)
    {
        auto& ch = chans[(size_t) c];
        if (accN > 0) { pushLevel (ch, 0, quantise (ch.acc)); ch.acc = 0.0f; }
        for (int l = 0; l + 1 < kLevels; ++l)
            if (ch.upN[l] > 0) { ch.lv[l + 1].push_back (ch.upAcc[l]); ch.upAcc[l] = 0; ch.upN[l] = 0; }
    }
    accN = 0;
    complete = true;
}

void PeakStore::clear()
{
    std::lock_guard<std::mutex> g (lock);
    for (auto& c : chans) c = Chan();
    accN = 0; total = 0; numCh = 1;
    complete = false;
}

uint64_t PeakStore::frames() const
{
    std::lock_guard<std::mutex> g (lock);
    return total;
}

int PeakStore::numChannels() const
{
    std::lock_guard<std::mutex> g (lock);
    return numCh;
}

void PeakStore::renderChannel (const Chan& c, double startFrame, double fpp, int width, float* out) const
{
    // Coarsest level that still has at least one bucket per pixel.
    int level = 0;
    double bucket = kFramesPerPeak;
    while (level + 1 < kLevels && bucket * kRatio <= fpp && ! c.lv[level + 1].empty()) { ++level; bucket *= kRatio; }

    const auto& v = c.lv[level];
    const double end = (double) total;
    for (int px = 0; px < width; ++px)
    {
        const double f0 = startFrame + px * fpp;
        if (f0 < 0 || f0 >= end || v.empty()) { out[px] = -1.0f; continue; }
        const size_t i0 = (size_t) (f0 / bucket);
        size_t i1 = (size_t) std::ceil ((f0 + fpp) / bucket);
        i1 = std::min (std::max (i1, i0 + 1), v.size());
        uint8_t m = 0;
        for (size_t i = i0; i < i1; ++i) m = std::max (m, v[i]);
        if (i0 >= v.size() && level > 0)
        {
            // coarse levels lag the live edge while recording: fall back to the fine level
            const auto& f = c.lv[0];
            const size_t j0 = (size_t) (f0 / kFramesPerPeak);
            const size_t j1 = std::min (f.size(), (size_t) std::ceil ((f0 + fpp) / kFramesPerPeak));
            for (size_t j = j0; j < j1; ++j) m = std::max (m, f[j]);
        }
        out[px] = (float) m / 255.0f;
    }
}

void PeakStore::render (double startFrame, double fpp, int width, float* out, int channel) const
{
    std::lock_guard<std::mutex> g (lock);
    if (channel >= 0) { renderChannel (chans[(size_t) juce::jmin (channel, numCh - 1)], startFrame, fpp, width, out); return; }
    renderChannel (chans[0], startFrame, fpp, width, out);
    if (numCh < 2) return;
    std::vector<float> r ((size_t) width);
    renderChannel (chans[1], startFrame, fpp, width, r.data());
    for (int i = 0; i < width; ++i) out[i] = std::max (out[i], r[(size_t) i]);
}

// File format v2: "OMRPK2" + i64 totalFrames + i32 channels + i32 count[ch][level] + data[ch][level]
bool PeakStore::save (const juce::File& f) const
{
    std::lock_guard<std::mutex> g (lock);
    f.getParentDirectory().createDirectory();
    juce::FileOutputStream os (f);
    if (! os.openedOk()) return false;
    os.setPosition (0);
    os.truncate();
    os.write ("OMRPK2", 6);
    os.writeInt64 ((juce::int64) total);
    os.writeInt (numCh);
    for (int c = 0; c < numCh; ++c) for (const auto& v : chans[(size_t) c].lv) os.writeInt ((int) v.size());
    for (int c = 0; c < numCh; ++c) for (const auto& v : chans[(size_t) c].lv) os.write (v.data(), v.size());
    os.flush();
    return ! os.getStatus().failed();
}

bool PeakStore::load (const juce::File& f)
{
    juce::FileInputStream is (f);
    if (! is.openedOk()) return false;
    char magic[6] = {};
    if (is.read (magic, 6) != 6) return false;
    const bool v1 = std::memcmp (magic, "OMRPK1", 6) == 0;
    if (! v1 && std::memcmp (magic, "OMRPK2", 6) != 0) return false;
    const auto t = (uint64_t) is.readInt64();
    const int nch = v1 ? 1 : is.readInt();
    if (nch < 1 || nch > kMaxChannels) return false;
    int counts[kMaxChannels][kLevels] {};
    for (int c = 0; c < nch; ++c)
        for (auto& n : counts[c]) { n = is.readInt(); if (n < 0 || n > (1 << 28)) return false; }
    std::array<Chan, kMaxChannels> loaded;
    for (int c = 0; c < nch; ++c)
        for (int l = 0; l < kLevels; ++l)
        {
            auto& v = loaded[(size_t) c].lv[l];
            v.resize ((size_t) counts[c][l]);
            if (counts[c][l] > 0 && is.read (v.data(), counts[c][l]) != counts[c][l]) return false;
        }
    std::lock_guard<std::mutex> g (lock);
    chans = std::move (loaded);
    numCh = nch;
    total = t;
    accN = 0;
    complete = true;
    return true;
}

// ---------------------------------------------------------------------------------------------

WaveformCache::WaveformCache() : juce::Thread ("omr-waveforms")
{
    formats.registerBasicFormats();
}

WaveformCache::~WaveformCache() { stopThread (4000); }

juce::File WaveformCache::cacheFileFor (const juce::File& wav)
{
    return wav.getParentDirectory().getChildFile (".peaks").getChildFile (wav.getFileName() + ".pk");
}

std::shared_ptr<PeakStore> WaveformCache::get (const juce::File& wav)
{
    std::lock_guard<std::mutex> g (lock);
    auto& s = stores[wav.getFullPathName()];
    if (s == nullptr)
    {
        s = std::make_shared<PeakStore>();
        queue.emplace_back (wav, s);
        if (! isThreadRunning()) startThread (juce::Thread::Priority::low);
        notify();
    }
    return s;
}

void WaveformCache::put (const juce::File& wav, std::shared_ptr<PeakStore> store)
{
    std::lock_guard<std::mutex> g (lock);
    stores[wav.getFullPathName()] = std::move (store);
}

void WaveformCache::clear()
{
    std::lock_guard<std::mutex> g (lock);
    stores.clear();
    queue.clear();
}

void WaveformCache::run()
{
    std::vector<float> buf (65536);
    while (! threadShouldExit())
    {
        std::pair<juce::File, std::shared_ptr<PeakStore>> job;
        {
            std::lock_guard<std::mutex> g (lock);
            if (! queue.empty()) { job = queue.front(); queue.erase (queue.begin()); }
        }
        if (job.second == nullptr) { wait (200); continue; }

        const auto& wav = job.first;
        auto& store = *job.second;
        const auto cache = cacheFileFor (wav);
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (wav));
        if (r == nullptr) { store.finish(); continue; }
        const int nch = r->numChannels > 1 ? 2 : 1;

        // Reuse the cache unless it is stale or from an older mono-only version for a stereo file.
        if (cache.existsAsFile() && cache.getLastModificationTime() >= wav.getLastModificationTime()
            && store.load (cache) && store.numChannels() == nch)
            continue;
        store.clear();
        juce::AudioBuffer<float> b (2, (int) buf.size());
        std::vector<float> inter (buf.size() * 2);
        for (juce::int64 pos = 0; pos < r->lengthInSamples && ! threadShouldExit();)
        {
            const int n = (int) std::min<juce::int64> ((juce::int64) buf.size(), r->lengthInSamples - pos);
            r->read (&b, 0, n, pos, true, nch == 2);
            if (nch == 1) store.append (b.getReadPointer (0), (size_t) n, 1);
            else
            {
                const float* l = b.getReadPointer (0);
                const float* rr = b.getReadPointer (1);
                for (int i = 0; i < n; ++i) { inter[(size_t) (2 * i)] = l[i]; inter[(size_t) (2 * i + 1)] = rr[i]; }
                store.append (inter.data(), (size_t) n, 2);
            }
            pos += n;
        }
        if (threadShouldExit()) break;
        store.finish();
        store.save (cache);
    }
}
} // namespace omr
