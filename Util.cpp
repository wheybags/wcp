#include "Util.hpp"
#include "Assert.hpp"
#include <fcntl.h>
#include <sys/stat.h>

void recursiveMkdir(std::string path)
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