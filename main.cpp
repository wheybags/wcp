#include <liburing.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/ioctl.h>
#include <vector>
#include <unistd.h>
#include <string>
#include <cstring>

#define DEBUG_COPY_OPS 0
#define DEBUG_FORCE_PARTIAL_READS 0
#define DEBUG_FORCE_PARTIAL_WRITES 0


#ifdef _MSC_VER
#define DEBUG_BREAK __debugbreak()
#else
#include <signal.h>
#define DEBUG_BREAK raise(SIGTRAP);
#endif

#define message_and_abort_fmt(message, ...)                                                                                                                    \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        fprintf(stderr, message, __VA_ARGS__);                                                                                                                 \
        DEBUG_BREAK;                                                                                                                                           \
        abort();                                                                                                                                               \
    } while (0)

#define message_and_abort(message) message_and_abort_fmt("%s\n", message)

#define release_assert(cond)                                                                                                                                   \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        if (!(cond))                                                                                                                                           \
            message_and_abort_fmt("ASSERTION FAILED: (%s) in %s:%d\n", #cond, __FILE__, __LINE__);                                                             \
    } while (0)

#ifdef NDEBUG
#define debug_assert(cond)                                                                                                                                     \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        (void)sizeof(cond);                                                                                                                                    \
    } while (0)
#else
#define debug_assert(cond) release_assert(cond)
#endif

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

struct CopyData
{
    static int32_t copiesRunning;
    static int32_t copiesPendingsubmit;

    int sourceFd;
    int destFd;
    off_t size;
    uint8_t* buffer;

    off_t readOffset = 0;
    off_t writeOffset = 0;

    struct EventData
    {
        enum class Type
        {
            Read,
            Write
        };

        CopyData* copyData;
        __s32 resultOverride;
        Type type;
    };

    int jobsRunning = 0;

    EventData readData {this, 0, EventData::Type::Read};
    EventData writeData {this, 0, EventData::Type::Write};

    CopyData(int sourceFd, int destFd, off_t size)
        : sourceFd(sourceFd)
        , destFd(destFd)
        , size(size)
        , buffer(new uint8_t[size])
    {
        copiesRunning++;
        copiesPendingsubmit++;
    }

    ~CopyData()
    {
        release_assert(jobsRunning == 0);

        delete[] buffer;
        close(sourceFd);
        close(destFd);

        copiesRunning--;
    }

    void addToBatch()
    {
#if DEBUG_COPY_OPS
        printf("START %d->%d\n", this->sourceFd, this->destFd);
#endif

        debug_assert(this->jobsRunning == 0);
        debug_assert(this->writeOffset <= this->size);

        io_uring_sqe* sqe = nullptr;

        unsigned int bytesToRead = this->size - this->readOffset;
#if DEBUG_FORCE_PARTIAL_READS
        bytesToRead = rand() % (bytesToRead + 1);
        bool readIsPartial = bytesToRead < this->size - this->readOffset;
#endif

        if (bytesToRead)
        {
            sqe = io_uring_get_sqe(&ring);
            this->jobsRunning++;

            io_uring_prep_read(sqe, this->sourceFd, this->buffer + this->readOffset, bytesToRead, this->readOffset);
            sqe->flags |= IOSQE_IO_LINK;

            static_assert(sizeof(sqe->user_data) == sizeof(void*), "");
            sqe->user_data = reinterpret_cast<__u64>(&this->readData);
        }

        if (this->writeOffset < this->size || this->size == 0)
        {
            sqe = io_uring_get_sqe(&ring);
            this->jobsRunning++;

            auto prepWrite = [&]()
            {
                unsigned int bytesToWrite = this->size - this->writeOffset;
#if DEBUG_FORCE_PARTIAL_WRITES
                bytesToWrite = rand() % (bytesToWrite + 1);
#endif

                io_uring_prep_write(sqe,
                                    this->destFd,
                                    this->buffer + this->writeOffset,
                                    bytesToWrite,
                                    this->writeOffset);
            };

#if DEBUG_FORCE_PARTIAL_READS
            if (readIsPartial)
            {
                // Normally if a read is short, the following write would be cancelled.
                // To emulate this, we just send a nop event and override it's result code to -ECANCELLED.
                io_uring_prep_nop(sqe);
                this->writeData.resultOverride = -ECANCELED;
            }
            else
            {
                prepWrite();
            }
#else
            prepWrite();
#endif
            sqe->user_data = reinterpret_cast<__u64>(&this->writeData);
        }

        //if (copiesPendingsubmit == 10)
        {
            io_uring_submit(&ring);
            copiesPendingsubmit = 0;
        }
    }

    bool onCompletionEvent(EventData::Type type, __s32 result)
    {
        this->jobsRunning--;
        debug_assert(jobsRunning >= 0);

        if (type == EventData::Type::Read)
        {
#if DEBUG_COPY_OPS
            printf("RD %d->%d JR:%d RES: %d\n", this->sourceFd, this->destFd, this->jobsRunning, result);
#endif

            if (result < 0)
                puts(strerror(-result));
            release_assert(result > 0);
            this->readOffset += result;
        }
        else
        {
#if DEBUG_COPY_OPS
            printf("WT %d->%d JR:%d RES: %d\n", this->sourceFd, this->destFd, this->jobsRunning, result);
#endif

            release_assert(result >= 0 || result == -ECANCELED);
            if (result > 0)
                this->writeOffset += result;
        }

        if (this->jobsRunning == 0 && this->writeOffset < this->size)
            this->addToBatch();

        return this->jobsRunning == 0 && this->writeOffset == this->size;
    }
};

int CopyData::copiesRunning = 0;
int CopyData::copiesPendingsubmit = 0;

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
        CopyData* copy = new CopyData(sourceFd, destFd, sb->st_size);
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

    if (CopyData::copiesPendingsubmit)
        io_uring_submit(&ring);

    while (CopyData::copiesRunning)
    {
        io_uring_cqe *cqe = nullptr;
        release_assert(io_uring_wait_cqe_nr(&ring, &cqe, 1) == 0);

        auto* eventData = reinterpret_cast<CopyData::EventData*>(cqe->user_data);
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