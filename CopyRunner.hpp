#pragma once
#include <cstdint>
#include <sys/types.h>
#include <asm/types.h>
#include <functional>
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

    CopyRunner(CopyQueue* queue,
               std::shared_ptr<QueueFileDescriptor> sourceFd,
               std::shared_ptr<QueueFileDescriptor> destFd,
               off_t offset,
               off_t size,
               size_t alignment);
    ~CopyRunner();

    CopyRunner() = delete;
    CopyRunner(const CopyRunner&) = delete;
    CopyRunner& operator=(const CopyRunner&) = delete;

    bool needsBuffer() const { return this->buffer == nullptr; }
    void giveBuffer(uint8_t* buffer);

    [[nodiscard]] Result addToBatch();

    class ContinueTag {};
    class RescheduleTag {};
    class FinishedTag {};
    using RunnerResult = std::variant<Error, RescheduleTag, FinishedTag, ContinueTag>;
    RunnerResult onCompletionEvent(EventData::Type type, __s32 result);

public:
    static constexpr int32_t MAX_JOBS_PER_RUNNER = 2;

private:
    friend class TestContainer;
    std::optional<Error> deferredError;

    CopyQueue* queue;
    std::shared_ptr<QueueFileDescriptor> sourceFd;
    std::shared_ptr<QueueFileDescriptor> destFd;
    off_t offset;
    off_t size;
    size_t alignment;

    uint8_t* buffer = nullptr;
    uint8_t* bufferAligned = nullptr;

    off_t readOffset = 0;
    off_t writeOffset = 0;

    int32_t jobsRunning = 0;

    EventData readData {this, std::numeric_limits<__s32>::max(), EventData::Type::Read};
    EventData writeData {this, std::numeric_limits<__s32>::max(), EventData::Type::Write};
};

