cmake_minimum_required (VERSION 3.11)
set(CMAKE_CXX_STANDARD 17)

project(wcp)

add_executable(wcp main.cpp CopyRunner.cpp CopyRunner.hpp Assert.hpp Config.hpp CopyQueue.cpp CopyQueue.hpp)

find_package(Threads)
target_link_libraries(wcp uring ${CMAKE_THREAD_LIBS_INIT})
