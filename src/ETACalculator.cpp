#include <algorithm>
#include <cmath>
#include "ETACalculator.hpp"
#include "Assert.hpp"

void ETACalculator::updateSpeedEstimate(double currentBytesPerSecondEstimate)
{
    auto val = this->bigSmallBuff.read();

    if (val.getCount() >= BUCKET_COUNT - 1)
    {
        int32_t idx = val.getBitsOnCount(BITSET_MASK);
        debug_assert(idx <= 8);

        if (copySpeedBuckets[idx].sampleCount == 0)
        {
            copySpeedBuckets[idx].bytesPerSecond = currentBytesPerSecondEstimate;
        }
        else
        {
            double alpha = 1.0 / 100.0;
            copySpeedBuckets[idx].bytesPerSecond = (1 - alpha) * copySpeedBuckets[idx].bytesPerSecond +
                                                   alpha * currentBytesPerSecondEstimate;
        }
        copySpeedBuckets[idx].sampleCount++;
    }
}

double ETACalculator::getEta()
{
    auto calculate = [&](size_t sampleCountThreshold)
    {
        double etaBytesPerSec = -1;

        double val = double(this->bigCopiesRemaining) / double(this->bigCopiesRemaining + this->smallCopiesRemaining);
        val = std::clamp(val, 0., 1.);
        val = val * (BUCKET_COUNT - 1);

        if (copySpeedBuckets[int(std::floor(val))].sampleCount > sampleCountThreshold &&
            copySpeedBuckets[int(std::ceil(val))].sampleCount > sampleCountThreshold)
        {
            double alpha = val - std::floor(val);
            etaBytesPerSec = copySpeedBuckets[int(std::floor(val))].bytesPerSecond * alpha +
                             copySpeedBuckets[int(std::ceil(val))].bytesPerSecond * (1.0 - alpha);
        }
        else
        {
            int32_t idealIndex = std::round(val);

            int32_t bestIndex = -1;
            int32_t bestDiff = std::numeric_limits<int32_t>::max();

            for (int32_t i = 0; i < BUCKET_COUNT; i++)
            {
                int32_t thisDiff = std::abs(idealIndex - i);
                if (thisDiff < bestDiff && copySpeedBuckets[i].sampleCount > sampleCountThreshold)
                {
                    bestDiff = thisDiff;
                    bestIndex = i;
                }
            }

            if (bestIndex != -1)
                etaBytesPerSec = copySpeedBuckets[bestIndex].bytesPerSecond;
        }

        return etaBytesPerSec;
    };

    if (this->bigCopiesRemaining == 0 && this->smallCopiesRemaining == 0)
        return 0;

    double retval = calculate(20);
    if (retval == -1)
        retval = calculate(1);

    return retval;
}

void ETACalculator::onCopyCompleted(size_t size)
{
    bool big = size > ETACalculator::BIG_FILE_SIZE;
    this->bigSmallBuff.addToBuff(big);
    if (big)
        this->bigCopiesRemaining--;
    else
        this->smallCopiesRemaining--;
}

void ETACalculator::onCopyAdded(size_t size)
{
    bool big = size > ETACalculator::BIG_FILE_SIZE;
    if (big)
        this->bigCopiesRemaining++;
    else
        this->smallCopiesRemaining++;
}

void RollingBitset::addToBuff(bool b)
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

int32_t RollingBitset::ReadProxy::getBitsOnCount(uint64_t mask) const
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
