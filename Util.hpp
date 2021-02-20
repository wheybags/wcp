#pragma once
#include <string>
#include <memory>
#include <variant>
#include <functional>
#include <atomic>

struct statx;

void recursiveMkdir(std::string& path);
[[maybe_unused]] static void recursiveMkdir(const std::string& path) { std::string copy(path); recursiveMkdir(copy); }
[[maybe_unused]] static void recursiveMkdir(std::string&& path) { recursiveMkdir(path); }

class Error
{
public:
    std::unique_ptr<std::string> humanFriendlyErrorMessage;

    Error() = delete;
    explicit Error(std::string&& message): humanFriendlyErrorMessage(std::make_unique<std::string>(std::move(message))) {}
};

using Result = std::variant<Error, nullptr_t>;
static constexpr nullptr_t Success() { return nullptr; }

using OpenResult = std::variant<Error, int>;
OpenResult myOpen(const std::string& path, int oflag, mode_t mode);
Result myClose(int fd);

[[maybe_unused]] static int retrySyscall(const std::function<void(void)>& func)
{
    errno = 0;
    for (int32_t tries = 0; tries < 5; tries++)
    {
        func();

        if (errno == EINTR || errno == EAGAIN)
            continue;

        break;
    }

    return errno;
}

// Taken from the manpage for getdents64() https://man7.org/linux/man-pages/man2/getdents64.2.html
struct linux_dirent64
{
    ino64_t             d_ino;    /* 64-bit inode number */
    off64_t             d_off;    /* 64-bit offset to next structure */
    unsigned short      d_reclen; /* Size of this dirent */
    unsigned char       d_type;   /* File type */
    __extension__ char  d_name[]; /* Filename (null-terminated). __extension__ allows use of flexible array members in g++ (normally only allowed in plain C) */
};

using GetDentsResult = std::variant<Error, size_t>;
[[nodiscard]] GetDentsResult myGetDents(int dfd, const std::string& path, void* buffer, size_t bufferSize);

[[nodiscard]] Result myStatx(int fd, const std::string& path, int flags, unsigned int mask, struct statx& buf);

struct RollingBitset
{
    std::atomic_uint64_t bigSmallBuffer = 0;

    struct ReadProxy
    {
        uint64_t val;
        explicit ReadProxy(uint64_t val) : val(val) {}

        uint64_t getCount() const { return (val & 0xFF00000000000000ULL) >> (8*7); }
        uint64_t getSet() const { return val & 0xFF;}//0x00FFFFFFFFFFFFFFULL; }
        int32_t getBitsOnCount() const
        {
            int32_t count = 0;
            uint64_t tmp = getSet();
            while (tmp)
            {
                count += tmp & 1;
                tmp >>= 1;
            }

            return count;
        }
    };

    ReadProxy read() const { return ReadProxy(bigSmallBuffer.load()); }

    void addToBuff(bool big)
    {
        while (true)
        {
            uint64_t oldVal = bigSmallBuffer.load();

            uint64_t countPart = ((oldVal & 0xFF00000000000000ULL) >> (8 * 7)) + 1;
            if (countPart > 8 * 7)
                countPart = 8 * 7;
            countPart <<= 8 * 7;

            uint64_t newVal = (oldVal << 1) | uint64_t(big);
            newVal = newVal & 0x00FFFFFFFFFFFFFFULL;
            newVal |= countPart;

            if (bigSmallBuffer.compare_exchange_strong(oldVal, newVal))
                break;
        }
    }
};