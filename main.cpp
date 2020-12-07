#include <liburing.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/ioctl.h>
#include <vector>
#include <unistd.h>
#include <string>
#include <cstring>


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

std::string src = "/home/wheybags/cp_test/test_data/512_2";
std::string dest = "/home/wheybags/cp_test/test_dest";

std::vector<uint8_t> buf;

static int f(const char* fpath, const struct stat64* sb, int tflag, struct FTW* ftwbuf)
{
    std::string destPath = dest + (fpath + src.length() + 1);

    if (tflag == FTW_D)
    {
        //printf("mkdir %s\n", destPath.c_str());
        recursive_mkdir(destPath.c_str());
    }

    if (tflag == FTW_F)
    {
        //printf("write file %s\n", destPath.c_str());

        buf.resize(sb->st_size);
        int readFd = open(fpath, O_RDONLY);
        release_assert(readFd > 0);

        {
            off_t offset = 0;

            while (offset != sb->st_size)
            {
                io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                io_uring_prep_read(sqe, readFd, buf.data(), sb->st_size, offset);

                release_assert(io_uring_submit(&ring) == 1);

                io_uring_cqe *cqe = nullptr;
                release_assert(io_uring_wait_cqe(&ring, &cqe) == 0);
                release_assert(cqe->res >= 0);
                offset += cqe->res;
                io_uring_cqe_seen(&ring, cqe);
            }
        }

        {
            int wfd = open(destPath.c_str(), O_WRONLY | O_CREAT, sb->st_mode);
            release_assert(wfd > 0);

            off_t offset = 0;

            while (offset != sb->st_size)
            {
                io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                io_uring_prep_write(sqe, wfd, buf.data(), sb->st_size, offset);
                release_assert(io_uring_submit(&ring) == 1);

                io_uring_cqe *cqe = nullptr;
                release_assert(io_uring_wait_cqe(&ring, &cqe) == 0);

                if (cqe->res < 0)
                    puts(strerror(-cqe->res));

                release_assert(cqe->res >= 0);
                offset += cqe->res;
                io_uring_cqe_seen(&ring, cqe);
            }

            close(wfd);
        }

        close(readFd);
    }

    return 0;
}


int main(int argc, char** argv)
{
    io_uring_queue_init(100, &ring,0);

    nftw64(src.c_str(), f, 100, 0);



//    const char* path = "/home/wheybags/cp_test/test_data/512_2/512/1";
//    int fd = open(path, O_RDONLY);
//    assert(fd != -1);
//    off_t size = getFileSize(fd);
//    std::vector<uint8_t> buf;
//    buf.resize(size);
//
//    {
//        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
//        io_uring_prep_read(sqe, fd, buf.data(), size, 0);
//        assert(io_uring_submit(&ring) == 1);
//
//        io_uring_cqe *cqe = nullptr;
//        assert(io_uring_wait_cqe(&ring, &cqe) == 0);
//        assert(cqe->res == size);
//        io_uring_cqe_seen(&ring, cqe);
//    }
//
//
//    int wfd = open("/tmp/out", O_WRONLY | O_CREAT);
//    assert(wfd != -1);
//    //assert(pwrite(wfd, buf.data(), size, 0) == size);
//
//    {
//        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
//        io_uring_prep_write(sqe, wfd, buf.data(), size, 0);
//        assert(io_uring_submit(&ring) == 1);
//
//        io_uring_cqe *cqe = nullptr;
//        assert(io_uring_wait_cqe(&ring, &cqe) == 0);
//        assert(cqe->res == size);
//        io_uring_cqe_seen(&ring, cqe);
//    }

    io_uring_queue_exit(&ring);

    return 0;
}

