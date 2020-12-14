#pragma once
#include <liburing.h>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include "FileDescriptor.hpp"

class CopyRunner;

class CopyQueue
{
public:
    CopyQueue();
    ~CopyQueue();

    void addCopyJob(std::shared_ptr<FileDescriptor> sourceFd, std::shared_ptr<FileDescriptor> destFd, off_t offset, off_t size);
    void start();
    void join();

private:
    friend class CopyRunner;
    void continueCopyJob(CopyRunner* runner);

    void submitLoop();
    void completionLoop();

    static void* staticCallSubmitLoop(void* instance) { reinterpret_cast<CopyQueue*>(instance)->submitLoop(); return nullptr; }
    static void* staticCallCompletionLoop(void* instance) { reinterpret_cast<CopyQueue*>(instance)->completionLoop(); return nullptr; }

private:

    static constexpr uint32_t RING_SIZE = 100;
    static constexpr uint32_t COMPLETION_RING_SIZE = RING_SIZE * 2; // TODO: I think this is right, need to confirm
    io_uring ring = {};

    std::vector<CopyRunner*> copiesPendingStart;
    pthread_mutexattr_t mutexAttrs;
    pthread_mutex_t copiesPendingStartMutex;

    std::atomic_uint32_t copiesPendingStartCount = 0;
    std::atomic_uint32_t keepAliveCount = 0;

    std::atomic_uint32_t submissionsRunning = 0;

    enum class State
    {
        Idle,
        Running,
        AdditionComplete,
    };
    std::atomic<State> state = State::Idle;

    pthread_t completionThread;
    pthread_t submitThread;
};


