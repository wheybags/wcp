cmake_minimum_required (VERSION 3.11)
set(CMAKE_CXX_STANDARD 17)

project(wcp)

add_subdirectory(liburing)

add_library(wcp_lib CopyRunner.cpp CopyRunner.hpp Assert.hpp Config.hpp CopyQueue.cpp CopyQueue.hpp FileDescriptor.hpp Heap.hpp Heap.cpp Config.cpp tests.cpp Util.hpp Util.cpp)

find_package(Threads)
target_link_libraries(wcp_lib Uring ${CMAKE_THREAD_LIBS_INIT})

add_executable(wcp main.cpp)
target_link_libraries(wcp wcp_lib)

add_executable(tests tests.cpp acutest.h)
target_link_libraries(tests wcp_lib)