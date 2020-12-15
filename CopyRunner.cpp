#include "CopyRunner.hpp"
#include "CopyQueue.hpp"
#include "Assert.hpp"
#include "Config.hpp"
#include <liburing.h>
#include <cerrno>


CopyRunner::CopyRunner(CopyQueue* queue,
                       std::shared_ptr<FileDescriptor> sourceFd,
                       std::shared_ptr<FileDescriptor> destFd,
                       off_t offset,
                       off_t size)
        : queue(queue)
        , sourceFd(std::move(sourceFd))
        , destFd(std::move(destFd))
        , offset(offset)
        , size(size)
        , readOffset(offset)
        , writeOffset(offset)
{}

CopyRunner::~CopyRunner()
{
    release_assert(jobsRunning == 0);

    if (this->buffer)
        this->queue->copyBufferHeap.returnBlock(this->buffer);
}

void CopyRunner::addToBatch()
{
    while(!this->buffer)
        this->buffer = this->queue->copyBufferHeap.getBlock(); // TODO: we need to align the buffer

#if DEBUG_COPY_OPS
    printf("START %d->%d\n", this->sourceFd->fd, this->destFd->fd);
#endif

    debug_assert(this->jobsRunning == 0);
    debug_assert(this->writeOffset <= this->offset + this->size);

    auto getSqe = [&]()
    {
        this->jobsRunning++;
        this->queue->submissionsRunning++;
        io_uring_sqe* sqe = nullptr;

        // TODO: we busy loop here
        while (!sqe)
            sqe = io_uring_get_sqe(&this->queue->ring);
        return sqe;
    };

    unsigned int bytesToRead = this->offset + this->size - this->readOffset;
#if DEBUG_FORCE_PARTIAL_READS
    bytesToRead = rand() % (bytesToRead + 1);
    bool readIsPartial = bytesToRead < this->offset + this->size - this->readOffset;
#endif

    if (bytesToRead)
    {
        io_uring_sqe* sqe = getSqe();

        io_uring_prep_read(sqe, this->sourceFd->fd, this->buffer + this->readOffset - this->offset, bytesToRead, this->readOffset);
        sqe->flags |= IOSQE_IO_LINK;

        static_assert(sizeof(sqe->user_data) == sizeof(void*));
        sqe->user_data = reinterpret_cast<__u64>(&this->readData);
    }

    if (this->writeOffset < this->offset + this->size || this->size == 0)
    {
        io_uring_sqe* sqe = getSqe();

        auto prepWrite = [&]()
        {
            unsigned int bytesToWrite = this->offset + this->size - this->writeOffset;
#if DEBUG_FORCE_PARTIAL_WRITES
            bytesToWrite = rand() % (bytesToWrite + 1);
#endif
            io_uring_prep_write(sqe,
                                this->destFd->fd,
                                this->buffer + this->writeOffset - this->offset,
                                bytesToWrite,
                                this->writeOffset);
        };

#if DEBUG_FORCE_PARTIAL_READS
        if (readIsPartial)
            {
                // Normally if a read is short, the following write would be cancelled.
                // To emulate this, we just send a nop event and override its result code to -ECANCELLED.
                io_uring_prep_nop(sqe);
                this->writeData.resultOverride = -ECANCELED;
            }
            else
            {
                prepWrite();
            }
#else
        prepWrite();
#endif
        sqe->user_data = reinterpret_cast<__u64>(&this->writeData);
    }

    debug_assert(this->jobsRunning <= MAX_JOBS_PER_RUNNER);

    int ret = 0;
    do
    {
        ret = io_uring_submit(&queue->ring);
    }
    while (ret == -EAGAIN || ret == -EINTR);
    release_assert(ret > 0);
}

bool CopyRunner::onCompletionEvent(EventData::Type type, __s32 result)
{
    this->jobsRunning--;
    this->queue->submissionsRunning--;
    debug_assert(jobsRunning >= 0);

    if (type == EventData::Type::Read)
    {
#if DEBUG_COPY_OPS
        printf("RD %d->%d JR:%d RES: %d %s\n",
               this->sourceFd->fd, this->destFd->fd, this->jobsRunning, result, (result < 0 ? strerror(-result) : ""));
#endif
        release_assert(result > 0);
        this->readOffset += result;
    }
    else
    {
#if DEBUG_COPY_OPS
        printf("WT %d->%d JR:%d RES: %d %s\n",
               this->sourceFd->fd, this->destFd->fd, this->jobsRunning, result, (result < 0 ? strerror(-result) : ""));
#endif

        release_assert(result >= 0 || result == -ECANCELED);
        if (result > 0)
            this->writeOffset += result;
    }

    if (this->jobsRunning == 0 && this->writeOffset < this->offset + this->size)
        this->queue->continueCopyJob(this);

    return this->jobsRunning == 0 && this->writeOffset == this->offset + this->size;
}