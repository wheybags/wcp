#include "QueueFileDescriptor.hpp"
#include <fcntl.h>
#include "Assert.hpp"
#include "CopyQueue.hpp"
#include "Config.hpp"

QueueFileDescriptor::QueueFileDescriptor(CopyQueue& queue, std::string path, int oflag, mode_t mode)
    : path(std::move(path))
    , oflag(oflag)
    , mode(mode)
    , queue(queue)
{
    if (this->reserveFileDescriptor(OpenPriority::Low))
        this->doOpen(); // ignore errors here, it will be seen when the user calls ensureOpened()
}

QueueFileDescriptor::~QueueFileDescriptor()
{
    debug_assert(this->fd == CLOSED_VAL || this->fd == NEVER_OPENED_VAL);
}

Result QueueFileDescriptor::ensureOpened()
{
    debug_assert(this->fd != CLOSED_VAL);

    if (this->fd == NEVER_OPENED_VAL)
    {
        while(!this->reserveFileDescriptor(OpenPriority::High))
        {
            if constexpr (Config::VALGRIND_MODE)
                pthread_yield();
        }

        return this->doOpen();
    }

    return Success();
}

void QueueFileDescriptor::notifyClosed()
{
    this->fd = CLOSED_VAL;
    this->queue.fileDescriptorsUsed--;
}

int QueueFileDescriptor::getFd() const
{
    debug_assert(this->fd > 2);
    return this->fd;
}

bool QueueFileDescriptor::reserveFileDescriptor(OpenPriority priority)
{
    uint64_t expected = this->queue.fileDescriptorsUsed;
    uint64_t limit = this->queue.fileDescriptorCap;
    if (priority == OpenPriority::Low)
        limit -= CopyQueue::RESERVED_HIGH_PRIORITY_FD_COUNT;

    if (expected >= limit || !this->queue.fileDescriptorsUsed.compare_exchange_strong(expected, expected + 1))
        return false;

    return true;
}

Result QueueFileDescriptor::doOpen()
{
    OpenResult result = myOpen(this->path, this->oflag, this->mode);
    if (std::holds_alternative<Error>(result))
        return std::move(std::get<Error>(result));

    this->fd = std::get<int>(result);
    return Success();
}