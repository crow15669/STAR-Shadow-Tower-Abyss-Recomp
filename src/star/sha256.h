#pragma once

// SHA-256 (boot ELF check). From God Hand: Rearmed (src/kamite/sha256.h, itself the Urban Reign port's) with the namespace renamed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace star
{
    class Sha256
    {
    public:
        Sha256();
        void update(const void *data, size_t size);
        std::array<uint8_t, 32> finish();
        std::string finishHex();

        static std::string hex(const void *data, size_t size);

    private:
        void block(const uint8_t *chunk);

        std::array<uint32_t, 8> m_state;
        std::array<uint8_t, 64> m_buffer{};
        size_t m_bufferSize = 0;
        uint64_t m_totalBytes = 0;
    };
}
