#pragma once
#include <string>
#include <memory>
#include <variant>

void recursiveMkdir(std::string& path);
[[maybe_unused]] static void recursiveMkdir(const std::string& path) { std::string copy(path); recursiveMkdir(copy); }
[[maybe_unused]] static void recursiveMkdir(std::string&& path) { recursiveMkdir(path); }

class Error
{
public:
    std::unique_ptr<std::string> humanFriendlyErrorMessage;

    Error() = default;
    explicit Error(std::string&& message): humanFriendlyErrorMessage(std::make_unique<std::string>(std::move(message))) {}
};

using Result = std::variant<Error, nullptr_t>;
static constexpr nullptr_t Success() { return nullptr; }

using OpenResult = std::variant<Error, int>;
OpenResult myOpen(const std::string& path, int oflag, mode_t mode);
Result myClose(int fd);