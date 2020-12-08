#pragma once
#include <liburing.h>
#include <mutex>
#include <atomic>
#include <thread>

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

    static constexpr unsigned int RING_SIZE = 100;
    io_uring ring = {};

    std::mutex ringMutex;
    std::atomic_uint32_t jobsRunning = 0;
    std::atomic_bool done = false;
    std::thread completionThread;
};


