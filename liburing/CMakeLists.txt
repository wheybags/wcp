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
