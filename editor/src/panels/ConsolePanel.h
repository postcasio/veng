#pragma once

#include <Veng/Log.h>

#include <VengEditor/EditorPanel.h>

namespace VengEditor
{
    // Mirrors Log:: output into a scrolling text area. On construction it installs
    // a Log sink that appends entries to a bounded ring; on destruction it restores
    // the default sink.
    class ConsolePanel final : public EditorPanel
    {
    public:
        ConsolePanel();
        ~ConsolePanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Console"; }
        void OnImGui() override;

    private:
        struct Entry
        {
            Veng::Log::Level Level;
            Veng::string Message;
        };

        Veng::vector<Entry> m_Entries;
        bool m_ScrollToBottom = false;

        static constexpr Veng::usize MaxEntries = 1000;
    };
}
