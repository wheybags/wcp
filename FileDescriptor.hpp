#pragma once
#include <unistd.h>
#include "Assert.hpp"

class FileDescriptor
{
public:
    explicit FileDescriptor(int fd) : fd(fd) { debug_assert(fd > 2); }
    ~FileDescriptor() { debug_assert(fd > 2); close(fd);}

    FileDescriptor(const FileDescriptor&) = delete;
    void operator=(const FileDescriptor&) = delete;

    int fd;
};


