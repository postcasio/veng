#include <Veng/Log.h>

#include <Veng/Time.h>

namespace Veng::Log
{
    static const char* ToString(Level lvl)
    {
        switch (lvl)
        {
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        }
        return "INFO";
    }

    void LogMessage(Level level, std::string_view message)
    {
        auto now = Time::Now();
        fmt::print("[{:.4f}] [{}] {}", now, ToString(level), message);

        if (!message.empty() && message.back() != '\n')
            fmt::print("\n");
    }
}
