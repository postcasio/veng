#include "ConsolePanel.h"

#include <Veng/UI/UI.h>

namespace VengEditor
{
    using namespace Veng;

    ConsolePanel::ConsolePanel()
    {
        Log::SetSink(
            [this](Log::Level level, std::string_view message)
            {
                if (m_Entries.size() >= MaxEntries)
                {
                    m_Entries.erase(m_Entries.begin());
                }

                m_Entries.push_back({.Level = level, .Message = string(message)});
                m_ScrollToBottom = true;
            });
    }

    ConsolePanel::~ConsolePanel()
    {
        Log::SetSink(nullptr);
    }

    void ConsolePanel::OnUI()
    {
        if (UI::Button("Clear"))
        {
            m_Entries.clear();
        }

        UI::Separator();

        if (auto log = UI::Child("ConsoleScroll", {}, UI::WindowFlags::HorizontalScrollbar))
        {
            for (const Entry& entry : m_Entries)
            {
                const UI::Theme& theme = UI::GetTheme();
                vec4 color = theme.TextMuted;
                switch (entry.Level)
                {
                case Log::Level::Warn:
                    color = theme.Warning;
                    break;
                case Log::Level::Error:
                    color = theme.Error;
                    break;
                case Log::Level::Info:
                    break;
                }

                UI::TextColored(color, entry.Message);
            }

            if (m_ScrollToBottom)
            {
                UI::ScrollToHere();
                m_ScrollToBottom = false;
            }
        }
    }
}
