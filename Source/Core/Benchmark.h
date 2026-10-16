#pragma once
// Pre-record disk benchmark: writes the real pattern (N parallel files, 192 KiB chunks,
// a sync every second) and reports sustained throughput and worst-case write latency.

#include <juce_core/juce_core.h>

namespace omr
{
struct BenchmarkResult
{
    bool completed = false;
    double mbPerSec = 0;        // sustained, including syncs
    double p99Ms = 0, worstMs = 0;
    double requiredMbPerSec = 0;
    bool pass = false;
    juce::String message;
};

// Blocking; run it on a background thread. `shouldStop` may be null.
BenchmarkResult runDiskBenchmark (const juce::File& folder, int numFiles, double seconds,
                                  double requiredBytesPerSec, double ringSeconds,
                                  const std::atomic<bool>* shouldStop = nullptr);
} // namespace omr
