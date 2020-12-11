#include "CopyQueue.hpp"
#include "Assert.hpp"
#include "CopyRunner.hpp"
#include "Config.hpp"

CopyQueue::CopyQueue()
{
    release_assert(io_uring_queue_init(RING_SIZE, &this->ring, 0) == 0);
}

CopyQueue::~CopyQueue()
{
    io_uring_queue_exit(&this->ring);
}

void CopyQueue::start()
{
    release_assert(this->state == State::Idle);
    this->state = State::Running;

    this->submitThread = std::thread([&](){
        // TODO: avoid busy looping? Or maybe it's fine? Not really sure tbh.
        while (true)
        {
            while (this->copiesPendingStartCount)
            {
                while(this->copiesPendingStartCount > 0 &&
                      // Rate limit to avoid overflowing the completion queue
                      (COMPLETION_RING_SIZE -this->submissionsRunning) >= CopyRunner::MAX_JOBS_PER_RUNNER)
                {
                    CopyRunner* toAdd = nullptr;
                    {
                        std::scoped_lock lock(this->copiesPendingStartMutex);
                        debug_assert(!this->copiesPendingStart.empty());
                        toAdd = this->copiesPendingStart.back();
                        this->copiesPendingStart.pop_back();
                    }

                    toAdd->addToBatch();
                    this->copiesRunning++;
                    this->copiesPendingStartCount--;
                }
            }

            if (this->state == State::AdditionComplete && this->copiesRunning == 0 && this->copiesPendingStartCount == 0)
            {
#if DEBUG_COPY_OPS
                puts("SUBMIT THREAD EXIT");
#endif
                return;
            }
        }
    });

    this->completionThread = std::thread([&]() {
        while (true)
        {
            while (this->copiesRunning)
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
                    copiesRunning--;
                }

                io_uring_cqe_seen(&ring, cqe);
            }

            if (this->state == State::AdditionComplete && this->copiesRunning == 0 && this->copiesPendingStartCount == 0)
            {
#if DEBUG_COPY_OPS
                puts("COMPLETION THREAD EXIT");
#endif
                return;
            }
        }
    });
}

void CopyQueue::join()
{
    this->state = State::AdditionComplete;
    this->submitThread.join();
    this->completionThread.join();
    this->state = State::Idle;
}

void CopyQueue::addCopyJob(int sourceFd, int destFd, off_t size)
{
    debug_assert(this->state == State::Running);

    std::scoped_lock lock(this->copiesPendingStartMutex);
    this->copiesPendingStartCount++;
    this->copiesPendingStart.push_back(new CopyRunner(this, sourceFd, destFd, size));
}

void CopyQueue::continueCopyJob(CopyRunner* runner)
{
    std::scoped_lock lock(this->copiesPendingStartMutex);
    this->copiesPendingStartCount++;
    this->copiesRunning--;
    this->copiesPendingStart.push_back(runner);
}

