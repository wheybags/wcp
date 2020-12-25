#include "QueueFileDescriptor.hpp"
#include <fcntl.h>
#include <unistd.h>
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
        this->doOpen(false); // ignore errors here, it will be seen when the user calls ensureOpened()
}

QueueFileDescriptor::~QueueFileDescriptor()
{
    debug_assert(fd > 2 || ((this->mode & O_RDONLY) == 0 && (this->mode & O_RDWR) == 0));

    if (fd != -1)
    {
        Result result = myClose(this->fd, this->queue.showingErrors);
        if (std::holds_alternative<Error>(result))
            this->queue.onError(std::move(std::get<Error>(result)));

        this->queue.fileDescriptorsUsed--;
    }
}

Result QueueFileDescriptor::ensureOpened()
{
    if (fd == -1)
    {
        while(!this->reserveFileDescriptor(OpenPriority::High))
        {
        }

        return this->doOpen(this->queue.showingErrors);
    }

    return Success();
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

Result QueueFileDescriptor::doOpen(bool showErrorMessages)
{
    OpenResult result = myOpen(this->path, this->oflag, this->mode, showErrorMessages);
    if (std::holds_alternative<Error>(result))
        return Error(std::move(std::get<Error>(result)));

    this->fd = std::get<int>(result);
    return Success();
}