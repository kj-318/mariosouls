#pragma once

// Remastered host memory utilities.
//
// Remastered is a 64-bit process; all addresses and pointers are 64-bit.
// Unlike PTDE, no fixed base address is assumed (ASLR status must be verified
// per executable, per the findings report). All reads/writes go through these
// helpers so that bounds and validity checks are centralised.

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace remem {

// Check whether an address range is readable.
inline bool isReadable(uintptr_t addr, size_t size) {
    if (!addr || !size)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void *>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect == PAGE_NOACCESS || mbi.Protect == PAGE_GUARD)
        return false;
    // Ensure the entire range is within the same committed region.
    uintptr_t end = addr + size - 1;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end < regionEnd;
}

// Check whether an address range is writable.
inline bool isWritable(uintptr_t addr, size_t size) {
    if (!addr || !size)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void *>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
        prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY)
        return false;
    uintptr_t end = addr + size - 1;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end < regionEnd;
}

// Read a value of type T from address `addr`. Returns false if the read fails.
template <typename T>
bool read(uintptr_t addr, T &out) {
    if (!addr)
        return false;
    if (!isReadable(addr, sizeof(T)))
        return false;
    out = *reinterpret_cast<const T *>(addr);
    return true;
}

// Write a value of type T to address `addr`. Returns false if the write fails.
template <typename T>
bool write(uintptr_t addr, const T &value) {
    if (!addr)
        return false;
    if (!isWritable(addr, sizeof(T)))
        return false;
    *reinterpret_cast<T *>(addr) = value;
    return true;
}

} // namespace remem