#include "CopyRunner.hpp"
#include "CopyQueue.hpp"
#include "Assert.hpp"
#include "Config.hpp"
#include <unistd.h>
#include <liburing.h>
#include <cerrno>


CopyRunner::CopyRunner(CopyQueue* queue, int sourceFd, int destFd, off_t size)
        : queue(queue)
        , sourceFd(sourceFd)
        , destFd(destFd)
        , size(size)
        , buffer(new uint8_t[size])
{}

CopyRunner::~CopyRunner()
{
    release_assert(jobsRunning == 0);

    delete[] buffer;
    close(sourceFd);
    close(destFd);
}

void CopyRunner::addToBatch()
{
    std::scoped_lock lock(this->queue->ringMutex);

#if DEBUG_COPY_OPS
    printf("START %d->%d\n", this->sourceFd, this->destFd);
#endif

    debug_assert(this->jobsRunning == 0);
    debug_assert(this->writeOffset <= this->size);

    io_uring_sqe* sqe = nullptr;

    unsigned int bytesToRead = this->size - this->readOffset;
#if DEBUG_FORCE_PARTIAL_READS
    bytesToRead = rand() % (bytesToRead + 1);
        bool readIsPartial = bytesToRead < this->size - this->readOffset;
#endif

    if (bytesToRead)
    {
        sqe = io_uring_get_sqe(&queue->ring);
        this->jobsRunning++;

        io_uring_prep_read(sqe, this->sourceFd, this->buffer + this->readOffset, bytesToRead, this->readOffset);
        sqe->flags |= IOSQE_IO_LINK;

        static_assert(sizeof(sqe->user_data) == sizeof(void*), "");
        sqe->user_data = reinterpret_cast<__u64>(&this->readData);
    }

    if (this->writeOffset < this->size || this->size == 0)
    {
        sqe = io_uring_get_sqe(&queue->ring);
        this->jobsRunning++;

        auto prepWrite = [&]()
        {
            unsigned int bytesToWrite = this->size - this->writeOffset;
#if DEBUG_FORCE_PARTIAL_WRITES
            bytesToWrite = rand() % (bytesToWrite + 1);
#endif

            io_uring_prep_write(sqe,
                                this->destFd,
                                this->buffer + this->writeOffset,
                                bytesToWrite,
                                this->writeOffset);
        };

#if DEBUG_FORCE_PARTIAL_READS
        if (readIsPartial)
            {
                // Normally if a read is short, the following write would be cancelled.
                // To emulate this, we just send a nop event and override it's result code to -ECANCELLED.
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

    io_uring_submit(&queue->ring);
}

bool CopyRunner::onCompletionEvent(EventData::Type type, __s32 result)
{
    this->jobsRunning--;
    debug_assert(jobsRunning >= 0);

    if (type == EventData::Type::Read)
    {
#if DEBUG_COPY_OPS
        printf("RD %d->%d JR:%d RES: %d\n", this->sourceFd, this->destFd, this->jobsRunning, result);
#endif
        release_assert(result > 0);
        this->readOffset += result;
    }
    else
    {
#if DEBUG_COPY_OPS
        printf("WT %d->%d JR:%d RES: %d\n", this->sourceFd, this->destFd, this->jobsRunning, result);
#endif

        release_assert(result >= 0 || result == -ECANCELED);
        if (result > 0)
            this->writeOffset += result;
    }

    if (this->jobsRunning == 0 && this->writeOffset < this->size)
        this->addToBatch();

    return this->jobsRunning == 0 && this->writeOffset == this->size;
}