#pragma once
#include "Util.hpp"

class ScopedFileDescriptor
{
public:
    ScopedFileDescriptor() = default;
    ~ScopedFileDescriptor();

    Result open(const std::string& path, int oflag, mode_t mode);
    int getFd() const;

private:
    int fd = -1;
};