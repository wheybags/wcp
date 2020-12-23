#pragma once
#include <string>
#include <sys/types.h>
#include <memory>

class CopyQueue;

class QueueFileDescriptor
{
public:
    QueueFileDescriptor(CopyQueue& queue, const std::string& path, int oflag, mode_t mode);
    QueueFileDescriptor(CopyQueue& queue, const std::string& path, int oflag) : QueueFileDescriptor(queue, path, oflag, 0) {}
    ~QueueFileDescriptor();

    QueueFileDescriptor(const QueueFileDescriptor&) = delete;
    void operator=(const QueueFileDescriptor&) = delete;

    void ensureOpened();
    int getFd() const;

private:
    enum class OpenPriority { Low, High };
    bool tryOpen(const std::string& path, int oflag, mode_t mode, OpenPriority priority);

private:
    struct DeferredOpenData
    {
        std::string path;
        int oflag;
        mode_t mode;
    };

    CopyQueue& queue;
    std::unique_ptr<DeferredOpenData> deferredOpenData;
    int fd = -1;

#ifndef NDEBUG
    std::string debugPath;
#endif
};


