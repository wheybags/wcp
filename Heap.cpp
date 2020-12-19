#include "Heap.hpp"
#include "Assert.hpp"

Heap::Heap(size_t blocks, size_t blockSize, size_t alignment)
    : blocks(blocks)
    , blockSize(blockSize)
    , alignment(alignment)
{
    debug_assert(alignment > 0 && !(alignment & (alignment-1))); // assert align is power of two

    this->data = (uint8_t*)aligned_alloc(this->alignment, this->blocks * this->blockSize);
    this->usedList = new std::atomic_bool[this->blocks]();
    debug_assert(this->getFreeBlocksCount() == this->blocks);
}

Heap::~Heap()
{
#ifndef NDEBUG
    for (size_t i = 0; i < this->blocks; i++)
        debug_assert(!this->usedList[i]);
#endif
    free(this->data);
    delete[] this->usedList;
}

uint8_t* Heap::getBlock()
{
    for (size_t i = 0; i < this->blocks; i++)
    {
        bool expected = false;
        if (usedList[i].compare_exchange_strong(expected, true))
            return this->data + this->blockSize * i;
    }

    return nullptr;
}

void Heap::returnBlock(const uint8_t* block)
{
    debug_assert(block >= this->data && block < this->data + (this->blocks * this->blockSize));
    debug_assert((intptr_t(block) - intptr_t(this->data)) % this->blockSize == 0);

    size_t i = (block - this->data) / this->blockSize;
    debug_assert(i < this->blocks && usedList[i]);
    usedList[i] = false;
}

size_t Heap::getFreeBlocksCount() const
{
    size_t sum = 0;
    for (size_t i = 0; i < this->blocks; i++)
    {
        if (!usedList[i])
            sum++;
    }

    return sum;
}

