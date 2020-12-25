#include "CopyRunner.hpp"
#include "CopyQueue.hpp"
#include "Assert.hpp"
#include "Config.hpp"
#include <liburing.h>
#include <cerrno>
#include <cstring>

CopyRunner::CopyRunner(CopyQueue* queue,
                       std::shared_ptr<QueueFileDescriptor> sourceFd,
                       std::shared_ptr<QueueFileDescriptor> destFd,
                       off_t offset,
                       off_t size,
                       size_t alignment)
        : queue(queue)
        , sourceFd(std::move(sourceFd))
        , destFd(std::move(destFd))
        , offset(offset)
        , size(size)
        , alignment(alignment)
        , readOffset(offset)
        , writeOffset(offset)
{
    this->queue->keepAliveCount++;
}

CopyRunner::~CopyRunner()
{
    release_assert(jobsRunning == 0);

    if (this->buffer)
        this->queue->copyBufferHeap.returnBlock(this->buffer);

    this->queue->keepAliveCount--;
}

void CopyRunner::giveBuffer(uint8_t* _buffer)
{
    debug_assert(this->buffer == nullptr);
    this->buffer = _buffer;

    // We need to align the buffer because we opened the source file with O_DIRECT.
    // The calling code should make sure that we have enough space to fit this->size bytes in our heap block
    // after alignment, but we do some asserts just to be sure.
    this->bufferAligned = this->buffer + (this->alignment - this->queue->copyBufferHeap.getAlignment());
    release_assert(intptr_t(this->bufferAligned) % this->alignment == 0);
    release_assert(this->bufferAligned + this->size <= this->buffer + this->queue->copyBufferHeap.getBlockSize());
}

