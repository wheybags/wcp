#include <iomanip>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include "CopyQueue.hpp"
#include "Assert.hpp"
#include "CopyRunner.hpp"
#include "Config.hpp"
#include "Util.hpp"
#include "ScopedFileDescriptor.hpp"

CopyQueue::CopyQueue(size_t requestedRingSize, size_t fileDescriptorCap, Heap&& heap)
    : fileDescriptorCap(fileDescriptorCap)
    , copyBufferHeap(std::move(heap))
{
    release_assert(this->fileDescriptorCap >= CopyQueue::minimumFileDescriptorCap());

    io_uring_params params = {};
    release_assert(io_uring_queue_init_params(requestedRingSize, &this->ring, &params) == 0);
    this->ringSize = params.sq_entries;
}

CopyQueue::~CopyQueue()
{
    io_uring_queue_exit(&this->ring);
    [[maybe_unused]] int ret = pthread_mutex_destroy(&this->copiesPendingStartMutex);
    debug_assert(ret == 0);
}

bool CopyQueue::isDone()
{
    return this->state == State::AdditionComplete && this->keepAliveCount == 0;
}

void CopyQueue::exitProcess()
{
    // When being used as a single queue for one copy operation, we can just exit immediately when we're done.
    // No need to carefully clean up all our threads and memory allocations, the kernel will clean up for us.
    // Using _exit disables any registered atexit handlers from running.
    _exit(int(this->errored));
}

void CopyQueue::onError(Error&& error)
{
    this->errored = true;

    if (this->showingErrors)
    {
        if (this->showingProgress)
        {
            pthread_mutex_lock(&this->errorMessagesMutex);

            this->errorMessages.emplace_back(std::move(*error.humanFriendlyErrorMessage));

            pthread_mutex_unlock(&this->errorMessagesMutex);
        }
        else
        {
            fprintf(stderr, "%s\n", error.humanFriendlyErrorMessage->c_str());
        }
    }
}

