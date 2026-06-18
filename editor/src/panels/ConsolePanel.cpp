#include "ConsolePanel.h"

#include <Veng/UI/UI.h>

namespace VengEditor
{
    using namespace Veng;

    ConsolePanel::ConsolePanel()
    {
        Log::SetSink([this](Log::Level level, std::string_view message)
        {
            if (m_Entries.size() >= MaxEntries)
                m_Entries.erase(m_Entries.begin());

            m_Entries.push_back({.Level = level, .Message = string(message)});
            m_ScrollToBottom = true;
        });
    }

    ConsolePanel::~ConsolePanel()
    {
        Log::SetSink(nullptr);
    }

    void ConsolePanel::OnImGui()
    {
        if (UI::Button("Clear"))
            m_Entries.clear();

        UI::Separator();

        if (auto log = UI::Child("ConsoleScroll", {}, UI::WindowFlags::HorizontalScrollbar))
        {
            for (const Entry& entry : m_Entries)
            {
                vec4 color{0.8f, 0.8f, 0.8f, 1.0f};
                switch (entry.Level)
                {
                case Log::Level::Warn: color = {1.0f, 0.8f, 0.2f, 1.0f}; break;
                case Log::Level::Error: color = {1.0f, 0.3f, 0.3f, 1.0f}; break;
                case Log::Level::Info: break;
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
