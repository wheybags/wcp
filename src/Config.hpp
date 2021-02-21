#pragma once

namespace Config
{
    static constexpr bool DEBUG_COPY_OPS = false;
    extern bool DEBUG_FORCE_PARTIAL_READS;
    extern bool DEBUG_FORCE_PARTIAL_WRITES;
    extern bool NO_CLEANUP;
    static constexpr bool VALGRIND_MODE = false;
    static constexpr bool PROGRESS_DEBUG_SIMPLE = false;
    static constexpr bool TEST_ETA_CALCULATION = false;
}

