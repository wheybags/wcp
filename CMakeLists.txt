cmake_minimum_required (VERSION 3.11)

project(wcp)

add_executable(wcp main.cpp CopyRunner.cpp CopyRunner.hpp Assert.hpp Config.hpp)
target_link_libraries(wcp uring)
