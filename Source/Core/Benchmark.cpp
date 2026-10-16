#include "Benchmark.h"
#include "PlatformFile.h"
#include <algorithm>
#include <memory>

namespace omr
{
BenchmarkResult runDiskBenchmark (const juce::File& folder, int numFiles, double seconds,
                                  double requiredBytesPerSec, double ringSeconds, const std::atomic<bool>* shouldStop)
{
    BenchmarkResult r;
    r.requiredMbPerSec = requiredBytesPerSec / 1.0e6;

    const auto dir = folder.getChildFile (".omr_benchmark");
    dir.deleteRecursively();
    if (! dir.createDirectory()) { r.message = "Cannot create benchmark folder"; return r; }

    std::vector<std::unique_ptr<PlatformFile>> files;
    for (int i = 0; i < numFiles; ++i)
    {
        auto f = std::make_unique<PlatformFile>();
        if (! f->create (dir.getChildFile ("bench_" + juce::String (i) + ".bin"))) { r.message = "Cannot create test file: " + f->lastErrorString(); dir.deleteRecursively(); return r; }
        files.push_back (std::move (f));
    }

    constexpr size_t kChunk = 192 * 1024;
    std::vector<uint8_t> buf (kChunk);
    juce::Random rng;
    for (auto& b : buf) b = (uint8_t) rng.nextInt (256); // incompressible

    std::vector<uint64_t> offsets ((size_t) numFiles, 0);
    std::vector<double> latencies;
    uint64_t total = 0;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    double lastSync = t0;
    bool ok = true;

    while (ok && juce::Time::getMillisecondCounterHiRes() - t0 < seconds * 1000.0)
    {
        if (shouldStop != nullptr && shouldStop->load()) break;
        for (int i = 0; i < numFiles && ok; ++i)
        {
            const double a = juce::Time::getMillisecondCounterHiRes();
            ok = files[(size_t) i]->writeAt (buf.data(), kChunk, offsets[(size_t) i]);
            latencies.push_back (juce::Time::getMillisecondCounterHiRes() - a);
            offsets[(size_t) i] += kChunk;
            total += kChunk;
        }
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now - lastSync >= 1000.0)
        {
            for (auto& f : files) { const double a = juce::Time::getMillisecondCounterHiRes(); ok &= f->dataSync(); latencies.push_back (juce::Time::getMillisecondCounterHiRes() - a); }
            lastSync = now;
        }
    }
    for (auto& f : files) ok &= f->dataSync();
    const double elapsed = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;

    files.clear();
    dir.deleteRecursively();

    if (! ok) { r.message = "Write failed during benchmark"; return r; }
    std::sort (latencies.begin(), latencies.end());
    r.completed = true;
    r.mbPerSec = (double) total / 1.0e6 / std::max (elapsed, 0.001);
    r.worstMs = latencies.empty() ? 0 : latencies.back();
    r.p99Ms = latencies.empty() ? 0 : latencies[(size_t) ((double) (latencies.size() - 1) * 0.99)];
    // Pass: 3x headroom on throughput, and the worst stall is well inside the ring.
    r.pass = r.mbPerSec >= 3.0 * r.requiredMbPerSec && r.worstMs < ringSeconds * 1000.0 * 0.25;
    r.message = juce::String (juce::roundToInt (r.mbPerSec)) + " MB/s sustained (need " + juce::String (r.requiredMbPerSec, 1)
              + "), p99 " + juce::String (r.p99Ms, 1) + " ms, worst " + juce::String (juce::roundToInt (r.worstMs)) + " ms: "
              + (r.pass ? "PASS" : "FAIL");
    return r;
}
} // namespace omr
