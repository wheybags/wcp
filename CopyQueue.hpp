#pragma once
#include <liburing.h>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <deque>
#include <memory>
#include <filesystem>
#include "QueueFileDescriptor.hpp"
#include "Heap.hpp"

class CopyRunner;

class CopyQueue
{
public:
    explicit CopyQueue(size_t ringSize, size_t fileDescriptorCap, Heap&& heap);
    ~CopyQueue();

    void addRecursiveCopy(std::string from, std::string dest);
    void addFileCopy(const std::string& from, const std::string& dest, const struct stat64* fromStatBuffer = nullptr);
    void start();

    enum class OnCompletionAction : uint8_t
    {
        ExitProcessNoCleanup,
        Return
    };
    void join(OnCompletionAction onCompletionAction = OnCompletionAction::Return);

    size_t getBlockSize() const { return this->copyBufferHeap.getBlockSize(); }
    size_t getHeapAlignment() const { return this->copyBufferHeap.getAlignment(); }

    static size_t minimumFileDescriptorCap() { return RESERVED_FD_COUNT + RESERVED_HIGH_PRIORITY_FD_COUNT; }

private:
    friend class CopyRunner;
    friend class QueueFileDescriptor;

    void addCopyJob(const std::string& src, const std::string& dest, const struct stat64& st);
    void addCopyJobPart(QueueFileDescriptor* sourceFd,
                        QueueFileDescriptor* destFd,
                        off_t offset,
                        off_t size,
                        size_t alignment,
                        int32_t* chunkCount);

    bool isDone();
    void exitProcess();
    void onError(Error&& error);

    void submitLoop();
    void showProgressLoop();

    static void* staticCallSubmitLoop(void* instance) { reinterpret_cast<CopyQueue *>(instance)->submitLoop(); return nullptr; }
    static void* staticCallShowProgressLoop(void* instance) { reinterpret_cast<CopyQueue*>(instance)->showProgressLoop(); return nullptr; }

private:
    size_t ringSize;
    size_t completionRingSize;
    size_t fileDescriptorCap;
    std::atomic<OnCompletionAction> completionAction = OnCompletionAction::Return;

    Heap copyBufferHeap;
    io_uring ring = {};

    std::deque<CopyRunner*> copiesPendingStart;
    pthread_mutex_t copiesPendingStartMutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;
    std::atomic_uint32_t copiesPendingStartCount = 0;

    std::atomic_uint32_t keepAliveCount = 0;
    std::atomic_uint32_t submissionsRunning = 0;

    std::atomic<size_t> totalBytesToCopy = 0;
    std::atomic<size_t> totalBytesCopied = 0;
    std::atomic<size_t> totalBytesFailed = 0;

    std::vector<std::string> errorMessages;
    pthread_mutex_t errorMessagesMutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;

    static constexpr uint64_t RESERVED_FD_COUNT = 2; // Reserved one for the ring itself, and one for directory iteration
    static constexpr uint64_t RESERVED_HIGH_PRIORITY_FD_COUNT = 2; // The real submit thread takes priority, and it needs at least 2 FDs to make progress
    std::atomic_uint64_t fileDescriptorsUsed = RESERVED_FD_COUNT;

    enum class State
    {
        Idle,
        Running,
        AdditionComplete,
    };
    std::atomic<State> state = State::Idle;

    pthread_t submitThread;
    pthread_t showProgressThread;
    pthread_mutex_t progressEndMutex = PTHREAD_MUTEX_INITIALIZER;

    bool showingProgress = false;
    bool showingErrors = true;
};


