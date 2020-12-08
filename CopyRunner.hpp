#pragma once
#include <cstdint>
#include <sys/types.h>
#include <asm/types.h>

struct io_uring;

class CopyRunner
{
public:
    struct EventData
    {
        enum class Type
        {
            Read,
            Write
        };

        CopyRunner* copyData;
        __s32 resultOverride;
        Type type;
    };

    CopyRunner(io_uring* ring, int sourceFd, int destFd, off_t size);
    ~CopyRunner();

    void addToBatch();
    bool onCompletionEvent(EventData::Type type, __s32 result);

private:
    io_uring* ring;
    int sourceFd;
    int destFd;
    off_t size;
    uint8_t* buffer;

    off_t readOffset = 0;
    off_t writeOffset = 0;

    int jobsRunning = 0;

    EventData readData {this, 0, EventData::Type::Read};
    EventData writeData {this, 0, EventData::Type::Write};

public:
    static int32_t copiesRunning;
    static int32_t copiesPendingsubmit;
};



