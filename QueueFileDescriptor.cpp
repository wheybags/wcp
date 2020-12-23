#include "QueueFileDescriptor.hpp"
#include <unistd.h>
#include <fcntl.h>
#include "Assert.hpp"
#include "CopyQueue.hpp"

QueueFileDescriptor::QueueFileDescriptor(CopyQueue& queue, const std::string& path, int oflag, mode_t mode)
    : queue(queue)
{
    if (!this->tryOpen(path, oflag, mode, OpenPriority::Low))
        this->deferredOpenData.reset(new DeferredOpenData{path, oflag, mode});

#ifndef NDEBUG
    this->debugPath = path;
#endif
}

QueueFileDescriptor::~QueueFileDescriptor()
{
    debug_assert(fd > 2 ||
                 ((this->deferredOpenData->mode & O_RDONLY) == 0 && (this->deferredOpenData->mode & O_RDWR) == 0));

    if (fd != -1)
    {
        release_assert(close(fd) == 0);
        this->queue.fileDescriptorsUsed--;
    }
}

void QueueFileDescriptor::ensureOpened()
{
    if (this->deferredOpenData)
    {
        while(!this->tryOpen(this->deferredOpenData->path,
                             this->deferredOpenData->oflag,
                             this->deferredOpenData->mode,
                             OpenPriority::High))
        {}

        this->deferredOpenData.reset();
    }
}

int QueueFileDescriptor::getFd() const
{
    debug_assert(this->fd > 2);
    return this->fd;
}

bool QueueFileDescriptor::tryOpen(const std::string& path, int oflag, mode_t mode, OpenPriority priority)
{
    uint64_t expected = this->queue.fileDescriptorsUsed;
    uint64_t limit = this->queue.fileDescriptorCap;
    if (priority == OpenPriority::Low)
        limit -= CopyQueue::RESERVED_HIGH_PRIORITY_FD_COUNT;

    if (expected >= limit || !this->queue.fileDescriptorsUsed.compare_exchange_strong(expected, expected + 1))
        return false;

    this->fd = open(path.c_str(), oflag, mode);
    debug_assert(this->fd > 2);
    return true;
}