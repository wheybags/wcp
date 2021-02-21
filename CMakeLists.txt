cmake_minimum_required (VERSION 3.11)
set(CMAKE_CXX_STANDARD 17)

project(wcp)

option(WERROR "" OFF)

add_subdirectory(liburing)

set(WCP_COMPILER_FLAGS -Wall -pedantic -Wextra)
if (WERROR)
    set(WCP_COMPILER_FLAGS ${WCP_COMPILER_FLAGS} -Werror)
endif()

add_library(wcp_lib
    src/CopyRunner.cpp
    src/CopyRunner.hpp
    src/Assert.hpp
    src/Config.hpp
    src/CopyQueue.cpp
    src/CopyQueue.hpp
    src/QueueFileDescriptor.hpp
    src/Heap.hpp
    src/Heap.cpp
    src/Config.cpp
    src/Util.hpp
    src/Util.cpp
    src/wcpMain.cpp
    src/wcpMain.hpp
    src/QueueFileDescriptor.cpp
    src/ScopedFileDescriptor.cpp
    src/ScopedFileDescriptor.hpp
    src/ETACalculator.hpp
    src/ETACalculator.cpp)

find_package(Threads)
target_link_libraries(wcp_lib Uring ${CMAKE_THREAD_LIBS_INIT})
target_compile_options(wcp_lib PRIVATE ${WCP_COMPILER_FLAGS})

add_executable(wcp src/main.cpp)
target_link_libraries(wcp wcp_lib)

add_executable(tests src/tests.cpp src/acutest.h)
target_link_libraries(tests wcp_lib)
target_compile_options(tests PRIVATE ${WCP_COMPILER_FLAGS})