#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cstring>
#include <string>
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
    char* src = argv[1];
    char* dest = argv[2];

    size_t oneGig = 1024 * 1024 * 1024;
    size_t ramQuota = std::max(getPhysicalRamSize() / 10, oneGig);
    size_t blockSize = 256 * 1024 * 1024; // 256M
    size_t ringSize = 100;
    CopyQueue copyQueue(ringSize, ramQuota / blockSize, blockSize);

    copyQueue.start();

    struct stat64 st = {};
    stat64(src, &st);

    if (S_ISDIR(st.st_mode))
    {
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
    else
    {
        auto sourceFd = std::make_shared<FileDescriptor>(open(src, O_RDONLY | O_DIRECT));
        release_assert(sourceFd->fd > 0);
        auto destFd = std::make_shared<FileDescriptor>(open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0777));
        release_assert(destFd->fd > 0);

        copyQueue.addCopyJob(sourceFd, destFd, st);
    }

    CopyQueue::OnCompletionAction completionAction = Config::NO_CLEANUP ? CopyQueue::OnCompletionAction::ExitProcessNoCleanup :
                                                                          CopyQueue::OnCompletionAction::Return;

    copyQueue.join(completionAction);

    return 0;
}