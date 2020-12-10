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

off_t getFileSize(int fd)
{
    struct stat st;

    release_assert(fstat(fd, &st) >= 0);

    if (S_ISBLK(st.st_mode))
    {
        unsigned long long bytes;
        release_assert(ioctl(fd, BLKGETSIZE64, &bytes) == 0);

        return bytes;
    }
    else if (S_ISREG(st.st_mode))
        return st.st_size;

    release_assert(false);
    return 0;
}

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

CopyQueue copyQueue;
std::string src;
std::string dest;

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
        int sourceFd = open(fpath, O_RDONLY);
        int destFd = open(destPath.c_str(), O_WRONLY | O_CREAT, sb->st_mode);
        release_assert(sourceFd > 0);
        release_assert(destFd > 0);
        copyQueue.addCopyJob(sourceFd, destFd, sb->st_size);
    }

    return 0;
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

    copyQueue.start();

    struct stat64 st = {};
    stat64(src.c_str(), &st);

    if (S_ISDIR(st.st_mode))
    {
        nftw64(src.c_str(), f, 200, 0);
    }
    else
    {
        int sourceFd = open(src.c_str(), O_RDONLY);
        int destFd = open(dest.c_str(), O_WRONLY | O_CREAT, 0777);
        copyQueue.addCopyJob(sourceFd, destFd, getFileSize(sourceFd));
    }

    copyQueue.join();

    return 0;
}