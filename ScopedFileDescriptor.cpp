#include <unistd.h>
#include "ScopedFileDescriptor.hpp"
#include "Assert.hpp"

ScopedFileDescriptor::~ScopedFileDescriptor()
{
    debug_assert(close(this->fd) == 0);
}

Result ScopedFileDescriptor::open(const std::string& path, int oflag, mode_t mode, bool showErrorMessages)
{
    OpenResult result = myOpen(path, oflag, mode, showErrorMessages);
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
