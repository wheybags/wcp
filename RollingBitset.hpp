#pragma once
#include <atomic>

// This class represents a bitset where instead of addressing bits by index, you just shift new values in from
// the right, and old values are bumped off at the left. The top byte is reserved for holding a count, which
// is the number of bits in the set that have been populated.
struct RollingBitset
{
    std::atomic_uint64_t atomicBuffer = 0;

    void addToBuff(bool b)
    {
        while (true)
        {
            uint64_t oldVal = atomicBuffer.load();

            uint64_t countPart = ((oldVal & 0xFF00000000000000ULL) >> (8 * 7)) + 1;
            if (countPart > 8 * 7)
                countPart = 8 * 7;
            countPart <<= 8 * 7;

            uint64_t newVal = (oldVal << 1) | uint64_t(b);
            newVal = newVal & 0x00FFFFFFFFFFFFFFULL;
            newVal |= countPart;

            if (atomicBuffer.compare_exchange_strong(oldVal, newVal))
                break;
        }
    }

    struct ReadProxy
    {
        uint64_t val;
        explicit ReadProxy(uint64_t val) : val(val) {}

        uint64_t getCount() const { return (val & 0xFF00000000000000ULL) >> (8*7); }
        uint64_t getSet(uint64_t mask = 0x00FFFFFFFFFFFFFFULL) const { return val & mask; }
        int32_t getBitsOnCount(uint64_t mask = 0x00FFFFFFFFFFFFFFULL) const
        {
            int32_t count = 0;
            uint64_t tmp = getSet(mask);
            while (tmp)
            {
                count += tmp & 1;
                tmp >>= 1;
            }
            return count;
        }
    };

    ReadProxy read() const { return ReadProxy(atomicBuffer.load()); }
};