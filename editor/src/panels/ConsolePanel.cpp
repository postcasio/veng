#include "ConsolePanel.h"

#include <Veng/Vendor/ImGui.h>

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
        if (ImGui::Button("Clear"))
            m_Entries.clear();

        ImGui::Separator();

        if (ImGui::BeginChild("ConsoleScroll", {0, 0}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            for (const Entry& entry : m_Entries)
            {
                ImVec4 color{0.8f, 0.8f, 0.8f, 1.0f};
                switch (entry.Level)
                {
                case Log::Level::Warn: color = {1.0f, 0.8f, 0.2f, 1.0f}; break;
                case Log::Level::Error: color = {1.0f, 0.3f, 0.3f, 1.0f}; break;
                case Log::Level::Info: break;
                }

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(entry.Message.c_str());
                ImGui::PopStyleColor();
            }

            if (m_ScrollToBottom)
            {
                ImGui::SetScrollHereY(1.0f);
                m_ScrollToBottom = false;
            }
        }
        ImGui::EndChild();
    }
}
