#include <ftw.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <liburing.h>
#include "Assert.hpp"
#include "CopyRunner.hpp"
#include "CopyQueue.hpp"
#include "Config.hpp"

static void recursive_mkdir(const char *dir) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp),"%s",dir);
    len = strlen(tmp);
    if(tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for(p = tmp + 1; *p; p++)
        if(*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    mkdir(tmp, S_IRWXU);
}

CopyQueue* copyQueue = nullptr;

std::string src;
std::string dest;

void addFile(std::shared_ptr<FileDescriptor> sourceFd, std::shared_ptr<FileDescriptor> destFd, const struct stat64& st)
{
    // Source file is opened with O_DIRECT flag. This means the buffer we read to has to be aligned.
    // O_DIRECT actually no longer requires us to align to the block size of the filesystem (which is what we're fetching here),
    // but now allows us to use the block size of the device backing the filesystem. Typical values would be 4096 for the
    // fs blocksize, and 512 for the device, so we are over-aligning. It can still be faster to use the higher alignment though,
    // and also there's no easy way to get the backing device's block size. We would need to open() the block device and use
    // an ioctl to fetch it, but first we need to know which device to use. And then we could run into edge cases with crossing
    // filesystem boundaries, so we'd need to account for that. We just use the fs blocksize because it's handily available
    // via stat() on the file, not the block device.
    size_t requiredAlignment = st.st_blksize;

    size_t chunkSize = copyQueue->getBlockSize();

    // The default heap alignment is pretty high, so we probably won't often need to do this adjustment.
    if (requiredAlignment > copyQueue->getHeapAlignment())
    {
        size_t start = requiredAlignment - copyQueue->getHeapAlignment();
        size_t bytesRemaining = copyQueue->getBlockSize() - start;
        size_t alignmentBlocks = bytesRemaining / requiredAlignment;
        chunkSize = alignmentBlocks * requiredAlignment;
    }

    release_assert(chunkSize > 0);

    // Zero size files are already handled, because the file has been opened for writing before calling this function
    off_t offset = 0;
    while (offset != st.st_size)
    {
        off_t count = std::min(off_t(chunkSize), st.st_size - offset);
        copyQueue->addCopyJob(sourceFd, destFd, offset, count, requiredAlignment);
        offset += count;
    }
}

static int f(const char* fpath, const struct stat64* sb, int tflag, struct FTW* ftwbuf)
{
    std::string destPath = dest + (fpath + src.length());

    if (tflag == FTW_D)
    {
        recursive_mkdir(destPath.c_str());
    }
    else if (tflag == FTW_F)
    {
        // TODO: even though we've bumped the concurrent open files limit a lot, we should still probably
        // try to make sure we don't exceed it.
        auto sourceFd = std::make_shared<FileDescriptor>(open(fpath, O_RDONLY | O_DIRECT));
        release_assert(sourceFd->fd > 0);
        auto destFd = std::make_shared<FileDescriptor>(open(destPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, sb->st_mode));
        release_assert(destFd->fd > 0);

        addFile(sourceFd, destFd, *sb);
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

        addFile(sourceFd, destFd, st);
    }

    CopyQueue::OnCompletionAction completionAction = Config::NO_CLEANUP ? CopyQueue::OnCompletionAction::ExitProcessNoCleanup :
                                                                          CopyQueue::OnCompletionAction::Return;

    copyQueue->join(completionAction);

    return 0;
}