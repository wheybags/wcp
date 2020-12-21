#pragma once
#include <string>

void recursiveMkdir(std::string& path);
static void recursiveMkdir(const std::string& path) { std::string copy(path); recursiveMkdir(copy); }
static void recursiveMkdir(std::string&& path) { recursiveMkdir(path); }