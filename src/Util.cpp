#include "Util.hpp"
#include "Assert.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <unistd.h>
#include <dirent.h>

void recursiveMkdir(std::string& path)
{
    auto mkdirOne = [](const char* onePath)
    {
        errno = 0;
        mkdir(onePath, S_IRWXU);
        release_assert(errno == 0 || errno == EEXIST);

        if (errno == EEXIST)
        {
            struct stat64 st = {};
            release_assert(stat64(onePath, &st) == 0);
            release_assert(S_ISDIR(st.st_mode));
        }
    };

    for (size_t i = 1; i < path.size(); i++)
    {
        if (path[i] == '/')
        {
            path[i] = '\0';
            mkdirOne(path.data());
            path[i] = '/';
        }
    }

    mkdirOne(path.c_str());
}

OpenResult myOpen(const std::string& path, int oflag, mode_t mode)
{
    using namespace std::string_literals;

    int fd = -1;

    for (int32_t tries = 0; tries < 5; tries++)
    {
        fd = open(path.c_str(), oflag, mode);

        // Some filesystems don't support O_DIRECT, so we retry without if it fails
        if (fd < 0 && (oflag & O_DIRECT) && errno == EINVAL)
        {
            oflag ^= O_DIRECT;
            continue;
        }

        if (fd < 0 && (errno == EINTR || errno == EAGAIN))
            continue;

        break;
    }

    if (fd < 0)
        return Error("Couldn't open \""s + path + "\": \""s + strerror(errno) + "\""s);

    return fd;
}

Result myClose(int fd)
{
    using namespace std::string_literals;

#   ifndef __linux__
#       error "Need to handle EINTR if this is ever ported. See notes here https://www.man7.org/linux/man-pages/man2/close.2.html"
#   endif

    int err = close(fd);

    if (err < 0)
        return Error("Failure on closing file: \""s + strerror(errno) + "\""s);

    return Success();
}

Result myStatx(int fd, const std::string& path, int flags, unsigned int mask, struct statx& buf)
{
    int err = retrySyscall([&]()
    {
        statx(fd, path.c_str(), flags, mask, &buf);
    });

    if (err != 0)
        return Error("Failed to stat \"" + path + "\": \"" + strerror(err) + "\"");

    return Success();
}

GetDentsResult myGetDents(int dfd, const std::string& path, void* buffer, size_t bufferSize)
{
    ssize_t retval = 0;
    int err = retrySyscall([&]()
    {
        retval = getdents64(dfd, buffer, bufferSize);
    });

    if (err != 0)
        return Error("Couldn't open directory \"" + path + "\": \"" + strerror(err) + "\"");

    return size_t(retval);
}