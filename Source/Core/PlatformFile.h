#pragma once
// Thin positional-I/O file wrapper: pwrite/fdatasync/F_PREALLOCATE on macOS & Linux,
// WriteFile(OVERLAPPED)/FlushFileBuffers/FileAllocationInfo on Windows.
// Used only on non-real-time threads.

#include <juce_core/juce_core.h>
#include <cstdint>

namespace omr
{
class PlatformFile
{
public:
    PlatformFile() = default;
    ~PlatformFile() { close(); }
    PlatformFile (const PlatformFile&) = delete;
    PlatformFile& operator= (const PlatformFile&) = delete;

    bool create (const juce::File& f);       // fails if the file already exists (never overwrite a take)
    bool openReadWrite (const juce::File& f);
    bool openReadOnly (const juce::File& f);

    bool writeAt (const void* data, size_t bytes, uint64_t offset);
    bool readAt (void* data, size_t bytes, uint64_t offset);

    // Reserve disk blocks without changing the logical size, so the file length always
    // equals what was really written (crash recovery depends on that).
    bool preallocateKeepSize (uint64_t offset, uint64_t length);

    bool dataSync();   // fast barrier: fdatasync / F_BARRIERFSYNC / FlushFileBuffers
    bool fullSync();   // through the drive cache: F_FULLFSYNC on macOS
    bool truncate (uint64_t size);
    uint64_t size() const;

    void close();
    bool isOpen() const noexcept;

    int lastError() const noexcept { return err; }
    juce::String lastErrorString() const;

private:
   #if JUCE_WINDOWS
    void* handle = nullptr;
   #else
    int fd = -1;
   #endif
    int err = 0;
    void captureError();
};
} // namespace omr
