#include <functional>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include "CopyQueue.hpp"
#include "CopyRunner.hpp"
#include "Config.hpp"
#include "Util.hpp"
#include "wcpMain.hpp"
#include "Assert.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"
#include "acutest.h"
#pragma GCC diagnostic pop

std::string getProjectBasePath()
{
    std::string thisFile = __FILE__;
    for (int32_t i = int32_t(thisFile.length()) - 1; i >= 0; i--)
    {
        if (thisFile[i] == '/')
            return thisFile.substr(0, i);
    }

    release_assert(false);
}

std::unique_ptr<CopyQueue> getTestQueue()
{
    size_t oneGig = 1024 * 1024 * 1024;
    size_t ramQuota = oneGig;
    size_t blockSize = 256 * 1024 * 1024; // 256M
    size_t ringSize = 100;
    size_t fileDescriptorCap = 512;
    auto queue = std::make_unique<CopyQueue>(ringSize, fileDescriptorCap, Heap(ramQuota / blockSize, blockSize));
    queue->start();

    return queue;
}

void runWithAllPartialModes(const std::function<void(void)>& func)
{
    Config::NO_CLEANUP = false;

    {
        Config::DEBUG_FORCE_PARTIAL_READS = false;
        Config::DEBUG_FORCE_PARTIAL_WRITES = false;
        func();
    }
    {
        Config::DEBUG_FORCE_PARTIAL_READS = false;
        Config::DEBUG_FORCE_PARTIAL_WRITES = true;
        func();
    }
    {
        Config::DEBUG_FORCE_PARTIAL_READS = true;
        Config::DEBUG_FORCE_PARTIAL_WRITES = false;
        func();
    }
    {
        Config::DEBUG_FORCE_PARTIAL_READS = true;
        Config::DEBUG_FORCE_PARTIAL_WRITES = true;
        func();
    }
}

// This function should do the same thing as generate_data() in bench.sh. They share cached data
std::string getTestDataFolder(size_t fileSize, size_t fileCount)
{
    auto sizeToString = [](size_t bytes) {
        size_t kibibyte = 1024;
        size_t mebibyte = kibibyte * 1024;
        size_t gibibyte = mebibyte * 1024;
        size_t tebibyte = gibibyte * 1024;

        size_t final = bytes;
        std::string unit;

        if (bytes >= tebibyte && bytes % tebibyte == 0)
        {
            final = bytes / tebibyte;
            unit = "T";
        }
        else if (bytes >= gibibyte && bytes % gibibyte == 0)
        {
            final = bytes / gibibyte;
            unit = "G";
        }
        else if (bytes >= mebibyte && bytes % mebibyte == 0)
        {
            final = bytes / mebibyte;
            unit = "M";
        }
        else if (bytes >= kibibyte && bytes % mebibyte == 0)
        {
            final = bytes / kibibyte;
            unit = "K";
        }

        return std::to_string(final) + unit;
    };

    std::string base = getProjectBasePath() + "/test_data";
    std::string folder = base + "/" + sizeToString(fileSize) + "_" + std::to_string(fileCount);
    std::string doneTag = folder + "/done_tag";

    if (access(doneTag.c_str(), F_OK) != 0)
    {
        if (access(folder.c_str(), F_OK) == 0)
            std::filesystem::remove_all(folder);

        std::string randFilesFolder = folder + "/" + sizeToString(fileSize);
        recursiveMkdir(randFilesFolder);

        std::vector<uint8_t> buffer;
        buffer.resize(fileSize);

        for (size_t i = 1; i <= fileCount; i++)
        {
            for (size_t j = 0; j < buffer.size(); j++)
                buffer[j] = rand() & 0xFF;

            FILE* newFile = fopen((randFilesFolder + "/" + std::to_string(i)).c_str(), "wb");
            TEST_ASSERT(newFile != nullptr);

            size_t remaining = buffer.size();
            uint8_t* ptr = buffer.data();
            do
            {
                size_t written = fwrite(ptr, 1, remaining, newFile);
                if (written == 0)
                    TEST_ASSERT(ferror(newFile) == 0);

                remaining -= written;
                ptr += written;
            } while (remaining);

            TEST_ASSERT(fclose(newFile) == 0);
        }

        FILE* doneTagFile = fopen(doneTag.c_str(), "wb");
        TEST_ASSERT(doneTagFile != nullptr);
        TEST_ASSERT(fclose(doneTagFile) == 0);
    }

    return folder;
}

