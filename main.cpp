#include <ftw.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include "Assert.hpp"
#include "CopyRunner.hpp"
#include "CopyQueue.hpp"
#include "Config.hpp"
#include "Util.hpp"

CopyQueue* copyQueue = nullptr;

std::string src;
std::string dest;

static int f(const char* fpath, const struct stat64* sb, int tflag, struct FTW* ftwbuf)
{
    std::string destPath = dest + (fpath + src.length());

    if (tflag == FTW_D)
    {
        recursiveMkdir(destPath);
    }
    else if (tflag == FTW_F)
    {
        // TODO: even though we've bumped the concurrent open files limit a lot, we should still probably
        // try to make sure we don't exceed it.
        auto sourceFd = std::make_shared<FileDescriptor>(open(fpath, O_RDONLY | O_DIRECT));
        release_assert(sourceFd->fd > 0);
        auto destFd = std::make_shared<FileDescriptor>(open(destPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, sb->st_mode));
        release_assert(destFd->fd > 0);

        copyQueue->addCopyJob(sourceFd, destFd, *sb);
    }

    return 0;
}

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
    src = argv[1];
    dest = argv[2];

    // Linux has stupidly low default limits for concurrently open files (1024 file descriptors).
    // This is due to a bad api (select) that uses a bitset to represent a set of file descriptors.
    // We just bump it up as far as we're allowed.
    {
        rlimit64 openFilesLimit = {};
        getrlimit64(RLIMIT_NOFILE, &openFilesLimit);
        openFilesLimit.rlim_cur = openFilesLimit.rlim_max;
        setrlimit64(RLIMIT_NOFILE, &openFilesLimit);
    }

    {
        size_t oneGig = 1024 * 1024 * 1024;
        size_t ramQuota = std::max(getPhysicalRamSize() / 10, oneGig);
        size_t blockSize = 256 * 1024 * 1024; // 256M
        size_t ringSize = 100;
        copyQueue = new CopyQueue(ringSize, ramQuota / blockSize, blockSize);
    }

    copyQueue->start();

    struct stat64 st = {};
    stat64(src.c_str(), &st);

    if (S_ISDIR(st.st_mode))
    {
        nftw64(src.c_str(), f, 200, 0);
    }
    else
    {
        auto sourceFd = std::make_shared<FileDescriptor>(open(src.c_str(), O_RDONLY | O_DIRECT));
        release_assert(sourceFd->fd > 0);
        auto destFd = std::make_shared<FileDescriptor>(open(dest.c_str(), O_WRONLY | O_CREAT, 0777));
        release_assert(destFd->fd > 0);

        copyQueue->addCopyJob(sourceFd, destFd, st);
    }

    CopyQueue::OnCompletionAction completionAction = Config::NO_CLEANUP ? CopyQueue::OnCompletionAction::ExitProcessNoCleanup :
                                                                          CopyQueue::OnCompletionAction::Return;

    copyQueue->join(completionAction);

    return 0;
}