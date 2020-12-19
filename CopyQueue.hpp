#pragma once
#include <liburing.h>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include "FileDescriptor.hpp"
#include "Heap.hpp"

class CopyRunner;

class CopyQueue
{
public:
    explicit CopyQueue(size_t ringSize, size_t heapBlocks, size_t heapBlockSize);
    ~CopyQueue();

    void addCopyJob(std::shared_ptr<FileDescriptor> sourceFd,
                    std::shared_ptr<FileDescriptor> destFd,
                    off_t offset, off_t size, size_t alignment);
    void start();

    enum class OnCompletionAction : uint8_t
    {
        ExitProcessNoCleanup,
        Return
    };
    void join(OnCompletionAction onCompletionAction);

    size_t getBlockSize() const { return this->copyBufferHeap.getBlockSize(); }
    size_t getHeapAlignment() const { return this->copyBufferHeap.getAlignment(); }

private:
    friend class CopyRunner;
    void continueCopyJob(CopyRunner* runner);

    bool isDone();
    void exitProcess();

    void submitLoop();
    void completionLoop();
    void showProgressLoop();

    static void* staticCallSubmitLoop(void* instance) { reinterpret_cast<CopyQueue*>(instance)->submitLoop(); return nullptr; }
    static void* staticCallCompletionLoop(void* instance) { reinterpret_cast<CopyQueue*>(instance)->completionLoop(); return nullptr; }
    static void* staticCallShowProgressLoop(void* instance) { reinterpret_cast<CopyQueue*>(instance)->showProgressLoop(); return nullptr; }

private:

    size_t ringSize;
    size_t completionRingSize;
    std::atomic<OnCompletionAction> completionAction = OnCompletionAction::Return;

    Heap copyBufferHeap;
    io_uring ring = {};

    std::vector<CopyRunner*> copiesPendingStart;
    std::vector<CopyRunner*> copiesPendingContinue;
    pthread_mutexattr_t mutexAttrs;
    pthread_mutex_t copiesPendingStartMutex;

    std::atomic_uint32_t copiesPendingStartCount = 0;
    std::atomic_uint32_t keepAliveCount = 0;

    std::atomic_uint32_t submissionsRunning = 0;

    std::atomic<size_t> totalBytesToCopy = 0;
    std::atomic<size_t> totalBytesCopied = 0;

    enum class State
    {
        Idle,
        Running,
        AdditionComplete,
    };
    std::atomic<State> state = State::Idle;

    pthread_t completionThread;
    pthread_t submitThread;

    pthread_t showProgressThread;
    bool showingProgress = false;
    pthread_mutex_t progressEndMutex = PTHREAD_MUTEX_INITIALIZER;
};


