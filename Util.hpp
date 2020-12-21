#pragma once
#include <string>

void recursiveMkdir(std::string& path);
[[maybe_unused]] static void recursiveMkdir(const std::string& path) { std::string copy(path); recursiveMkdir(copy); }
[[maybe_unused]] static void recursiveMkdir(std::string&& path) { recursiveMkdir(path); }