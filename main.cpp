#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <filesystem>
#include "Assert.hpp"
#include "CopyQueue.hpp"
#include "Config.hpp"

size_t getPhysicalRamSize()
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    release_assert(pages != -1 && pageSize != -1);
    return size_t(pages) * size_t(pageSize);
}

int main(int argc, char** argv)
{
    release_assert(argc == 3);
    std::filesystem::path src = argv[1];
    std::filesystem::path dest = argv[2];
    release_assert(!src.empty() && !dest.empty());

    size_t oneGig = 1024 * 1024 * 1024;
    size_t ramQuota = std::max(getPhysicalRamSize() / 10, oneGig);
    size_t blockSize = 256 * 1024 * 1024; // 256M
    size_t ringSize = 100;
    CopyQueue copyQueue(ringSize, ramQuota / blockSize, blockSize);
    copyQueue.start();

    struct stat64 srcStat = {};
    release_assert(stat64(src.c_str(), &srcStat) == 0);

    struct stat64 destStat = {};
    errno = 0;
    stat64(src.c_str(), &destStat);
    int destStatResult = errno;
    release_assert(destStatResult == 0 || destStatResult == ENOENT);

    // TODO: deal with dest being in a nonexistent dir

    if (S_ISDIR(srcStat.st_mode))
    {
        dest /= src.filename();

        if (destStatResult == 0)
            release_assert(S_ISDIR(destStat.st_mode));

        // Linux has stupidly low default limits for concurrently open files (1024 file descriptors).
        // This is due to a bad api (select) that uses a bitset to represent a set of file descriptors.
        // We just bump it up as far as we're allowed.
        {
            rlimit64 openFilesLimit = {};
            getrlimit64(RLIMIT_NOFILE, &openFilesLimit);
            openFilesLimit.rlim_cur = openFilesLimit.rlim_max;
            setrlimit64(RLIMIT_NOFILE, &openFilesLimit);
        }

        copyQueue.addRecursiveCopy(src, dest);
    }
    else if (S_ISREG(srcStat.st_mode))
    {
        if (destStatResult == 0 && S_ISDIR(destStat.st_mode))
            dest /= src.filename();

        auto sourceFd = std::make_shared<FileDescriptor>(open(src.c_str(), O_RDONLY | O_DIRECT));
        release_assert(sourceFd->fd > 0);
        auto destFd = std::make_shared<FileDescriptor>(open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777));
        release_assert(destFd->fd > 0);

        copyQueue.addCopyJob(sourceFd, destFd, srcStat);
    }
    else
    {
        release_assert(false);
    }

    CopyQueue::OnCompletionAction completionAction = Config::NO_CLEANUP ? CopyQueue::OnCompletionAction::ExitProcessNoCleanup :
                                                                          CopyQueue::OnCompletionAction::Return;

    copyQueue.join(completionAction);

    return 0;
}