//#include <linux/io_uring.h>
//#include <sys/syscall.h>
//#include <sys/mman.h>
//#include <unistd.h>
//#include <cstdio>
//#include <cassert>
//#include <ftw.h>
//#include <fcntl.h>
//#include <sys/ioctl.h>
//#include <cstdlib>
//
//int io_uring_setup(unsigned entries, struct io_uring_params *p)
//{
//    return (int) syscall(__NR_io_uring_setup, entries, p);
//}
//
//int io_uring_enter(int ring_fd, unsigned int to_submit,
//                   unsigned int min_complete, unsigned int flags)
//{
//    return (int) syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
//                         flags, NULL, 0);
//}
//
//struct SubmissionQueueRingBuffer {
//    unsigned *head;
//    unsigned *tail;
//    unsigned *ring_mask;
//    unsigned *ring_entries;
//    unsigned *flags;
//    unsigned *array;
//};
//
//struct CompletionQueueRingBuffer {
//    unsigned *head;
//    unsigned *tail;
//    unsigned *ring_mask;
//    unsigned *ring_entries;
//    struct io_uring_cqe *cqes;
//};
//
///* This is x86 specific */
//#define read_barrier()  __asm__ __volatile__("":::"memory")
//#define write_barrier() __asm__ __volatile__("":::"memory")
//
//static int f(const char* fpath, const struct stat64* sb, int tflag, struct FTW* ftwbuf)
//{
//    puts(fpath);
//
//    return 0;
//}
//
//SubmissionQueueRingBuffer submissionBuffer = {};
//CompletionQueueRingBuffer completionBuffer = {};
//io_uring_sqe* submissionQueueEntries = nullptr;
//int ioUringFileDescriptor;
//
//
//
//int main(int argc, char** argv)
//{
//    //nftw64("/home/wheybags/cp_test/test_data/512_2", f, 100, 0);
//
//    io_uring_params params = {};
//
//    //IORING_OP_READV
//
//    ioUringFileDescriptor = io_uring_setup(100, &params);
//    assert(ioUringFileDescriptor > 0);
//    assert(params.features & IORING_FEAT_SINGLE_MMAP);
//
//    int sring_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
//    int cring_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
//    if (cring_sz > sring_sz)
//        sring_sz = cring_sz;
//    cring_sz = sring_sz;
//
//    char* queuePtr = reinterpret_cast<char*>(mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ioUringFileDescriptor, IORING_OFF_SQ_RING));
//    assert(queuePtr != MAP_FAILED);
//
//    // Ring mask is a power of two, eg 127 (2**7). In this case, ring indexes are 7-bit numbers, so they wrap at 127.
//    // This avoids having to deal with wrapping manually. The & is just a modulo!
//
//
//    submissionBuffer.head = reinterpret_cast<unsigned int*>(queuePtr + params.sq_off.head);
//    submissionBuffer.tail = reinterpret_cast<unsigned int*>(queuePtr + params.sq_off.tail);
//    submissionBuffer.ring_mask = reinterpret_cast<unsigned int*>(queuePtr + params.sq_off.ring_mask);
//    submissionBuffer.ring_entries = reinterpret_cast<unsigned int*>(queuePtr + params.sq_off.ring_entries);
//    submissionBuffer.flags = reinterpret_cast<unsigned int*>(queuePtr + params.sq_off.flags);
//    submissionBuffer.array = reinterpret_cast<unsigned int*>(queuePtr + params.sq_off.array);
//
//    submissionQueueEntries = reinterpret_cast<io_uring_sqe*>(mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
//               PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
//                                                                  ioUringFileDescriptor, IORING_OFF_SQES));
//    assert(submissionQueueEntries != MAP_FAILED);
//
//    completionBuffer.head = reinterpret_cast<unsigned int*>(queuePtr + params.cq_off.head);
//    completionBuffer.tail = reinterpret_cast<unsigned int*>(queuePtr + params.cq_off.tail);
//    completionBuffer.ring_mask = reinterpret_cast<unsigned int*>(queuePtr + params.cq_off.ring_mask);
//    completionBuffer.ring_entries = reinterpret_cast<unsigned int*>(queuePtr + params.cq_off.ring_entries);
//    completionBuffer.cqes = reinterpret_cast<io_uring_cqe*>(queuePtr + params.cq_off.cqes);
//
//
//
//    return 0;
//}
//

//
//struct FileInfo
//{
//    off_t size;
//    struct iovec iovecs[];      /* Referred by readv/writev */
//};
//
//int submit_to_sq(char *file_path, struct submitter *s)
//{
//    static constexpr int BLOCK_SZ = 512;
//    FileInfo* fileInfo;
//
//    int fileFd = open(file_path, O_RDONLY);
//    assert(fileFd > 0);
//
//    off_t fileSize = getFileSize(fileFd);
//    if (fileSize < 0)
//        return 1;
//
//    off_t bytesRemaining = fileSize;
//    int blocks = (int) fileSize / BLOCK_SZ;
//    if (fileSize % BLOCK_SZ)
//        blocks++;
//
//    fileInfo = (FileInfo*) malloc(sizeof(*fileInfo) + sizeof(struct iovec) * blocks);
//    assert(fileInfo);
//
//    fileInfo->size = fileSize;
//
//    unsigned current_block = 0;
//
//    /*
//     * For each block of the file we need to read, we allocate an iovec struct
//     * which is indexed into the iovecs array. This array is passed in as part
//     * of the submission. If you don't understand this, then you need to look
//     * up how the readv() and writev() system calls work.
//     * */
//    while (bytesRemaining)
//    {
//        off_t bytes_to_read = bytesRemaining;
//        if (bytes_to_read > BLOCK_SZ)
//            bytes_to_read = BLOCK_SZ;
//
//        fileInfo->iovecs[current_block].iov_len = bytes_to_read;
//
//        void *buf;
//        assert(!posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ));
//
//        fileInfo->iovecs[current_block].iov_base = buf;
//
//        current_block++;
//        bytesRemaining -= bytes_to_read;
//    }
//
//    /* Add our submission queue entry to the tail of the SQE ring buffer */
//    unsigned next_tail = (*submissionBuffer.tail) + 1;
//    unsigned tail = *submissionBuffer.tail;
//
//    read_barrier();
//
//    unsigned index = tail & *submissionBuffer.ring_mask;
//    struct io_uring_sqe* submissionQueueEntry = &submissionQueueEntries[index];
//    submissionQueueEntry->fd = fileFd;
//    submissionQueueEntry->flags = 0;
//    submissionQueueEntry->opcode = IORING_OP_READV;
//    submissionQueueEntry->addr = (unsigned long) fileInfo->iovecs;
//    submissionQueueEntry->len = blocks;
//    submissionQueueEntry->off = 0;
//    submissionQueueEntry->user_data = (unsigned long long) fileInfo;
//    submissionBuffer.array[index] = index;
//    tail = next_tail;
//
//    /* Update the tail so the kernel can see it. */
//    if (*submissionBuffer.tail != tail)
//    {
//        *submissionBuffer.tail = tail;
//        write_barrier();
//    }
//
//    /*
//     * Tell the kernel we have submitted events with the io_uring_enter() system
//     * call. We also pass in the IOURING_ENTER_GETEVENTS flag which causes the
//     * io_uring_enter() call to wait until min_complete events (the 3rd param)
//     * complete.
//     * */
//    int ret = io_uring_enter(ioUringFileDescriptor, 1, 1,
//                             IORING_ENTER_GETEVENTS);
//    if (ret < 0)
//    {
//        perror("io_uring_enter");
//        return 1;
//    }
//
//    return 0;
//}
