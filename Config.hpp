#pragma once

#define DEBUG_COPY_OPS 0
#define DEBUG_FORCE_PARTIAL_READS 0
#define DEBUG_FORCE_PARTIAL_WRITES 0

#ifdef NDEBUG
#   define NO_CLEANUP 1
#else
#   define NO_CLEANUP 0
#endif