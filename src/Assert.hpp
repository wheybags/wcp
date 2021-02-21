#pragma once
#include <cstdio>
#include <cstdlib>

#ifdef _MSC_VER
#define DEBUG_BREAK __debugbreak()
#else
#include <csignal>
#define DEBUG_BREAK raise(SIGTRAP);
#endif

#define message_and_abort_fmt(message, ...)                                                                                                                    \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        fprintf(stderr, message, __VA_ARGS__);                                                                                                                 \
        DEBUG_BREAK;                                                                                                                                           \
        abort();                                                                                                                                               \
    } while (0)

#define message_and_abort(message) message_and_abort_fmt("%s\n", message)

#define release_assert(cond)                                                                                                                                   \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        if (!(cond))                                                                                                                                           \
            message_and_abort_fmt("ASSERTION FAILED: (%s) in %s:%d\n", #cond, __FILE__, __LINE__);                                                             \
    } while (0)

#ifdef NDEBUG
#define debug_assert(cond)                                                                                                                                     \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        (void)sizeof(cond);                                                                                                                                    \
    } while (0)
#else
#define debug_assert(cond) release_assert(cond)
#endif