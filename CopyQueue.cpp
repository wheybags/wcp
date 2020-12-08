#include "CopyQueue.hpp"
#include "Assert.hpp"
#include "CopyRunner.hpp"

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
    this->done = false;

    this->completionThread = std::thread([&]() {
        while (true)
        {
            while (this->jobsRunning)
            {
                io_uring_cqe *cqe = nullptr;
                release_assert(io_uring_wait_cqe_nr(&ring, &cqe, 1) == 0);

                auto *eventData = reinterpret_cast<CopyRunner::EventData *>(cqe->user_data);
                __s32 result = cqe->res;
                if (eventData->resultOverride)
                    result = eventData->resultOverride;
                eventData->resultOverride = 0;

                if (eventData->copyData->onCompletionEvent(eventData->type, result))
                {
                    delete eventData->copyData;
                    jobsRunning--;
                }

                io_uring_cqe_seen(&ring, cqe);
            }

            while (this->jobsRunning == 0)
            {
                if (this->done)
                    return true;
            }
        }
    });
}

void CopyQueue::join()
{
    this->done = true;
    this->completionThread.join();
}


void CopyQueue::addCopyJob(int sourceFd, int destFd, off_t size)
{
    while(jobsRunning > RING_SIZE) {} // this is a bit nasty alright

    jobsRunning++;
    (new CopyRunner(this, sourceFd, destFd, size))->addToBatch();
}

