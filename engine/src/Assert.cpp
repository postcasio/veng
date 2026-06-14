#include <Veng/Assert.h>

#include <cstdlib>

namespace Veng::Detail
{
    void FatalAssert(const char* file, int line, const char* expr, std::string_view message)
    {
        Log::Error("Assertion failed: ({}) at {}:{}\n  {}", expr, file, line, message);

#if defined(VE_DEBUG)
        VE_DEBUG_BREAK();
#endif

        std::abort();
    }
}
