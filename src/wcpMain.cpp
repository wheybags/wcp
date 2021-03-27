#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <filesystem>
#include "Assert.hpp"
#include "CopyQueue.hpp"
#include "Config.hpp"
#include "Util.hpp"

size_t getPhysicalRamSize()
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    release_assert(pages != -1 && pageSize != -1);
    return size_t(pages) * size_t(pageSize);
}

struct OsVersion
{
    std::string osName;
    int32_t majorVersion = -1;
    int32_t minorVersion = -1;
};

OsVersion getOsVersion()
{
    utsname utsname = {};
    uname(&utsname);

    OsVersion version;
    version.osName = utsname.sysname;

    std::string acc;
    for (int32_t i = 0; utsname.release[i] != '\0'; i++)
    {
        char c = utsname.release[i];
        if (isdigit(c))
        {
            acc.push_back(utsname.release[i]);
        }
        else if (c == '.')
        {
            if (version.majorVersion == -1)
            {
                version.majorVersion = std::stoi(acc);
            }
            else
            {
                version.minorVersion = std::stoi(acc);
                break;
            }
            acc.clear();
        }
        else
        {
            break;
        }
    }

    return version;
}

int wcpMain(int argc, char** argv)
{
    OsVersion osVersion = getOsVersion();
    if (osVersion.osName != "Linux")
    {
        fprintf(stderr, "How did you even compile this on %s?\n", osVersion.osName.c_str());
        return 1;
    }
    if (osVersion.majorVersion < 5 || (osVersion.majorVersion == 5 && osVersion.minorVersion < 6))
    {
        fputs("Sorry, wcp requires at least Linux 5.6\n", stderr);
        return 1;
    }

    if(argc != 3)
    {
        fprintf(stderr, "Usage: %s SRC DEST\n", argv[0]);
        fputs("Supports copying files and folders.\n", stderr);
        fputs("Contributions welcome to add better argument handling, and multiple sources!\n", stderr);
        return 1;
    }

    std::filesystem::path src = argv[1];
    std::filesystem::path dest = argv[2];
    release_assert(!src.empty() && !dest.empty());

    size_t oneGig = 1024 * 1024 * 1024;
    size_t ramQuota = std::max(getPhysicalRamSize() / 10, oneGig);
    size_t blockSize = 128 * 1024 * 1024; // 128M
    size_t ringSize = ramQuota / blockSize;
    size_t fileDescriptorCap = 900;
    CopyQueue copyQueue(ringSize, fileDescriptorCap, Heap(ramQuota / blockSize, blockSize));
    copyQueue.start();

    struct statx srcStat = {};
    {
        Result result = myStatx(AT_FDCWD, src, 0, STATX_BASIC_STATS, srcStat);
        if (std::holds_alternative<Error>(result))
        {
            fprintf(stderr, "%s\n", std::get<Error>(result).humanFriendlyErrorMessage->c_str());
            return 1;
        }
    }

    struct statx destStat = {};
    int destStatResult = 0;
    {
        destStatResult = retrySyscall([&]()
        {
            statx(AT_FDCWD, dest.c_str(), 0, STATX_BASIC_STATS, &destStat);
        });

        if (destStatResult != 0 && destStatResult != ENOENT)
        {
            fprintf(stderr, "Failed to stat \"%s\": \"%s\"\n", dest.c_str(), strerror(destStatResult));
            return 1;
        }
    }

    {
        std::filesystem::path destRealParent = std::filesystem::absolute(dest);
        if (!destRealParent.has_filename()) // This is needed to account for if dest has a trailing slash
            destRealParent = destRealParent.parent_path();
        destRealParent = destRealParent.parent_path();

        release_assert(access(destRealParent.c_str(), F_OK) == 0);
    }

    if (S_ISDIR(srcStat.stx_mode))
    {
        dest /= src.filename();
        recursiveMkdir(dest.string());

        if (destStatResult == 0)
            release_assert(S_ISDIR(destStat.stx_mode));

        copyQueue.addRecursiveCopy(src, dest);
    }
    else if (S_ISREG(srcStat.stx_mode))
    {
        if (destStatResult == 0 && S_ISDIR(destStat.stx_mode))
            dest /= src.filename();

        copyQueue.addFileCopy(src, dest, &srcStat);
    }
    else
    {
        release_assert(false);
    }

    CopyQueue::OnCompletionAction completionAction = Config::NO_CLEANUP ? CopyQueue::OnCompletionAction::ExitProcessNoCleanup :
                                                     CopyQueue::OnCompletionAction::Return;

    bool success = copyQueue.join(completionAction);

    return int(!success);
}