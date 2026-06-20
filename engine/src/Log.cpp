#include <Veng/Log.h>

#include <Veng/Time.h>

namespace Veng::Log
{
    static const char* ToString(Level lvl)
    {
        switch (lvl)
        {
        case Level::Info:
            return "INFO";
        case Level::Warn:
            return "WARN";
        case Level::Error:
            return "ERROR";
        }
        return "INFO";
    }

    namespace
    {
        void DefaultSink(Level level, std::string_view message)
        {
            auto now = Time::Now();
            fmt::print("[{:.4f}] [{}] {}", now, ToString(level), message);

            if (!message.empty() && message.back() != '\n')
                fmt::print("\n");
        }

        Sink s_Sink = DefaultSink;
        Level s_MinimumLevel = Level::Info;
    }

    void SetSink(Sink sink)
    {
        s_Sink = sink ? std::move(sink) : Sink(DefaultSink);
    }

    void SetMinimumLevel(Level level)
    {
        s_MinimumLevel = level;
    }

    void LogMessage(Level level, std::string_view message)
    {
        if (static_cast<int>(level) < static_cast<int>(s_MinimumLevel))
            return;

        s_Sink(level, message);
    }
}