void CopyQueue::submitLoop()
{
    pthread_setname_np(pthread_self(), "submit");

    uint8_t* nextBuffer = nullptr;
    std::deque<CopyRunner*> copiesPendingContinue;

    while (true)
    {
        if constexpr (Config::VALGRIND_MODE)
            pthread_yield();

        // SUBMIT
        int32_t runnersAdded = 0;
        bool doneAdding = false;
        while (!doneAdding)
        {
            doneAdding = true;

            size_t waitingForStart = this->copiesPendingStartCount + copiesPendingContinue.size();
            bool rateLimited = (this->ringSize - this->submissionsRunning) < CopyRunner::MAX_JOBS_PER_RUNNER;

            if (waitingForStart == 0 || rateLimited)
                break;

            if (!nextBuffer)
                nextBuffer = this->copyBufferHeap.getBlock();

            CopyRunner* toAdd = nullptr;
            if (!copiesPendingContinue.empty())
            {
                toAdd = copiesPendingContinue.front();
                debug_assert(!toAdd->needsBuffer());

                copiesPendingContinue.pop_front();
            }

            if (!toAdd && nextBuffer && this->copiesPendingStartCount > 0)
            {
                pthread_mutex_lock(&this->copiesPendingStartMutex);
                {
                    debug_assert(!this->copiesPendingStart.empty());

                    CopyRunner* temp = this->copiesPendingStart.front();

                    bool outOfFds = this->fileDescriptorCap - this->fileDescriptorsUsed < RESERVED_HIGH_PRIORITY_FD_COUNT;
                    if (!outOfFds || !temp->needsFileDescriptors())
                    {
                        if (temp->needsBuffer())
                        {
                            temp->giveBuffer(nextBuffer);
                            nextBuffer = nullptr;
                        }

                        toAdd = temp;

                        this->copiesPendingStart.pop_front();
                        this->copiesPendingStartCount--;
                    }
                }
                pthread_mutex_unlock(&this->copiesPendingStartMutex);
            }

            if (toAdd)
            {
                if (toAdd->size == 0)
                {
                    // In this case, there's no actual copying to be done, just open and close the dest file

                    Result result = toAdd->destFd->ensureOpened();
                    if (!std::holds_alternative<Error>(result))
                        result = toAdd->handleFileClose();

                    if (std::holds_alternative<Error>(result))
                        this->onError(std::move(std::get<Error>(result)));

                    delete toAdd;
                    doneAdding = false;
                }
                else
                {
                    Result result = toAdd->addToBatch();

                    if (std::holds_alternative<Error>(result))
                    {
                        delete toAdd;
                        this->onError(std::move(std::get<Error>(result)));
                    }
                    else
                    {
                        doneAdding = false;
                        runnersAdded++;
                    }
                }
            }
        }

        if (runnersAdded)
        {
            int ret = 0;
            do
            {
                ret = io_uring_submit(&this->ring);
            }
            while (ret == -EAGAIN || ret == -EINTR);
            release_assert(ret > 0);
        }

        // COMPLETE
        while (this->submissionsRunning)
        {
            io_uring_cqe *cqe = nullptr;
            int err = io_uring_peek_cqe(&ring, &cqe);
            release_assert(err == 0 || err == -EINTR || err == -EAGAIN);

            if (err != 0)
                break;

            auto *eventData = reinterpret_cast<CopyRunner::EventData *>(cqe->user_data);

            CopyRunner::RunnerResult runnerResult = eventData->copyData->onCompletionEvent(*eventData, cqe->res);
            if (std::holds_alternative<CopyRunner::FinishedTag>(runnerResult))
            {
                Result closeResult = eventData->copyData->handleFileClose();
                if (std::holds_alternative<Error>(closeResult))
                    this->onError(std::move(std::get<Error>(closeResult)));
                delete eventData->copyData;
            }
            else if (std::holds_alternative<Error>(runnerResult))
            {
                this->onError(std::move(std::get<Error>(runnerResult)));
                delete eventData->copyData;
            }
            else if(std::holds_alternative<CopyRunner::RescheduleTag>(runnerResult))
            {
                copiesPendingContinue.push_back(eventData->copyData);
            }

            io_uring_cqe_seen(&ring, cqe);
            this->submissionsRunning--;
        }

        if (this->isDone())
        {
            if (Config::DEBUG_COPY_OPS)
                puts("COMPLETION THREAD EXIT");

            if (completionAction == OnCompletionAction::ExitProcessNoCleanup && !this->showingProgress)
                this->exitProcess();

            if (nextBuffer)
                this->copyBufferHeap.returnBlock(nextBuffer);

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

        return str + " " + unit;
    };

    auto leftPad = [](const std::string& str, int32_t targetLength)
    {
        std::string pad;
        for (int32_t i = 0; i < targetLength - int32_t(str.length()); i++)
            pad += " ";
        return pad + str;
    };

    auto rightPad = [](const std::string& str, int32_t targetLength)
    {
        std::string pad;
        for (int32_t i = 0; i < targetLength - int32_t(str.length()); i++)
            pad += " ";
        return str + pad;
    };

    auto centreAlign = [](const std::string& str, int32_t lineWidth)
    {
        std::string pad;
        for (uint32_t i = 0; i < (lineWidth / 2) - (str.length() / 2); i++)
            pad += " ";
        return pad + str;
    };

    bool firstShow = true;
    auto showProgress = [&]()
    {
        int32_t termWidth = 100;
        if (!Config::PROGRESS_DEBUG_SIMPLE)
        {
            winsize winsize = {};
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsize);
            termWidth = winsize.ws_col;
        }

        if (!firstShow && !Config::VALGRIND_MODE && !Config::PROGRESS_DEBUG_SIMPLE) // don't overwrite valgrind output
        {
            // return to start of line, and move three lines up (ie, move cursor to the top left of our draw area)
            fputs("\r\033[2A", stderr);
        }

        auto showLine = [&](const std::string& line)
        {
            fputs((rightPad(line, termWidth) + "\n").c_str(), stderr);
        };

        std::vector<std::string> localErrorMessages;
        {
            pthread_mutex_lock(&this->errorMessagesMutex);
            if (!this->errorMessages.empty())
               this->errorMessages.swap(localErrorMessages);
            pthread_mutex_unlock(&this->errorMessagesMutex);
        }

        for (const auto& message: localErrorMessages)
            showLine(message);

        bool haveTotal = int(this->state.load()) >= int(State::AdditionComplete);

        // show raw amount copied
        {
            int32_t sectionLength = 10;

            std::string status = leftPad(humanFriendlyFileSize(this->totalBytesCopied), sectionLength) + " / ";
            if (haveTotal)
                status += leftPad(humanFriendlyFileSize(this->totalBytesToCopy), sectionLength);
            else
                status += "???";

            showLine(centreAlign(status, termWidth));
        }


        if (!haveTotal)
        {
            showLine(centreAlign("Found so far: " + humanFriendlyFileSize(this->totalBytesToCopy), termWidth));
        }
        else  // progress bar
        {
            float ratio = this->totalBytesToCopy > 0 ? float(this->totalBytesCopied) / float(this->totalBytesToCopy) : 0;
            int percentDone = int(ratio * 100.0f);

            int32_t width = termWidth - 6;

            std::string percentString = std::to_string(percentDone);
            std::string progressBarLine = leftPad(percentString, 3) + "% ";

            int doneChars = int(ratio * width);
            for (int32_t i = 0; i < width; i++)
                progressBarLine += i < doneChars ? "█" : "▒";

            showLine(progressBarLine);
        }

        firstShow = false;
    };

    fputs("\n", stderr);

    const float updateIntervalSeconds = 0.25f;

    while (!this->isDone())
    {
        showProgress();

        // wait for the specified time, but allow fast exit when we're done (by calling thread unlocking the mutex)
        {
            // I hate this data structure...
            timespec timeoutTime = {};
            clock_gettime(CLOCK_REALTIME, &timeoutTime);
            timeoutTime.tv_sec += time_t(std::floor(updateIntervalSeconds));
            timeoutTime.tv_nsec += long((updateIntervalSeconds - std::floor(updateIntervalSeconds)) * 1000000000.0f);
            if (timeoutTime.tv_nsec > 1000000000L)
            {
                timeoutTime.tv_sec += 1;
                timeoutTime.tv_nsec -= 1000000000L;
            }

            int err = pthread_mutex_timedlock(&this->progressEndMutex, &timeoutTime);
            release_assert(err == 0 || err == ETIMEDOUT);
            if (err == 0)
            {
                pthread_mutex_unlock(&this->progressEndMutex);
                break;
            }
        }
    }

    showProgress();
    if (completionAction == OnCompletionAction::ExitProcessNoCleanup)
        this->exitProcess();
}

