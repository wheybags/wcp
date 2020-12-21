#include "Config.hpp"

namespace Config
{
    const bool DEBUG_COPY_OPS = false;
    bool DEBUG_FORCE_PARTIAL_READS = false;
    bool DEBUG_FORCE_PARTIAL_WRITES = false;

    bool NO_CLEANUP =
#ifdef NDEBUG
        true;
#else
        false;
#endif
}