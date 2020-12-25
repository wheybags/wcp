#include "Util.hpp"
#include "Assert.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <unistd.h>

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

OpenResult myOpen(const std::string& path, int oflag, mode_t mode, bool showErrorMessages)
{
    int fd = -1;

    for (int32_t tries = 0; tries < 5; tries++)
    {
        fd = open(path.c_str(), oflag, mode);

        if (fd < 0 && errno == EINTR)
            continue;

        break;
    }

    if (fd < 0)
    {
        if (showErrorMessages)
            return Error("Couldn't open \"" + path + "\": \"" + strerror(errno));
        else
            return Error();
    }

    return fd;
}

Result myClose(int fd, bool showErrorMessages)
{
#   ifndef __linux__
#       error "Need to handle EINTR if this is ever ported. See notes here https://www.man7.org/linux/man-pages/man2/close.2.html"
#   endif

    int err = close(fd);

    if (err < 0)
    {
        if (showErrorMessages)
            return Error(std::string("Failure on closing file: \"") + strerror(errno) + "\"");
        else
            return Error();
    }

    return Success();
}