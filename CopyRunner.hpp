#pragma once
#include <cstdint>
#include <sys/types.h>
#include <asm/types.h>
#include "CopyQueue.hpp"


class CopyQueue;

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

    CopyRunner(CopyQueue* queue, int sourceFd, int destFd, off_t size);
    ~CopyRunner();

    void addToBatch();
    bool onCompletionEvent(EventData::Type type, __s32 result);

private:
    CopyQueue* queue;
    int sourceFd;
    int destFd;
    off_t size;
    uint8_t* buffer;

    off_t readOffset = 0;
    off_t writeOffset = 0;

    int jobsRunning = 0;

    EventData readData {this, 0, EventData::Type::Read};
    EventData writeData {this, 0, EventData::Type::Write};
};

