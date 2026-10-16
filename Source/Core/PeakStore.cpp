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

void PeakStore::pushLevel (int level, uint8_t v)
{
    lv[level].push_back (v);
    if (level + 1 >= kLevels) return;
    upAcc[level] = std::max (upAcc[level], v);
    if (++upN[level] == kRatio)
    {
        const uint8_t up = upAcc[level];
        upAcc[level] = 0;
        upN[level] = 0;
        pushLevel (level + 1, up);
    }
}

void PeakStore::append (const float* x, size_t n, int channels)
{
    std::lock_guard<std::mutex> g (lock);
    for (size_t i = 0; i < n; ++i)
    {
        for (int c = 0; c < channels; ++c) acc = std::max (acc, std::abs (x[i * (size_t) channels + (size_t) c]));
        if (++accN == kFramesPerPeak)
        {
            pushLevel (0, quantise (acc));
            acc = 0.0f;
            accN = 0;
        }
    }
    total += n;
}

void PeakStore::finish()
{
    std::lock_guard<std::mutex> g (lock);
    if (accN > 0) { pushLevel (0, quantise (acc)); acc = 0.0f; accN = 0; }
    for (int l = 0; l + 1 < kLevels; ++l)
        if (upN[l] > 0) { lv[l + 1].push_back (upAcc[l]); upAcc[l] = 0; upN[l] = 0; }
    complete = true;
}

void PeakStore::clear()
{
    std::lock_guard<std::mutex> g (lock);
    for (auto& v : lv) v.clear();
    acc = 0; accN = 0; total = 0;
    std::fill (std::begin (upAcc), std::end (upAcc), (uint8_t) 0);
    std::fill (std::begin (upN), std::end (upN), 0);
    complete = false;
}

uint64_t PeakStore::frames() const
{
    std::lock_guard<std::mutex> g (lock);
    return total;
}

void PeakStore::render (double startFrame, double fpp, int width, float* out) const
{
    std::lock_guard<std::mutex> g (lock);
    // Coarsest level that still has at least one bucket per pixel.
    int level = 0;
    double bucket = kFramesPerPeak;
    while (level + 1 < kLevels && bucket * kRatio <= fpp && ! lv[level + 1].empty()) { ++level; bucket *= kRatio; }

    const auto& v = lv[level];
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
            const auto& f = lv[0];
            const size_t j0 = (size_t) (f0 / kFramesPerPeak);
            const size_t j1 = std::min (f.size(), (size_t) std::ceil ((f0 + fpp) / kFramesPerPeak));
            for (size_t j = j0; j < j1; ++j) m = std::max (m, f[j]);
        }
        out[px] = (float) m / 255.0f;
    }
}

// File format: "OMRPK1" + u64 totalFrames + u32 count[3] + level data
bool PeakStore::save (const juce::File& f) const
{
    std::lock_guard<std::mutex> g (lock);
    f.getParentDirectory().createDirectory();
    juce::FileOutputStream os (f);
    if (! os.openedOk()) return false;
    os.setPosition (0);
    os.truncate();
    os.write ("OMRPK1", 6);
    os.writeInt64 ((juce::int64) total);
    for (const auto& v : lv) os.writeInt ((int) v.size());
    for (const auto& v : lv) os.write (v.data(), v.size());
    os.flush();
    return ! os.getStatus().failed();
}

bool PeakStore::load (const juce::File& f)
{
    juce::FileInputStream is (f);
    if (! is.openedOk()) return false;
    char magic[6] = {};
    if (is.read (magic, 6) != 6 || std::memcmp (magic, "OMRPK1", 6) != 0) return false;
    const auto t = (uint64_t) is.readInt64();
    int counts[kLevels];
    for (auto& c : counts) { c = is.readInt(); if (c < 0 || c > (1 << 28)) return false; }
    std::vector<uint8_t> data[kLevels];
    for (int l = 0; l < kLevels; ++l)
    {
        data[l].resize ((size_t) counts[l]);
        if (counts[l] > 0 && is.read (data[l].data(), counts[l]) != counts[l]) return false;
    }
    std::lock_guard<std::mutex> g (lock);
    for (int l = 0; l < kLevels; ++l) lv[l] = std::move (data[l]);
    total = t;
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
        if (cache.existsAsFile() && cache.getLastModificationTime() >= wav.getLastModificationTime() && store.load (cache))
            continue;

        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (wav));
        if (r == nullptr) { store.finish(); continue; }
        const bool stereo = r->numChannels > 1;
        juce::AudioBuffer<float> b (2, (int) buf.size());
        for (juce::int64 pos = 0; pos < r->lengthInSamples && ! threadShouldExit();)
        {
            const int n = (int) std::min<juce::int64> ((juce::int64) buf.size(), r->lengthInSamples - pos);
            r->read (&b, 0, n, pos, true, stereo);
            if (stereo) // waveform shows the louder of L/R
                juce::FloatVectorOperations::abs (b.getWritePointer (0), b.getReadPointer (0), n),
                juce::FloatVectorOperations::abs (b.getWritePointer (1), b.getReadPointer (1), n),
                juce::FloatVectorOperations::max (b.getWritePointer (0), b.getReadPointer (0), b.getReadPointer (1), n);
            store.append (b.getReadPointer (0), (size_t) n);
            pos += n;
        }
        if (threadShouldExit()) break;
        store.finish();
        store.save (cache);
    }
}
} // namespace omr
