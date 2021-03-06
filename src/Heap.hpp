#pragma once
#include <cstdlib>
#include <vector>
#include <cstdint>
#include <atomic>
#include <pthread.h>

// Thread safe slab allocator
class Heap
{
public:
    explicit Heap(size_t blocks, size_t blockSize, size_t alignment = 4096);
    Heap(Heap&& other);
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    uint8_t* getBlock();
    void returnBlock(uint8_t* block);

    size_t getBlockCount() const { return this->blocks; }
    size_t getBlockSize() const { return this->blockSize; }
    size_t getAlignment() const { return this->alignment; }

    // NOT thread safe
    size_t getFreeBlocksCount() const;

private:
    std::atomic_bool* usedList = nullptr;
    std::atomic_uint32_t valgrindModeUsedCount = 0;
    uint8_t* data = nullptr;
    size_t blocks = 0;
    size_t blockSize = 0;
    size_t alignment = 0;
};


