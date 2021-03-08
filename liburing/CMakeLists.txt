include (CheckCSourceCompiles)

check_c_source_compiles("
#include <linux/fs.h>
int main(int argc, char **argv)
{
    __kernel_rwf_t x;
    x = 0;
    return x;
}" HAVE__kernel_rwf_t)

check_c_source_compiles("
#include <linux/time.h>
#include <linux/time_types.h>
int main(int argc, char **argv)
{
    struct __kernel_timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1;
    return 0;
}" HAVE__kernel_timespec)

check_c_source_compiles("
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
int main(int argc, char **argv)
{
  struct open_how how;
  how.flags = 0;
  how.mode = 0;
  how.resolve = 0;
  return 0;
}" HAVE_open_how)


set(compat_h_src "
/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_COMPAT_H
#define LIBURING_COMPAT_H
")

if (NOT HAVE__kernel_rwf_t)
    set(compat_h_src "${compat_h_src} 
    typedef int __kernel_rwf_t;
    ")
endif()

if (HAVE__kernel_timespec)
    set(compat_h_src "${compat_h_src} 
    #include <linux/time_types.h>
    ")
else()
    set(compat_h_src "${compat_h_src} 
    #include <stdint.h>

    struct __kernel_timespec {
	    int64_t		tv_sec;
	    long long	tv_nsec;
    };
    ")
endif()

if (NOT HAVE__open_how)
    set(compat_h_src "${compat_h_src} 
    #include <inttypes.h>

    struct open_how {
	    uint64_t	flags;
	    uint64_t	mode;
	    uint64_t	resolve;
    };
    ")
endif()

set(compat_h_src "${compat_h_src} 
#endif")

write_file("${CMAKE_CURRENT_SOURCE_DIR}/src/include/liburing/compat.h" "${compat_h_src}")

add_library(Uring
    src/setup.c
    src/queue.c
    src/syscall.h
    src/syscall.c
    src/register.c
    src/include/liburing.h
    src/include/liburing/barrier.h
    src/include/liburing/io_uring.h
    )

target_include_directories(Uring SYSTEM PUBLIC src/include)