void CopyQueue::start()
{
    release_assert(this->state == State::Idle);
    this->state = State::Running;

    release_assert(pthread_create(&this->submitThread, nullptr, CopyQueue::staticCallSubmitLoop, this) == 0);

    // Don't try to show a progress bar if we're not outputting to a terminal
    this->showingProgress = Config::PROGRESS_DEBUG_SIMPLE || (isatty(STDERR_FILENO) && getenv("TERM"));
    if (this->showingProgress)
    {
        pthread_mutex_lock(&this->progressEndMutex);
        release_assert(pthread_create(&this->showProgressThread, nullptr, CopyQueue::staticCallShowProgressLoop, this) == 0);
    }
}

bool CopyQueue::join(OnCompletionAction onCompletionAction)
{
    debug_assert(this->state == State::Running);
    this->state = State::AdditionComplete;
    this->completionAction = onCompletionAction;

    pthread_join(this->submitThread, nullptr);

    debug_assert(this->submissionsRunning == 0);
    debug_assert(this->copyBufferHeap.getFreeBlocksCount() == this->copyBufferHeap.getBlockCount());

    if (this->showingProgress)
    {
        pthread_mutex_unlock(&this->progressEndMutex); // signals the progress thread to stop sleeping, if it is ATM
        pthread_join(this->showProgressThread, nullptr);
    }
    this->state = State::Idle;
    this->totalBytesToCopy = 0;
    this->totalBytesCopied = 0;
    this->totalBytesFailed = 0;
    this->errored = false;

    return !this->errored;
}

