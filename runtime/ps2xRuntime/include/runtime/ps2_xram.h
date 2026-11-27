#ifndef PS2_XRAM_H
#define PS2_XRAM_H

// The extension RAM: the backing above the game's 32 MB (PS2_XRAM_BASE .. PS2_RDRAM_SIZE, ps2_memory.h). The game never
// sees it (its memory map is fixed below 0x01FFC000); only the port's own code hands it out. Save states carry the
// used part of it in their XRAM section (ps2_save_state.cpp); an older state (no section) loads with it cleared.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "runtime/ps2_memory.h"

namespace ps2x::xram
{
    // Bytes of the extension in use: up to its last nonzero word, rounded up to 64 KB. A scan from the top (~10 ms over
    // an empty 96 MB), so it also catches anything written there without going through an allocator.
    inline uint32_t usedBytes(const uint8_t *rdram)
    {
        if (rdram == nullptr)
            return 0u;
        const uint8_t *base = rdram + PS2_XRAM_BASE;
        size_t words = PS2_XRAM_SIZE / 8u;
        while (words > 0u)
        {
            uint64_t w;
            std::memcpy(&w, base + (words - 1u) * 8u, 8u);
            if (w != 0u)
                break;
            --words;
        }
        const size_t used = (words * 8u + 0xFFFFu) & ~static_cast<size_t>(0xFFFFu);
        return static_cast<uint32_t>(std::min<size_t>(used, PS2_XRAM_SIZE));
    }

    inline void clear(uint8_t *rdram)
    {
        if (rdram != nullptr)
            std::memset(rdram + PS2_XRAM_BASE, 0, usedBytes(rdram));
    }
}

#endif // PS2_XRAM_H