Result CopyRunner::addToBatch()
{
    debug_assert(!this->needsBuffer());

    {
        Result result;

        for (auto& fd : {this->sourceFd, this->destFd})
        {
            result = fd->ensureOpened();
            if (std::holds_alternative<Error>(result))
                break;
        }

        if (std::holds_alternative<Error>(result))
        {
            // TODO: account for other fragments of this copy
            this->queue->totalBytesFailed += this->size;
            return result;
        }
    }

    if (Config::DEBUG_COPY_OPS)
        printf("START %d->%d %ld\n", this->sourceFd->getFd(), this->destFd->getFd(), this->offset);

    release_assert(this->jobsRunning == 0);
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

    if (Config::DEBUG_FORCE_PARTIAL_READS && bytesToRead > 0)
        bytesToRead = (rand() % bytesToRead) + 1; // ensure we never force a read of zero, as that would indicate EOF

    bool readForcedPartial = bytesToRead < this->offset + this->size - this->readOffset;
    unsigned int unalignedReadSize = bytesToRead;

    if (bytesToRead)
    {
        io_uring_sqe* sqe = getSqe();

        // Account for O_DIRECT alignment requirements on bytesToRead.
        // O_DIRECT requires not only the destination buffer to be aligned, but also the byte count.
        // The only reason we should end up with a bytesToRead value that is not a multiple of alignment is when we are at the
        // end of a file whose size is not a multiple of alignment. The rules still apply, even at the end of a file.
        // For example, let's say alignment is 4096, and we are reading a 10-byte long file. We must issue a read of 4096 bytes,
        // even though we know the file is only 10 bytes long, because the O_DIRECT api requires we do so. Of course, we'll
        // get a short read, and only 10 bytes will be written, so all is well. It does mean the subsequent write will be cancelled,
        // but it will go through fine the next time this runner is scheduled, so no problem.
        if (bytesToRead % this->alignment != 0)
        {
            size_t blocks = (bytesToRead / this->alignment) + 1;
            bytesToRead = blocks * this->alignment;
        }

        io_uring_prep_read(sqe, this->sourceFd->getFd(), this->bufferAligned + this->readOffset - this->offset, bytesToRead, this->readOffset);
        sqe->flags |= IOSQE_IO_LINK; // The following write command will fail with ECANCELLED if this read doesn't fully complete

        static_assert(sizeof(sqe->user_data) == sizeof(void*));
        sqe->user_data = reinterpret_cast<__u64>(&this->readData);

        if (Config::DEBUG_FORCE_PARTIAL_READS)
        {
            if (readForcedPartial)
                this->readData.resultOverride = unalignedReadSize;
        }
    }

    if (this->writeOffset < this->offset + this->size || this->size == 0)
    {
        io_uring_sqe* sqe = getSqe();

        auto prepWrite = [&]()
        {
            unsigned int bytesToWrite = this->offset + this->size - this->writeOffset;
            if (Config::DEBUG_FORCE_PARTIAL_WRITES)
                bytesToWrite = rand() % (bytesToWrite + 1);

            io_uring_prep_write(sqe,
                                this->destFd->getFd(),
                                this->bufferAligned + this->writeOffset - this->offset,
                                bytesToWrite,
                                this->writeOffset);
        };

        if (Config::DEBUG_FORCE_PARTIAL_READS)
        {
            if (readForcedPartial)
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
        }
        else
        {
            prepWrite();
        }

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

    return Success();
}

CopyRunner::RunnerResult CopyRunner::onCompletionEvent(EventData::Type type, __s32 result)
{
    this->jobsRunning--;
    debug_assert(jobsRunning >= 0);

    if (this->jobsRunning == 0 && this->deferredError)
        return std::move(*this->deferredError);

    std::optional<Error> error;

    if (type == EventData::Type::Read)
    {
        if (Config::DEBUG_COPY_OPS)
        {
            printf("RD %d->%d %ld JR:%d RES: %d %s\n",
                   this->sourceFd->getFd(), this->destFd->getFd(), this->offset, this->jobsRunning, result, (result < 0 ? strerror(-result) : ""));
        }

        // result of zero indicates EOF. This will happen if the file is truncated by another process.
        // If this happens, then we just bail out. Someone else is modifying the file as we read it, so there's not much else we can do.
        if (result == 0)
        {
            if (this->queue->showingErrors)
                error = Error("File shrank while copying: \"" + this->sourceFd->getPath() + "\"");
            else
                error = Error();
        }
        if (result < 0)
        {
            if (this->queue->showingErrors)
                error = Error("Error reading file \"" + this->sourceFd->getPath() + "\": \"" + strerror(-result) + "\"");
            else
                error = Error();
        }
        else
        {
            this->readOffset += result;

            // If we're not at the end of the file, and we read up to a non-aligned offset, then back up until
            // we're aligned again. This probably won't happen in the real world, but the DEBUG_FORCE_PARTIAL_READS mode
            // does make it happen, so we handle it. Also it's not guaranteed to never happen for real.
            unsigned int bytesToRead = this->offset + this->size - this->readOffset;
            if (bytesToRead)
                this->readOffset = (this->readOffset / this->alignment) * this->alignment;
        }
    }
    else
    {
        if (Config::DEBUG_COPY_OPS)
        {
            printf("WT %d->%d %ld JR:%d RES: %d %s\n",
                   this->sourceFd->getFd(), this->destFd->getFd(), this->size, this->jobsRunning, result, (result < 0 ? strerror(-result) : ""));
        }

        if (result < 0)
        {
            // ECANCELED means the preceding read failed or didn't fully complete.
            // This is set up with the IOSQE_IO_LINK flag on the read submission.
            if (result != -ECANCELED)
            {
                if (this->queue->showingErrors)
                    error = Error("Error writing file \"" + this->destFd->getPath() + "\": \"" + strerror(-result) + "\"");
                else
                    error = Error();
            }
        }
        else
        {
            this->writeOffset += result;
            this->queue->totalBytesCopied += result;
        }
    }

    debug_assert(this->writeOffset <= this->offset + this->size);

    if (error)
    {
        if (this->jobsRunning > 0)
            this->deferredError = std::move(*error);
        else
            return std::move(*error);
    }

    if (this->jobsRunning == 0)
    {
        if (this->writeOffset == this->offset + this->size)
            return FinishedTag();
        else
            return RescheduleTag();
    }

    return ContinueTag();
}