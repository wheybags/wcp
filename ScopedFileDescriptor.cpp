#include <unistd.h>
#include "ScopedFileDescriptor.hpp"
#include "Assert.hpp"

ScopedFileDescriptor::~ScopedFileDescriptor()
{
    [[maybe_unused]] int ret = close(this->fd);
    debug_assert(ret == 0);
}

Result ScopedFileDescriptor::open(const std::string& path, int oflag, mode_t mode)
{
    OpenResult result = myOpen(path, oflag, mode);
    if (std::holds_alternative<Error>(result))
        return Error(std::move(std::get<Error>(result)));

    this->fd = std::get<int>(result);
    return Success();
}

int ScopedFileDescriptor::getFd() const
{
    debug_assert(this->fd > 0);
    return this->fd;
}
