#include "PlatformFile.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <cerrno>
 #include <cstring>
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace omr
{
#if JUCE_WINDOWS

static HANDLE H (void* h) { return (HANDLE) h; }

void PlatformFile::captureError() { err = (int) GetLastError(); }

bool PlatformFile::create (const juce::File& f)
{
    close();
    handle = CreateFileW (f.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                          nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { handle = nullptr; captureError(); return false; }
    return true;
}

bool PlatformFile::openReadWrite (const juce::File& f)
{
    close();
    handle = CreateFileW (f.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { handle = nullptr; captureError(); return false; }
    return true;
}

bool PlatformFile::openReadOnly (const juce::File& f)
{
    close();
    handle = CreateFileW (f.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { handle = nullptr; captureError(); return false; }
    return true;
}

bool PlatformFile::writeAt (const void* data, size_t bytes, uint64_t offset)
{
    OVERLAPPED ov {};
    ov.Offset = (DWORD) (offset & 0xffffffffu);
    ov.OffsetHigh = (DWORD) (offset >> 32);
    DWORD written = 0;
    if (! WriteFile (H (handle), data, (DWORD) bytes, &written, &ov) || written != bytes) { captureError(); return false; }
    return true;
}

bool PlatformFile::readAt (void* data, size_t bytes, uint64_t offset)
{
    OVERLAPPED ov {};
    ov.Offset = (DWORD) (offset & 0xffffffffu);
    ov.OffsetHigh = (DWORD) (offset >> 32);
    DWORD got = 0;
    if (! ReadFile (H (handle), data, (DWORD) bytes, &got, &ov) || got != bytes) { captureError(); return false; }
    return true;
}

bool PlatformFile::preallocateKeepSize (uint64_t offset, uint64_t length)
{
    FILE_ALLOCATION_INFO info {};
    info.AllocationSize.QuadPart = (LONGLONG) (offset + length);
    if (! SetFileInformationByHandle (H (handle), FileAllocationInfo, &info, sizeof (info))) { captureError(); return false; }
    return true;
}

bool PlatformFile::dataSync() { if (! FlushFileBuffers (H (handle))) { captureError(); return false; } return true; }
bool PlatformFile::fullSync() { return dataSync(); }

bool PlatformFile::truncate (uint64_t newSize)
{
    FILE_END_OF_FILE_INFO info {};
    info.EndOfFile.QuadPart = (LONGLONG) newSize;
    if (! SetFileInformationByHandle (H (handle), FileEndOfFileInfo, &info, sizeof (info))) { captureError(); return false; }
    return true;
}

uint64_t PlatformFile::size() const
{
    LARGE_INTEGER s {};
    return GetFileSizeEx (H (handle), &s) ? (uint64_t) s.QuadPart : 0;
}

void PlatformFile::close()
{
    if (handle != nullptr) { CloseHandle (H (handle)); handle = nullptr; }
}

bool PlatformFile::isOpen() const noexcept { return handle != nullptr; }

juce::String PlatformFile::lastErrorString() const { return "Windows error " + juce::String (err); }

#else // POSIX ------------------------------------------------------------------

void PlatformFile::captureError() { err = errno; }

bool PlatformFile::create (const juce::File& f)
{
    close();
    fd = ::open (f.getFullPathName().toRawUTF8(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) { captureError(); return false; }
    return true;
}

bool PlatformFile::openReadWrite (const juce::File& f)
{
    close();
    fd = ::open (f.getFullPathName().toRawUTF8(), O_RDWR | O_CLOEXEC);
    if (fd < 0) { captureError(); return false; }
    return true;
}

bool PlatformFile::openReadOnly (const juce::File& f)
{
    close();
    fd = ::open (f.getFullPathName().toRawUTF8(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { captureError(); return false; }
    return true;
}

bool PlatformFile::writeAt (const void* data, size_t bytes, uint64_t offset)
{
    auto* p = static_cast<const char*> (data);
    while (bytes > 0)
    {
        const ssize_t w = ::pwrite (fd, p, bytes, (off_t) offset);
        if (w < 0) { if (errno == EINTR) continue; captureError(); return false; }
        p += w; bytes -= (size_t) w; offset += (uint64_t) w;
    }
    return true;
}

bool PlatformFile::readAt (void* data, size_t bytes, uint64_t offset)
{
    auto* p = static_cast<char*> (data);
    while (bytes > 0)
    {
        const ssize_t r = ::pread (fd, p, bytes, (off_t) offset);
        if (r < 0) { if (errno == EINTR) continue; captureError(); return false; }
        if (r == 0) { err = EIO; return false; }
        p += r; bytes -= (size_t) r; offset += (uint64_t) r;
    }
    return true;
}

bool PlatformFile::preallocateKeepSize (uint64_t offset, uint64_t length)
{
   #if JUCE_MAC
    juce::ignoreUnused (offset);
    fstore_t st { F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0, (off_t) length, 0 };
    if (fcntl (fd, F_PREALLOCATE, &st) == -1)
    {
        st.fst_flags = F_ALLOCATEALL;
        if (fcntl (fd, F_PREALLOCATE, &st) == -1) { captureError(); return false; }
    }
    return true;
   #elif JUCE_LINUX || JUCE_BSD
    if (fallocate (fd, FALLOC_FL_KEEP_SIZE, (off_t) offset, (off_t) length) != 0) { captureError(); return false; }
    return true;
   #else
    juce::ignoreUnused (offset, length);
    return true;
   #endif
}

bool PlatformFile::dataSync()
{
   #if JUCE_MAC
    #ifdef F_BARRIERFSYNC
     if (fcntl (fd, F_BARRIERFSYNC) == 0) return true;
    #endif
    if (::fsync (fd) != 0) { captureError(); return false; }
    return true;
   #else
    if (::fdatasync (fd) != 0) { captureError(); return false; }
    return true;
   #endif
}

bool PlatformFile::fullSync()
{
   #if JUCE_MAC
    if (fcntl (fd, F_FULLFSYNC) == 0) return true;
   #endif
    if (::fsync (fd) != 0) { captureError(); return false; }
    return true;
}

bool PlatformFile::truncate (uint64_t newSize)
{
    if (::ftruncate (fd, (off_t) newSize) != 0) { captureError(); return false; }
    return true;
}

uint64_t PlatformFile::size() const
{
    struct stat st {};
    return ::fstat (fd, &st) == 0 ? (uint64_t) st.st_size : 0;
}

void PlatformFile::close()
{
    if (fd >= 0) { ::close (fd); fd = -1; }
}

bool PlatformFile::isOpen() const noexcept { return fd >= 0; }

juce::String PlatformFile::lastErrorString() const { return juce::String (std::strerror (err)) + " (errno " + juce::String (err) + ")"; }

#endif
} // namespace omr
