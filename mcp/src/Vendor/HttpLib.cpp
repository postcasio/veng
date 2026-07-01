// Aggregation TU that compiles cpp-httplib's single-header implementation once
// into libveng_mcp. Built with -fexceptions (a per-TU override) so httplib's
// throws compile while the rest of libveng_mcp stays -fno-exceptions. httplib
// contains any exception at its own dispatch boundary (set_exception_handler),
// so none unwinds out of this TU into a -fno-exceptions frame.
#define CPPHTTPLIB_IMPLEMENTATION
#include "httplib.h"
