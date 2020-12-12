#pragma once
#include <liburing.h>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>

class CopyRunner;

class CopyQueue
{
public:
    CopyQueue();
    ~CopyQueue();

    void addCopyJob(int sourceFd, int destFd, off_t size);
    void start();
    void join();

private:
    friend class CopyRunner;
    void continueCopyJob(CopyRunner* runner);

private:

    static constexpr uint32_t RING_SIZE = 100;
    static constexpr uint32_t COMPLETION_RING_SIZE = RING_SIZE * 2; // TODO: I think this is right, need to confirm
    io_uring ring = {};

    std::vector<CopyRunner*> copiesPendingStart;
    std::mutex copiesPendingStartMutex;

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

    std::thread completionThread;
    std::thread submitThread;
};


