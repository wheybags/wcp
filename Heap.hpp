#pragma once
#include <cstdlib>
#include <vector>
#include <cstdint>
#include <atomic>

// Thread safe slab allocator
class Heap
{
public:
    explicit Heap(size_t blocks, size_t blockSize);
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    uint8_t* getBlock();
    void returnBlock(const uint8_t* block);

    size_t getBlockSize() const { return this->blockSize; }

private:
    std::atomic_bool* usedList;
    uint8_t* data = nullptr;
    size_t blocks = 0;
    size_t blockSize = 0;
};


