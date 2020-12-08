#include <ftw.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <liburing.h>
#include "Assert.hpp"
#include "CopyRunner.hpp"

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


io_uring ring = {};

std::string src = "/home/wheybags/wcp/test_data/512_20";
std::string dest = "/home/wheybags/wcp/test_dest";

static int f(const char* fpath, const struct stat64* sb, int tflag, struct FTW* ftwbuf)
{
    std::string destPath = dest + (fpath + src.length());

    if (tflag == FTW_D)
    {
        recursive_mkdir(destPath.c_str());
    }
    else if (tflag == FTW_F)
    {
        int sourceFd = open(fpath, O_RDONLY);
        int destFd = open(destPath.c_str(), O_WRONLY | O_CREAT, sb->st_mode);
        CopyRunner* copy = new CopyRunner(&ring, sourceFd, destFd, sb->st_size);
        copy->addToBatch();
    }

    return 0;
}


int main(int argc, char** argv)
{
//    release_assert(argc == 3);
//
//    src = argv[1];
//    dest = argv[2];

    release_assert(io_uring_queue_init(100, &ring, 0) == 0);

    nftw64(src.c_str(), f, 100, 0);


//    int sourceFd = open("/home/wheybags/wcp/test_data/512_2/512/1", O_RDONLY);
//    int destFd = open("/tmp/out", O_WRONLY | O_CREAT, 0777);
//
//    auto copy = new CopyData(sourceFd, destFd, getFileSize(sourceFd));
//    copy->addToBatch();

    if (CopyRunner::copiesPendingsubmit)
        io_uring_submit(&ring);

    while (CopyRunner::copiesRunning)
    {
        io_uring_cqe *cqe = nullptr;
        release_assert(io_uring_wait_cqe_nr(&ring, &cqe, 1) == 0);

        auto* eventData = reinterpret_cast<CopyRunner::EventData*>(cqe->user_data);
        __s32 result = cqe->res;
        if (eventData->resultOverride)
            result = eventData->resultOverride;
        eventData->resultOverride = 0;

        if (eventData->copyData->onCompletionEvent(eventData->type, result))
            delete eventData->copyData;

        io_uring_cqe_seen(&ring, cqe);
    }

    io_uring_queue_exit(&ring);

    return 0;
}