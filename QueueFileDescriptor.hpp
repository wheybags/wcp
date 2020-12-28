#pragma once
#include <string>
#include <sys/types.h>
#include <memory>
#include "Util.hpp"

class CopyQueue;

class QueueFileDescriptor
{
public:
    QueueFileDescriptor(CopyQueue& queue, std::string path, int oflag, mode_t mode);
    QueueFileDescriptor(CopyQueue& queue, std::string path, int oflag) : QueueFileDescriptor(queue, std::move(path), oflag, 0) {}
    ~QueueFileDescriptor();

    QueueFileDescriptor(const QueueFileDescriptor&) = delete;
    void operator=(const QueueFileDescriptor&) = delete;

    [[nodiscard]] Result ensureOpened();
    void notifyClosed();

    bool hasBeenClosed() const { return this->fd == CLOSED_VAL; }
    bool isOpen() const { return this->fd > 0; }

    int getFd() const;
    const std::string& getPath() const { return this->path; }

private:
    enum class OpenPriority { Low, High };
    bool reserveFileDescriptor(OpenPriority priority);
    Result doOpen(bool showErrorMessages);

private:
    std::string path;
    int oflag;
    mode_t mode;

    CopyQueue& queue;

    static constexpr int NEVER_OPENED_VAL = -1;
    static constexpr int CLOSED_VAL = -2;
    int fd = NEVER_OPENED_VAL;
};


