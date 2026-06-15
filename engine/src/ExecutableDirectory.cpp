#include <Veng/Application.h>

#include <Veng/Assert.h>

#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace Veng
{
    path ExecutableDirectory()
    {
#if defined(_WIN32)
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;)
        {
            const DWORD length =
                GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            VE_ASSERT(length != 0, "GetModuleFileNameW failed");
            if (length < buffer.size())
            {
                return path(std::wstring(buffer.data(), length)).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__APPLE__)
        u32 size = 0;
        // First call with a zero-sized buffer asks dyld for the required length.
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size);
        const int result = _NSGetExecutablePath(buffer.data(), &size);
        VE_ASSERT(result == 0, "_NSGetExecutablePath failed");
        return path(buffer.data()).parent_path();
#else
        std::vector<char> buffer(1024);
        for (;;)
        {
            const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
            VE_ASSERT(length > 0, "readlink(/proc/self/exe) failed");
            if (static_cast<usize>(length) < buffer.size())
            {
                return path(string(buffer.data(), static_cast<usize>(length))).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
#endif
    }
}
