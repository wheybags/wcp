#include "Heap.hpp"
#include "Assert.hpp"
#include "Config.hpp"

Heap::Heap(size_t blocks, size_t blockSize, size_t alignment)
    : blocks(blocks)
    , blockSize(blockSize)
    , alignment(alignment)
{
    debug_assert(alignment > 0 && !(alignment & (alignment-1))); // assert align is power of two

    if (!Config::VALGRIND_MODE)
    {
        this->data = (uint8_t *) aligned_alloc(this->alignment, this->blocks * this->blockSize);
        this->usedList = new std::atomic_bool[this->blocks]();
    }
    debug_assert(this->getFreeBlocksCount() == this->blocks);
}

Heap::Heap(Heap&& other)
{
    this->usedList = other.usedList;
    this->data = other.data;
    this->blocks = other.blocks;
    this->blockSize = other.blockSize;
    this->alignment = other.alignment;

    other.usedList = nullptr;
    other.data = nullptr;
    other.blocks = 0;
    other.blockSize = 0;
    other.alignment = 0;
}

Heap::~Heap()
{
    if (Config::VALGRIND_MODE)
        return;

#ifndef NDEBUG
    for (size_t i = 0; i < this->blocks; i++)
        debug_assert(!this->usedList[i]);
#endif
    free(this->data);
    delete[] this->usedList;
}

uint8_t* Heap::getBlock()
{
    if (Config::VALGRIND_MODE)
    {
        uint32_t expected = this->valgrindModeUsedCount;
        if (this->valgrindModeUsedCount.compare_exchange_strong(expected, expected + 1))
        {
            void* retval = nullptr;
            // valgrind doesn't support aligned_alloc yet
            release_assert(posix_memalign(&retval, this->alignment, this->blockSize) == 0);
            return (uint8_t*)retval;
        }

        return nullptr;
    }

    for (size_t i = 0; i < this->blocks; i++)
    {
        bool expected = false;
        if (usedList[i].compare_exchange_strong(expected, true))
            return this->data + this->blockSize * i;
    }

    return nullptr;
}

void Heap::returnBlock(uint8_t* block)
{
    if (Config::VALGRIND_MODE)
    {
        free(block);
        this->valgrindModeUsedCount--;
        return;
    }

    debug_assert(block >= this->data && block < this->data + (this->blocks * this->blockSize));
    debug_assert((intptr_t(block) - intptr_t(this->data)) % this->blockSize == 0);

    size_t i = (block - this->data) / this->blockSize;
    debug_assert(i < this->blocks && usedList[i]);
    usedList[i] = false;
}

size_t Heap::getFreeBlocksCount() const
{
    if (Config::VALGRIND_MODE)
        return this->blocks - this->valgrindModeUsedCount;

    size_t sum = 0;
    for (size_t i = 0; i < this->blocks; i++)
    {
        if (!usedList[i])
            sum++;
    }

    return sum;
}

