#include "Heap.hpp"
#include "Assert.hpp"

Heap::Heap(size_t blocks, size_t blockSize)
    : blocks(blocks)
    , blockSize(blockSize)
{
    this->data = (uint8_t*)aligned_alloc(4096, this->blocks * this->blockSize);
    this->usedList = new std::atomic_bool[this->blocks];
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
    size_t i = (block - this->data) / this->blockSize;
    usedList[i] = false;
}