void assertFilesEqual(const std::string& a, const std::string& b)
{
    auto readWholeFileSimple = [](const std::string& path)
    {
        FILE* f = fopen(path.c_str(), "rb");
        TEST_ASSERT(f != nullptr);

        TEST_ASSERT(fseek(f, SEEK_END, 0) == 0);
        long int size = ftell(f);
        TEST_ASSERT(size > 0);

        std::vector<uint8_t> data;
        data.resize(size);

        TEST_ASSERT(fseek(f, SEEK_SET, 0) == 0);

        size_t remaining = size;
        uint8_t* ptr = data.data();
        do
        {
            size_t read = fread(ptr, 1, remaining, f);
            if (read == 0)
                TEST_ASSERT(ferror(f) == 0);

            remaining -= read;
            ptr += read;
        } while (remaining);

        TEST_ASSERT(fclose(f) == 0);

        return data;
    };

    std::vector<uint8_t> aData = readWholeFileSimple(a);
    std::vector<uint8_t> bData = readWholeFileSimple(b);

    TEST_ASSERT(aData == bData);
}

void assertFoldersEqual(const std::string& a, const std::string& b)
{
    // We just spawn diff, it's a known good implementation
    const char* argv[] = {"/usr/bin/diff", "-r", a.c_str(), b.c_str(), nullptr};

    pid_t pid = 0;
    int err = posix_spawn(&pid, argv[0], nullptr, nullptr, (char**)argv, environ);
    TEST_ASSERT(err == 0);
    TEST_ASSERT(waitpid(pid, &err, 0) != -1);
    TEST_ASSERT(err == 0);
}

void clearTargetFile(const std::string& path)
{
    errno = 0;
    remove(path.c_str());
    TEST_ASSERT(errno == 0 || errno == ENOENT);
}

class TestContainer
{
public:
    static void CopySmallFileNotAlignedSize()
    {
        runWithAllPartialModes([]() {
            std::unique_ptr<CopyQueue> queue = getTestQueue();
            std::string srcFile = getTestDataFolder(10, 1) + "/10/1";
            std::string destPath = "/tmp/test_CopySmallFileNotAlignedSize";

            clearTargetFile(destPath);

            queue->addFileCopy(srcFile, destPath);
            queue->join();

            assertFilesEqual(srcFile, destPath);
        });
    }

    static void CopyLargeFolder()
    {
        runWithAllPartialModes([]() {
            std::unique_ptr<CopyQueue> queue = getTestQueue();
            std::string srcFolder = getTestDataFolder(1024 * 1024 * 512, 5);
            std::string destPath = getProjectBasePath() + "/test_dest";

            std::filesystem::remove_all(destPath);

            queue->addRecursiveCopy(srcFolder, destPath);
            queue->join();

            assertFoldersEqual(srcFolder, destPath);
        });
    }

