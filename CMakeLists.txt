cmake_minimum_required (VERSION 3.11)
set(CMAKE_CXX_STANDARD 17)

project(wcp)

option(WERROR "" OFF)

add_subdirectory(liburing)

set(WCP_COMPILER_FLAGS -Wall -pedantic -Wextra)
if (WERROR)
    set(WCP_COMPILER_FLAGS ${WCP_COMPILER_FLAGS} -Werror)
endif()

add_library(wcp_lib CopyRunner.cpp CopyRunner.hpp Assert.hpp Config.hpp CopyQueue.cpp CopyQueue.hpp FileDescriptor.hpp Heap.hpp Heap.cpp Config.cpp tests.cpp Util.hpp Util.cpp wcpMain.cpp wcpMain.hpp)

find_package(Threads)
target_link_libraries(wcp_lib Uring ${CMAKE_THREAD_LIBS_INIT})
target_compile_options(wcp_lib PRIVATE ${WCP_COMPILER_FLAGS})

add_executable(wcp main.cpp)
target_link_libraries(wcp wcp_lib)

add_executable(tests tests.cpp acutest.h)
target_link_libraries(tests wcp_lib)
target_compile_options(tests PRIVATE ${WCP_COMPILER_FLAGS})