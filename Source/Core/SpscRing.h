#pragma once
// Lock-free single-producer / single-consumer containers.
// Producer = a device's audio callback, consumer = a disk writer thread (or the UI).
// Neither side ever blocks, allocates or takes a lock after construction.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace omr
{
inline size_t nextPow2 (size_t v) noexcept
{
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// Sample ring. Positions are free-running counters, so (write - read) is always the fill.
template <typename T>
class SpscRing
{
public:
    explicit SpscRing (size_t minCapacity)
        : buf (nextPow2 (std::max<size_t> (minCapacity, 2))), mask (buf.size() - 1) {}

    // Producer. Returns the number of items written (fewer than n when full). Never blocks.
    size_t write (const T* src, size_t n) noexcept
    {
        const size_t w = writePos.load (std::memory_order_relaxed);
        const size_t r = readPos.load (std::memory_order_acquire);
        n = std::min (n, buf.size() - (w - r));
        const size_t i = w & mask, first = std::min (n, buf.size() - i);
        std::memcpy (buf.data() + i, src, first * sizeof (T));
        std::memcpy (buf.data(), src + first, (n - first) * sizeof (T));
        writePos.store (w + n, std::memory_order_release);
        return n;
    }

    // Consumer.
    size_t read (T* dst, size_t n) noexcept
    {
        const size_t r = readPos.load (std::memory_order_relaxed);
        const size_t w = writePos.load (std::memory_order_acquire);
        n = std::min (n, w - r);
        const size_t i = r & mask, first = std::min (n, buf.size() - i);
        std::memcpy (dst, buf.data() + i, first * sizeof (T));
        std::memcpy (dst + first, buf.data(), (n - first) * sizeof (T));
        readPos.store (r + n, std::memory_order_release);
        return n;
    }

    size_t fill() const noexcept
    {
        const size_t r = readPos.load (std::memory_order_acquire);
        return writePos.load (std::memory_order_acquire) - r;
    }
    size_t capacity() const noexcept      { return buf.size(); }
    size_t writePosition() const noexcept { return writePos.load (std::memory_order_acquire); }
    size_t readPosition() const noexcept  { return readPos.load (std::memory_order_acquire); }
    float fillFraction() const noexcept   { return (float) fill() / (float) buf.size(); }

private:
    std::vector<T> buf; // sized once, before recording starts
    const size_t mask;
    alignas (64) std::atomic<size_t> writePos { 0 };
    alignas (64) std::atomic<size_t> readPos { 0 };
};

// Queue of small POD records (gaps, real-time events).
// front()/pop() let the consumer partially consume the front item in place.
template <typename T>
class SpscQueue
{
public:
    explicit SpscQueue (size_t minCapacity)
        : items (nextPow2 (std::max<size_t> (minCapacity, 2))), mask (items.size() - 1) {}

    bool push (const T& v) noexcept
    {
        const size_t h = head.load (std::memory_order_relaxed);
        if (h - tail.load (std::memory_order_acquire) == items.size()) return false;
        items[h & mask] = v;
        head.store (h + 1, std::memory_order_release);
        return true;
    }

    T* front() noexcept
    {
        const size_t t = tail.load (std::memory_order_relaxed);
        return t == head.load (std::memory_order_acquire) ? nullptr : &items[t & mask];
    }

    void pop() noexcept { tail.store (tail.load (std::memory_order_relaxed) + 1, std::memory_order_release); }

    bool empty() const noexcept
    {
        return tail.load (std::memory_order_acquire) == head.load (std::memory_order_acquire);
    }

private:
    std::vector<T> items;
    const size_t mask;
    alignas (64) std::atomic<size_t> head { 0 };
    alignas (64) std::atomic<size_t> tail { 0 };
};

// A run of frames the producer had to drop because the ring was full.
// pos = ring write position at which the missing frames belong.
struct Gap
{
    size_t pos = 0;
    size_t remaining = 0;
};

} // namespace omr
