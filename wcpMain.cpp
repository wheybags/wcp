#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <filesystem>
#include "Assert.hpp"
#include "CopyQueue.hpp"
#include "Config.hpp"
#include "Util.hpp"

size_t getPhysicalRamSize()
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    release_assert(pages != -1 && pageSize != -1);
    return size_t(pages) * size_t(pageSize);
}

int wcpMain(int argc, char** argv)
{
    release_assert(argc == 3);
    std::filesystem::path src = argv[1];
    std::filesystem::path dest = argv[2];
    release_assert(!src.empty() && !dest.empty());

    // Linux has stupidly low default limits for concurrently open files (1024 file descriptors).
    // This is due to a bad api (select) that uses a bitset to represent a set of file descriptors.
    // We just bump it up as far as we're allowed.
    size_t fileDescriptorCap = 512;
    {
        rlimit64 openFilesLimit = {};
        getrlimit64(RLIMIT_NOFILE, &openFilesLimit);
        openFilesLimit.rlim_cur = openFilesLimit.rlim_max;

        // Try to leave a reasonable amount for other purposes (it will be a lot anyway, ~1mil on my machine)
        if (setrlimit64(RLIMIT_NOFILE, &openFilesLimit) == 0 && openFilesLimit.rlim_cur > 2048)
            fileDescriptorCap = openFilesLimit.rlim_cur - 1024;
    }

    size_t oneGig = 1024 * 1024 * 1024;
    size_t ramQuota = std::max(getPhysicalRamSize() / 10, oneGig);
    size_t blockSize = 256 * 1024 * 1024; // 256M
    size_t ringSize = ramQuota / blockSize;
    CopyQueue copyQueue(ringSize, fileDescriptorCap, Heap(ramQuota / blockSize, blockSize));
    copyQueue.start();

    struct statx srcStat = {};
    {
        Result result = myStatx(AT_FDCWD, src, 0, STATX_BASIC_STATS, srcStat);
        if (std::holds_alternative<Error>(result))
        {
            fprintf(stderr, "%s\n", std::get<Error>(result).humanFriendlyErrorMessage->c_str());
            return 1;
        }
    }

    struct statx destStat = {};
    int destStatResult = 0;
    {
        destStatResult = retrySyscall([&]()
        {
            statx(AT_FDCWD, dest.c_str(), 0, STATX_BASIC_STATS, &destStat);
        });

        if (destStatResult != 0 && destStatResult != ENOENT)
        {
            fprintf(stderr, "Failed to stat \"%s\": \"%s\"\n", dest.c_str(), strerror(destStatResult));
            return 1;
        }
    }

    {
        std::filesystem::path destRealParent = std::filesystem::absolute(dest);
        if (!destRealParent.has_filename()) // This is needed to account for if dest has a trailing slash
            destRealParent = destRealParent.parent_path();
        destRealParent = destRealParent.parent_path();

        release_assert(access(destRealParent.c_str(), F_OK) == 0);
    }

    if (S_ISDIR(srcStat.stx_mode))
    {
        dest /= src.filename();
        recursiveMkdir(dest.string());

        if (destStatResult == 0)
            release_assert(S_ISDIR(destStat.stx_mode));

        copyQueue.addRecursiveCopy(src, dest);
    }
    else if (S_ISREG(srcStat.stx_mode))
    {
        if (destStatResult == 0 && S_ISDIR(destStat.stx_mode))
            dest /= src.filename();

        copyQueue.addFileCopy(src, dest, &srcStat);
    }
    else
    {
        release_assert(false);
    }

    CopyQueue::OnCompletionAction completionAction = Config::NO_CLEANUP ? CopyQueue::OnCompletionAction::ExitProcessNoCleanup :
                                                     CopyQueue::OnCompletionAction::Return;

    bool success = copyQueue.join(completionAction);

    return int(!success);
}