    static void ResolveCopyDestination()
    {
        Config::DEBUG_FORCE_PARTIAL_READS = false;
        Config::DEBUG_FORCE_PARTIAL_WRITES = false;
        Config::NO_CLEANUP = false;

        std::string base = getProjectBasePath() + "/test_data/TestResolveCopyDestination";
        std::string source = base + "/source";
        std::string dest = base + "/dest";
        std::string contentFile = source + "/content_file";

        // set up a clean copy of our source folder, containing a single file "content_file"
        {
            std::filesystem::remove_all(base);
            recursiveMkdir(base);

            TEST_ASSERT(mkdir(source.c_str(), S_IRWXU) == 0);

            FILE *f = fopen(contentFile.c_str(), "wb");
            TEST_ASSERT(f != nullptr);
            TEST_ASSERT(fclose(f) == 0);
        }

        auto call = [](const std::string &a, const std::string &b) {
            int argc = 3;
            const char *argv[] = {"wcp", a.c_str(), b.c_str(), nullptr};
            wcpMain(argc, (char **) argv);
        };

        call(source, dest);
        TEST_ASSERT(access((dest + "/source/content_file").c_str(), F_OK) == 0);

        std::filesystem::remove_all(dest);
        call(source + "/", dest);
        TEST_ASSERT(access((dest + "/content_file").c_str(), F_OK) == 0);

        std::filesystem::remove_all(dest);
        call(source, dest + "/");
        TEST_ASSERT(access((dest + "/source/content_file").c_str(), F_OK) == 0);

        std::filesystem::remove_all(dest);
        call(source + "/", dest + "/");
        TEST_ASSERT(access((dest + "/content_file").c_str(), F_OK) == 0);

        std::filesystem::remove_all(dest);
        call(source + "/.", dest);
        TEST_ASSERT(access((dest + "/content_file").c_str(), F_OK) == 0);

        std::filesystem::remove_all(dest);
        call(contentFile, dest);
        TEST_ASSERT(access(dest.c_str(), F_OK) == 0);

        std::filesystem::remove_all(dest);
        TEST_ASSERT(mkdir(dest.c_str(), S_IRWXU) == 0);
        call(contentFile, dest);
        TEST_ASSERT(access((dest + "/content_file").c_str(), F_OK) == 0);
    }

    static void TruncatedDuringCopy()
    {
        std::unique_ptr<CopyQueue> queue = getTestQueue();
        std::string srcFile = getProjectBasePath() + "/test_data/TruncateTest";
        std::string destPath = getProjectBasePath() + "/test_dest";

        std::filesystem::remove_all(destPath);

        size_t initialSize = 1024*1024*512;
        size_t truncatedSize = 1024*1024*300;

        int srcWriteFd = open(srcFile.c_str(), O_WRONLY | O_TRUNC | O_CREAT, S_IRWXU);
        release_assert(srcWriteFd > 0);
        release_assert(ftruncate(srcWriteFd, initialSize) == 0);
        fsync(srcWriteFd);


        int32_t didTruncateAtInvocation = -1;
        int32_t invocations = 0;

        CopyRunner::testingCallbackOnCompletionEventStart = [&](CopyRunner&, CopyRunner::EventData::Type type, __s32 result)
        {
            if (didTruncateAtInvocation == -1 && type == CopyRunner::EventData::Type::Read && result > 0)
            {
                didTruncateAtInvocation = invocations;

                int a = ftruncate64(srcWriteFd, truncatedSize);
                release_assert( a== 0);
            }

            invocations++;
        };

        queue->addFileCopy(srcFile, destPath);
        queue->join();

        struct stat64 sb = {};
        release_assert(stat64(destPath.c_str(), &sb) == 0);
        release_assert(size_t(sb.st_size) == truncatedSize);

        CopyRunner::testingCallbackOnCompletionEventStart = nullptr;
        release_assert(close(srcWriteFd) == 0);

        release_assert(didTruncateAtInvocation == 0);
        release_assert(invocations > didTruncateAtInvocation);
    }
};

TEST_LIST =
{
    {"CopySmallFileNotAlignedSize", TestContainer::CopySmallFileNotAlignedSize},
    {"ResolveCopyDestination", TestContainer::ResolveCopyDestination},
    {"TruncatedDuringCopy", TestContainer::TruncatedDuringCopy},
    {"CopyLargeFolder", TestContainer::CopyLargeFolder},
    {nullptr, nullptr }
};