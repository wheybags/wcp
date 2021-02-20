#pragma once
#include <atomic>
#include <cstddef>

// This class represents a bitset where instead of addressing bits by index, you just shift new values in from
// the right, and old values are bumped off at the left. The top byte is reserved for holding a count, which
// is the number of bits in the set that have been populated.
struct RollingBitset
{
    std::atomic_uint64_t atomicBuffer = 0;

    void addToBuff(bool b);

    struct ReadProxy
    {
        uint64_t val;
        explicit ReadProxy(uint64_t val) : val(val) {}

        uint64_t getCount() const { return (val & 0xFF00000000000000ULL) >> (8*7); }
        uint64_t getSet(uint64_t mask = 0x00FFFFFFFFFFFFFFULL) const { return val & mask; }
        int32_t getBitsOnCount(uint64_t mask = 0x00FFFFFFFFFFFFFFULL) const;
    };

    ReadProxy read() const { return ReadProxy(atomicBuffer.load()); }
};

class ETACalculator
{
public:
    void updateSpeedEstimate(double currentBytesPerSecondEstimate);
    double getEta();

    void onCopyCompleted(size_t size);
    void onCopyAdded(size_t size);

private:
    static constexpr size_t BIG_FILE_SIZE = 1024 * 5;
    static constexpr uint32_t BITSET_MASK = 0xFF;
    static constexpr int32_t BUCKET_COUNT = 8 + 1;

    struct CopySpeedBucket { double bytesPerSecond = 0; size_t sampleCount = 0; };
    CopySpeedBucket copySpeedBuckets[BUCKET_COUNT] = {};
    std::atomic<size_t> bigCopiesRemaining = 0;
    std::atomic<size_t> smallCopiesRemaining = 0;
    RollingBitset bigSmallBuff;
};