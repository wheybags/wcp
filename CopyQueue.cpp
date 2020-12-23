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

CopyQueue::CopyQueue(size_t ringSize, size_t fileDescriptorCap, Heap&& heap)
    : ringSize(ringSize)
    , fileDescriptorCap(fileDescriptorCap)
    , copyBufferHeap(std::move(heap))
{
    release_assert(this->fileDescriptorCap >= CopyQueue::minimumFileDescriptorCap());

    io_uring_params params = {};
    release_assert(io_uring_queue_init_params(this->ringSize, &this->ring, &params) == 0);
    this->completionRingSize = params.cq_entries;

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

void CopyQueue::exitProcess()
{
    // When being used as a single queue for one copy operation, we can just exit immediately when we're done.
    // No need to carefully clean up all our threads and memory allocations, the kernel will clean up for us.
    // Using _exit disables any registered atexit handlers from running.
    _exit(0); // TODO: some sensible error code
}

void CopyQueue::submitLoop()
{
    pthread_setname_np(pthread_self(), "Submit thread");

    uint8_t* nextBuffer = nullptr;

    // TODO: avoid busy looping? Or maybe it's fine? Not really sure tbh.
    while (true)
    {
        while(this->copiesPendingStartCount > 0 &&
              // Rate limit to avoid overflowing the completion queue
              (this->completionRingSize - this->submissionsRunning) >= CopyRunner::MAX_JOBS_PER_RUNNER)
        {
            if (!nextBuffer)
                nextBuffer = this->copyBufferHeap.getBlock();

            CopyRunner* toAdd = nullptr;
            pthread_mutex_lock(&this->copiesPendingStartMutex);
            {
                debug_assert(!this->copiesPendingContinue.empty() || !this->copiesPendingStart.empty());

                if (!this->copiesPendingContinue.empty())
                {
                    toAdd = this->copiesPendingContinue.front();
                    debug_assert(!toAdd->needsBuffer());

                    this->copiesPendingContinue.pop_front();
                }
                else if (nextBuffer)
                {
                    CopyRunner* temp = this->copiesPendingStart.front();
                    debug_assert(temp->needsBuffer());

                    temp->giveBuffer(nextBuffer);
                    nextBuffer = nullptr;
                    toAdd = temp;

                    this->copiesPendingStart.pop_front();
                }
            }
            pthread_mutex_unlock(&this->copiesPendingStartMutex);

            if (toAdd)
            {
                toAdd->addToBatch();
                this->copiesPendingStartCount--;
            }
        }

        if (this->isDone())
        {
            if (Config::DEBUG_COPY_OPS)
                puts("SUBMIT THREAD EXIT");

            if (nextBuffer)
                this->copyBufferHeap.returnBlock(nextBuffer);

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
            if (eventData->resultOverride != std::numeric_limits<__s32>::max())
            {
                // Don't allow spoofing to drop a real error
                if (cqe->res >= 0)
                {
                    // only allow spoofing an error, or a shorter-than-real io event, never a longer-than-real one.
                    if (eventData->resultOverride < 0 || eventData->resultOverride <= cqe->res)
                        result = eventData->resultOverride;
                }

                eventData->resultOverride = std::numeric_limits<__s32>::max();
            }

            if (eventData->copyData->onCompletionEvent(eventData->type, result))
            {
                delete eventData->copyData;
                this->keepAliveCount--;
            }

            io_uring_cqe_seen(&ring, cqe);
            this->submissionsRunning--;
        }

        if (this->isDone())
        {
            if (Config::DEBUG_COPY_OPS)
                puts("COMPLETION THREAD EXIT");

            if (completionAction == OnCompletionAction::ExitProcessNoCleanup)
                this->exitProcess();
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

    bool firstShow = true;
    auto showProgress = [&]()
    {
        winsize winsize = {};
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsize);

        if (!firstShow)
        {
            // return to start of line, and move three lines up (ie, move cursor to the top left of our draw area)
            fputs("\r\033[2A", stderr);
        }

        auto showLine = [&](const std::string& line)
        {
            fputs((rightPad(line, winsize.ws_col) + "\n").c_str(), stderr);
        };

        bool haveTotal = int(this->state.load()) >= int(State::AdditionComplete);

        // show raw amount copied
        {
            int32_t sectionLength = 10;

            std::string status = leftPad(humanFriendlyFileSize(this->totalBytesCopied), sectionLength) + " / ";
            if (haveTotal)
                status += leftPad(humanFriendlyFileSize(this->totalBytesToCopy), sectionLength);
            else
                status += leftPad("???", sectionLength);

            // Centre align
            std::string statusLine;
            for (uint32_t i = 0; i < (winsize.ws_col / 2) - (status.length() / 2); i++)
                statusLine += " ";
            statusLine += status;

            showLine(statusLine);
        }

        // progress bar
        {
            float ratio = (haveTotal && this->totalBytesToCopy > 0) ? float(this->totalBytesCopied) / float(this->totalBytesToCopy) : 0;
            int percentDone = int(ratio * 100.0f);

            int32_t width = winsize.ws_col - 6;

            std::string percentString = haveTotal ? std::to_string(percentDone) : "???";
            std::string progressBarLine = leftPad(percentString, 3) + "% ";

            int doneChars = int(ratio * width);
            for (int32_t i = 0; i < width; i++)
                progressBarLine += i < doneChars ? "█" : "▒";

            showLine(progressBarLine);
        }

        firstShow = false;
    };

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
}

void CopyQueue::start()
{
    release_assert(this->state == State::Idle);
    this->state = State::Running;
    this->totalBytesToCopy = 0;
    this->totalBytesCopied = 0;

    release_assert(pthread_create(&this->submitThread, nullptr, CopyQueue::staticCallSubmitLoop, this) == 0);
    release_assert(pthread_create(&this->completionThread, nullptr, CopyQueue::staticCallCompletionLoop, this) == 0);

    // Don't try to show a progress bar if we're not outputting to a terminal
    this->showingProgress = isatty(STDERR_FILENO) && isatty(STDOUT_FILENO) && getenv("TERM");
    if (this->showingProgress)
    {
        pthread_mutex_lock(&this->progressEndMutex);
        release_assert(pthread_create(&this->showProgressThread, nullptr, CopyQueue::staticCallShowProgressLoop, this) == 0);
    }
}

void CopyQueue::join(OnCompletionAction onCompletionAction)
{
    debug_assert(this->state == State::Running);
    this->state = State::AdditionComplete;
    this->completionAction = onCompletionAction;

    pthread_join(this->completionThread, nullptr);
    // completionThread will probably terminate the process for us before we get here.
    // Check here too just in case, as there is a race on setting this->completionAction.
    if (this->completionAction == OnCompletionAction::ExitProcessNoCleanup)
        this->exitProcess();

    debug_assert(this->submissionsRunning == 0);
    debug_assert(this->copyBufferHeap.getFreeBlocksCount() == this->copyBufferHeap.getBlockCount());

    pthread_join(this->submitThread, nullptr);

    if (this->showingProgress)
    {
        pthread_mutex_unlock(&this->progressEndMutex); // signals the progress thread to stop sleeping, if it is ATM
        pthread_join(this->showProgressThread, nullptr);
    }
    this->state = State::Idle;
}

void CopyQueue::addRecursiveCopy(std::string from, std::string dest)
{
    debug_assert(!from.empty() && !dest.empty());
    if (from.back() != '/')
        from += '/';
    if (dest.back() != '/')
        dest += '/';

    // Taken from the manpage for getdents64() https://man7.org/linux/man-pages/man2/getdents64.2.html
    struct linux_dirent64
    {
        ino64_t             d_ino;    /* 64-bit inode number */
        off64_t             d_off;    /* 64-bit offset to next structure */
        unsigned short      d_reclen; /* Size of this dirent */
        unsigned char       d_type;   /* File type */
        __extension__ char  d_name[]; /* Filename (null-terminated). __extension__ allows use of flexible array members in g++ (normally only allowed in plain C) */
    };

    recursiveMkdir(dest);

    std::vector<std::string> directoryStack;
    directoryStack.emplace_back(from);

    std::vector<uint8_t> dirBuffer;
    dirBuffer.resize(1024 * 1024 * 32); // 32mb, should be enough for a one-shot almost always

    while (!directoryStack.empty())
    {
        std::string current = std::move(directoryStack.back());
        directoryStack.pop_back();

        // TODO: RAII this
        int currentFd = open(current.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        release_assert(currentFd > 0);

        ssize_t ret = 0;
        do
        {
            ret = getdents64(currentFd, dirBuffer.data(), dirBuffer.size());
            release_assert(ret >= 0);

            uint8_t* nextPtr = dirBuffer.data();
            while (nextPtr < dirBuffer.data() + ret)
            {
                linux_dirent64* currentEntry = reinterpret_cast<linux_dirent64*>(nextPtr);
                nextPtr += currentEntry->d_reclen;

                std::string fullPath = current + currentEntry->d_name;

                unsigned char type = currentEntry->d_type;

                struct stat64 sb = {};
                bool didStat = false;

                if (currentEntry->d_type == DT_UNKNOWN)
                {
                    release_assert(stat64(fullPath.c_str(), &sb) == 0);
                    didStat = true;

                    if (S_ISFIFO(sb.st_mode))
                        type = DT_FIFO;
                    else if (S_ISCHR(sb.st_mode))
                        type = DT_CHR;
                    else if (S_ISDIR(sb.st_mode))
                        type = DT_DIR;
                    else if (S_ISREG(sb.st_mode))
                        type = DT_REG;
                    else if (S_ISLNK(sb.st_mode))
                        type = DT_LNK;
                    else if (S_ISSOCK(sb.st_mode))
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
                        release_assert(stat64(fullPath.c_str(), &sb) == 0);
                        didStat = true;
                    }

                    auto sourceFd = std::make_shared<QueueFileDescriptor>(*this, fullPath, O_RDONLY | O_DIRECT | O_CLOEXEC);
                    auto destFd = std::make_shared<QueueFileDescriptor>(*this, destPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, sb.st_mode);

                    this->addCopyJob(sourceFd, destFd, sb);
                }
                else
                {
                    release_assert(false); // not handled yet
                }
            }

        } while (ret != 0);

        release_assert(close(currentFd) == 0);
    }
}

void CopyQueue::addFileCopy(const std::string& from, const std::string& dest, const struct stat64* fromStatBuffer)
{
    std::unique_ptr<struct stat64> tmp;
    if (!fromStatBuffer)
    {
        tmp = std::make_unique<struct stat64>();
        release_assert(stat64(from.c_str(), tmp.get()) == 0);
        fromStatBuffer = tmp.get();
    }

    auto sourceFd = std::make_shared<QueueFileDescriptor>(*this, from, O_RDONLY | O_DIRECT | O_CLOEXEC);
    auto destFd = std::make_shared<QueueFileDescriptor>(*this, dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, fromStatBuffer->st_mode);

    this->addCopyJob(sourceFd, destFd, *fromStatBuffer);
}

void CopyQueue::addCopyJob(std::shared_ptr<QueueFileDescriptor> sourceFd, std::shared_ptr<QueueFileDescriptor> destFd, const struct stat64& st)
{
    if (st.st_size == 0)
    {
        // This makes sure the file gets created. Otherwise, it could be deferred to the CopyRunner, but the deferred close would
        // never happen since there's no bytes to copy, so no CopyRunner gets created.
        destFd->ensureOpened();
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
    size_t requiredAlignment = st.st_blksize;

    size_t chunkSize = this->getBlockSize();

    // The default heap alignment is pretty high, so we probably won't often need to do this adjustment.
    if (requiredAlignment > this->getHeapAlignment())
    {
        size_t start = requiredAlignment - this->getHeapAlignment();
        size_t bytesRemaining = this->getBlockSize() - start;
        size_t alignmentBlocks = bytesRemaining / requiredAlignment;
        chunkSize = alignmentBlocks * requiredAlignment;
    }

    release_assert(chunkSize > 0);

    off_t offset = 0;
    while (offset != st.st_size)
    {
        off_t count = std::min(off_t(chunkSize), st.st_size - offset);
        this->addCopyJobPart(sourceFd, destFd, offset, count, requiredAlignment);
        offset += count;
    }
}

void CopyQueue::addCopyJobPart(std::shared_ptr<QueueFileDescriptor> sourceFd,
                               std::shared_ptr<QueueFileDescriptor> destFd,
                               off_t offset, off_t size, size_t alignment)
{
    debug_assert(this->state == State::Running);

    this->totalBytesToCopy += size;

    pthread_mutex_lock(&this->copiesPendingStartMutex);
    {
        this->keepAliveCount++;
        this->copiesPendingStartCount++;
        this->copiesPendingStart.push_back(new CopyRunner(this, std::move(sourceFd), std::move(destFd), offset, size, alignment));
    }
    pthread_mutex_unlock(&this->copiesPendingStartMutex);
}

void CopyQueue::continueCopyJob(CopyRunner* runner)
{
    pthread_mutex_lock(&this->copiesPendingStartMutex);
    {
        this->copiesPendingStartCount++;
        this->copiesPendingContinue.push_back(runner);
    }
    pthread_mutex_unlock(&this->copiesPendingStartMutex);
}


