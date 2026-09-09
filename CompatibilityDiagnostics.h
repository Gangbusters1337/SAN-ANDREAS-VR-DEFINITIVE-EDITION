#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

// Read-only diagnostics. These results never select an address or authorize a patch.
namespace CompatibilityDiagnostics {
    inline bool Read(uintptr_t address, void* output, size_t size) {
        SIZE_T copied = 0;
        return address && output && size && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(address), output, size, &copied) && copied == size;
    }

    struct PatchCheck {
        const char* status = "invalid-range";
        std::array<uint8_t, 32> observed{};
        size_t observedSize = 0;
        DWORD protection = 0;
        bool matches = false;
    };

    inline PatchCheck CheckPatch(uintptr_t base, size_t imageSize, uintptr_t rva,
        const uint8_t* expected, size_t size) {
        PatchCheck result;
        if (!base || !expected || !size || size > result.observed.size()
            || rva >= imageSize || size > imageSize - rva
            || base > (std::numeric_limits<uintptr_t>::max)() - imageSize)
            return result;
        const uintptr_t address = base + rva;
        MEMORY_BASIC_INFORMATION page{};
        result.status = "unreadable";
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &page, sizeof(page))
            || page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return result;
        result.protection = page.Protect;
        const uintptr_t pageBase = reinterpret_cast<uintptr_t>(page.BaseAddress);
        if (address < pageBase || address - pageBase >= page.RegionSize
            || size > page.RegionSize - (address - pageBase)
            || !Read(address, result.observed.data(), size))
            return result;
        result.observedSize = size;
        const DWORD execute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(page.Protect & execute)) { result.status = "not-executable"; return result; }
        if (std::memcmp(expected, result.observed.data(), size) == 0) {
            result.status = "match"; result.matches = true; return result;
        }
        bool nopped = true;
        for (size_t i = 0; i < size; ++i) nopped &= result.observed[i] == 0x90;
        result.status = nopped ? "already-nopped-or-different-build" : "byte-mismatch";
        return result;
    }
}