void CopyQueue::addRecursiveCopy(std::string from, std::string dest)
{
    debug_assert(!from.empty() && !dest.empty());
    if (from.back() != '/')
        from += '/';
    if (dest.back() != '/')
        dest += '/';



    recursiveMkdir(dest);

    std::vector<std::string> directoryStack;
    directoryStack.emplace_back(from);

    std::vector<uint8_t> dirBuffer;
    dirBuffer.resize(1024 * 1024 * 32); // 32mb, should be enough for a one-shot almost always

    while (!directoryStack.empty())
    {
        std::string current = std::move(directoryStack.back());
        directoryStack.pop_back();

        ScopedFileDescriptor currentFd;
        {
            Result result = currentFd.open(current, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
            if (std::holds_alternative<Error>(result))
            {
                this->onError(std::move(std::get<Error>(result)));
                continue;
            }
        }

        ssize_t written = 0;
        do
        {
            {
                GetDentsResult result = myGetDents(currentFd.getFd(), current, dirBuffer.data(), dirBuffer.size());
                if (std::holds_alternative<Error>(result))
                {
                    this->onError(std::move(std::get<Error>(result)));
                    continue;
                }

                written = std::get<size_t>(result);
            }

            uint8_t* nextPtr = dirBuffer.data();
            while (nextPtr < dirBuffer.data() + written)
            {
                linux_dirent64* currentEntry = reinterpret_cast<linux_dirent64*>(nextPtr);
                nextPtr += currentEntry->d_reclen;

                std::string fullPath = current + currentEntry->d_name;

                unsigned char type = currentEntry->d_type;

                struct statx sb = {};
                bool didStat = false;

                if (currentEntry->d_type == DT_UNKNOWN)
                {
                    {
                        Result result = myStatx(AT_FDCWD, fullPath, 0, STATX_BASIC_STATS, sb);
                        if (std::holds_alternative<Error>(result))
                        {
                            this->onError(std::move(std::get<Error>(result)));
                            continue;
                        }
                        didStat = true;
                    }

                    if (S_ISFIFO(sb.stx_mode))
                        type = DT_FIFO;
                    else if (S_ISCHR(sb.stx_mode))
                        type = DT_CHR;
                    else if (S_ISDIR(sb.stx_mode))
                        type = DT_DIR;
                    else if (S_ISREG(sb.stx_mode))
                        type = DT_REG;
                    else if (S_ISLNK(sb.stx_mode))
                        type = DT_LNK;
                    else if (S_ISSOCK(sb.stx_mode))
                        type = DT_SOCK;
                }

                std::string destPath = dest + (fullPath.data() + from.length());

                if (type == DT_DIR)
                {
                    if (strcmp(currentEntry->d_name, ".") != 0 && strcmp(currentEntry->d_name, "..") != 0)
                    {
                        recursiveMkdir(destPath);
                        fullPath += '/';
                        directoryStack.emplace_back(std::move(fullPath));
                    }
                }
                else if (type == DT_REG)
                {
                    if (!didStat)
                    {
                        Result result = myStatx(AT_FDCWD, fullPath, 0, STATX_BASIC_STATS, sb);
                        if (std::holds_alternative<Error>(result))
                        {
                            this->onError(std::move(std::get<Error>(result)));
                            continue;
                        }
                        didStat = true;
                    }

                    this->addCopyJob(fullPath, destPath, sb);
                }
                else
                {
                    release_assert(false); // not handled yet
                }
            }

        } while (written != 0);
    }
}

void CopyQueue::addFileCopy(const std::string& from, const std::string& dest, const struct statx* fromStatBuffer)
{
    std::unique_ptr<struct statx> tmp;
    if (!fromStatBuffer)
    {
        tmp = std::make_unique<struct statx>();
        fromStatBuffer = tmp.get();
        Result result = myStatx(AT_FDCWD, from, 0, STATX_BASIC_STATS, *tmp);
        if (std::holds_alternative<Error>(result))
        {
            this->onError(std::move(std::get<Error>(result)));
            return;
        }
    }

    this->addCopyJob(from, dest, *fromStatBuffer);
}

void CopyQueue::addCopyJob(const std::string& src, const std::string& dest, const struct statx& st)
{
    auto* sourceFd = new QueueFileDescriptor(*this, src, O_RDONLY | O_DIRECT | O_CLOEXEC);
    auto* destFd = new QueueFileDescriptor(*this, dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.stx_mode);

    if (st.stx_size == 0)
    {
        this->addCopyJobPart(sourceFd, destFd, 0, 0, 0, new int32_t(1));
        return;
    }

    // Source file is opened with O_DIRECT flag. This means the buffer we read to has to be aligned.
    // O_DIRECT actually no longer requires us to align to the block size of the filesystem (which is what we're fetching here),
    // but now allows us to use the block size of the device backing the filesystem. Typical values would be 4096 for the
    // fs blocksize, and 512 for the device, so we are over-aligning. It can still be faster to use the higher alignment though,
    // and also there's no easy way to get the backing device's block size. We would need to open() the block device and use
    // an ioctl to fetch it, but first we need to know which device to use. And then we could run into edge cases with crossing
    // filesystem boundaries, so we'd need to account for that. We just use the fs blocksize because it's handily available
    // via stat() on the file, not the block device.
    size_t requiredAlignment = st.stx_blksize;

    size_t chunkSize = this->getBlockSize();

    // The default heap alignment is pretty high, so we probably won't often need to do this adjustment.
    if (requiredAlignment > this->getHeapAlignment())
    {
        size_t bytesRemaining = this->getBlockSize() - (requiredAlignment - 1);
        size_t alignmentBlocks = bytesRemaining / requiredAlignment;
        chunkSize = alignmentBlocks * requiredAlignment;
    }

    release_assert(chunkSize > 0);

    int32_t chunkCount = st.stx_size / chunkSize;
    if (st.stx_size % chunkSize != 0)
        chunkCount++;

    int32_t* chunksDoneTracker = new int32_t(chunkCount);

    int32_t i = 0;
    size_t offset = 0;
    while (offset != st.stx_size)
    {
        size_t count = std::min<size_t>(chunkSize, st.stx_size - offset);

        this->addCopyJobPart(sourceFd, destFd, offset, count, requiredAlignment, chunksDoneTracker);
        offset += count;
        i++;
    }

    debug_assert(chunkCount == i);
}

void CopyQueue::addCopyJobPart(QueueFileDescriptor* sourceFd,
                               QueueFileDescriptor* destFd,
                               off_t offset,
                               off_t size,
                               size_t alignment,
                               int32_t* chunkCount)
{
    debug_assert(this->state == State::Running || this->state == State::Idle);

    this->totalBytesToCopy += size;

    pthread_mutex_lock(&this->copiesPendingStartMutex);
    {
        this->copiesPendingStart.push_back(new CopyRunner(this,
                                                          sourceFd,
                                                          destFd,
                                                          offset,
                                                          size,
                                                          alignment,
                                                          chunkCount));
        this->copiesPendingStartCount++;
    }
    pthread_mutex_unlock(&this->copiesPendingStartMutex);
}


