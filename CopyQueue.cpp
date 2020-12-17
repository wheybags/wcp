#include <iomanip>
#include "CopyQueue.hpp"
#include "Assert.hpp"
#include "CopyRunner.hpp"
#include "Config.hpp"

CopyQueue::CopyQueue(size_t ringSize, size_t heapBlocks, size_t heapBlockSize)
    : copyBufferHeap(heapBlocks, heapBlockSize)
    , ringSize(ringSize)
    , completionRingSize(ringSize * 2) // TODO: I think this is right, need to confirm)
{
    release_assert(io_uring_queue_init(this->ringSize, &this->ring, 0) == 0);

    release_assert(pthread_mutexattr_init(&this->mutexAttrs) == 0);
    release_assert(pthread_mutexattr_settype(&this->mutexAttrs, PTHREAD_MUTEX_ADAPTIVE_NP) == 0);
    release_assert(pthread_mutex_init(&this->copiesPendingStartMutex, &this->mutexAttrs) == 0);
}

CopyQueue::~CopyQueue()
{
    io_uring_queue_exit(&this->ring);
    debug_assert(pthread_mutex_destroy(&this->copiesPendingStartMutex) == 0);
    debug_assert(pthread_mutexattr_destroy(&this->mutexAttrs) == 0);
}

bool CopyQueue::isDone()
{
    return this->state == State::AdditionComplete && this->keepAliveCount == 0;
}


void CopyQueue::submitLoop()
{
    pthread_setname_np(pthread_self(), "Submit thread");

    // TODO: avoid busy looping? Or maybe it's fine? Not really sure tbh.
    while (true)
    {
        while(this->copiesPendingStartCount > 0 &&
              // Rate limit to avoid overflowing the completion queue
              (this->completionRingSize - this->submissionsRunning) >= CopyRunner::MAX_JOBS_PER_RUNNER)
        {
            CopyRunner* toAdd = nullptr;
            pthread_mutex_lock(&this->copiesPendingStartMutex);
            {
                debug_assert(!this->copiesPendingStart.empty());
                toAdd = this->copiesPendingStart.back();
                this->copiesPendingStart.pop_back();
            }
            pthread_mutex_unlock(&this->copiesPendingStartMutex);


            toAdd->addToBatch(); // This might block for a while, if all the heap blocks are already in use
            this->copiesPendingStartCount--;
        }

        if (this->isDone())
        {
#if DEBUG_COPY_OPS
            puts("SUBMIT THREAD EXIT");
#endif
            return;
        }
    }
}

void CopyQueue::completionLoop()
{
    pthread_setname_np(pthread_self(), "Completion thread");

    while (true)
    {
        while (this->keepAliveCount)
        {
            io_uring_cqe *cqe = nullptr;

            int err = 0;
            do
            {
                err = io_uring_wait_cqe_nr(&ring, &cqe, 1);
            } while(err == -EINTR || err == -EAGAIN);
            release_assert( err == 0);

            auto *eventData = reinterpret_cast<CopyRunner::EventData *>(cqe->user_data);
            __s32 result = cqe->res;
            if (eventData->resultOverride)
                result = eventData->resultOverride;
            eventData->resultOverride = 0;

            if (eventData->copyData->onCompletionEvent(eventData->type, result))
            {
                delete eventData->copyData;
                this->keepAliveCount--;
            }

            io_uring_cqe_seen(&ring, cqe);
        }

        if (this->isDone())
        {
#if DEBUG_COPY_OPS
            puts("COMPLETION THREAD EXIT");
#endif
            return;
        }
    }
}

void CopyQueue::showProgressLoop()
{
    pthread_setname_np(pthread_self(), "Show Progress thread");

    auto humanFriendlyFileSize = [](size_t bytes)
    {
        size_t kibibyte = 1024;
        size_t mebibyte = kibibyte * 1024;
        size_t gibibyte = mebibyte * 1024;
        size_t tebibyte = gibibyte * 1024;

        double final = bytes;
        std::string unit = "B";

        if (bytes >= tebibyte)
        {
            final = double(__float128(bytes) / __float128(tebibyte));
            unit = "TiB";
        }
        else if (bytes >= gibibyte)
        {
            final = double(bytes) / double(gibibyte);
            unit = "GiB";
        }
        else if (bytes >= mebibyte)
        {
            final = double(bytes) / double(mebibyte);
            unit = "MiB";
        }
        else if (bytes >= kibibyte)
        {
            final = double(bytes) / double(kibibyte);
            unit = "KiB";
        }

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << final;
        std::string str = ss.str();

        // strip trailing zeros
        if (str.find('.') != std::string::npos)
        {
            for (int32_t i = str.size() - 1; i >= 0; i--)
            {
                if (str[i] == '0')
                {
                    str.resize(str.size() - 1);
                }
                else if (str[i] == '.')
                {
                    str.resize(str.size() - 1);
                    break;
                }
                else
                {
                    break;
                }
            }
        }

        return str + unit;
    };

    auto showProgress = [&]()
    {
        if (int(this->state.load()) >= int(State::AdditionComplete))
        {
            int percentDone = this->totalBytesToCopy > 0 ? int(float(this->totalBytesCopied) / float(this->totalBytesToCopy) * 100.0f) : 0;

            printf("Copied %s / %s (%d%%)\n",
                   humanFriendlyFileSize(this->totalBytesCopied).c_str(),
                   humanFriendlyFileSize(this->totalBytesToCopy).c_str(),
                   percentDone);
        }
        else
        {
            printf("Copied %s / ???\n", humanFriendlyFileSize(this->totalBytesCopied).c_str());
        }
    };

    while (!this->isDone())
    {
        showProgress();
        usleep(1 * 1000000);
    }

    showProgress();
}

void CopyQueue::start()
{
    release_assert(this->state == State::Idle);
    this->state = State::Running;
    this->totalBytesToCopy = 0;
    this->totalBytesCopied = 0;

    release_assert(pthread_create(&this->submitThread, nullptr, CopyQueue::staticCallSubmitLoop, this) == 0);
    release_assert(pthread_create(&this->completionThread, nullptr, CopyQueue::staticCallCompletionLoop, this) == 0);
    release_assert(pthread_create(&this->showProgressThread, nullptr, CopyQueue::staticCallShowProgressLoop, this) == 0);
}

void CopyQueue::join()
{
    debug_assert(this->state == State::Running);
    this->state = State::AdditionComplete;

    pthread_join(this->submitThread, nullptr);
    pthread_join(this->completionThread, nullptr);
    pthread_join(this->showProgressThread, nullptr);
    this->state = State::Idle;
}

void CopyQueue::addCopyJob(std::shared_ptr<FileDescriptor> sourceFd, std::shared_ptr<FileDescriptor> destFd, off_t offset, off_t size)
{
    debug_assert(this->state == State::Running);

    this->totalBytesToCopy += size;

    pthread_mutex_lock(&this->copiesPendingStartMutex);
    {
        this->keepAliveCount++;
        this->copiesPendingStartCount++;
        this->copiesPendingStart.push_back(new CopyRunner(this, std::move(sourceFd), std::move(destFd), offset, size));
    }
    pthread_mutex_unlock(&this->copiesPendingStartMutex);
}

void CopyQueue::continueCopyJob(CopyRunner* runner)
{
    pthread_mutex_lock(&this->copiesPendingStartMutex);
    {
        this->copiesPendingStartCount++;
        this->copiesPendingStart.push_back(runner);
    }
    pthread_mutex_unlock(&this->copiesPendingStartMutex);
}


