#pragma once

#include <Veng/Log.h>

#include <VengEditor/EditorPanel.h>

namespace VengEditor
{
    /// @brief Scrolling log panel backed by a bounded ring of Log entries.
    ///
    /// Installs a Log sink on construction that appends to the ring; restores
    /// the default sink on destruction.
    class ConsolePanel final : public EditorPanel
    {
    public:
        ConsolePanel();
        ~ConsolePanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Console"; }
        void OnImGui() override;

    private:
        /// @brief A single captured log entry.
        struct Entry
        {
            Veng::Log::Level Level;
            Veng::string Message;
        };

        Veng::vector<Entry> m_Entries;
        /// @brief Set when a new entry is appended; cleared after ScrollToHere.
        bool m_ScrollToBottom = false;

        /// @brief Maximum number of entries retained before the oldest is evicted.
        static constexpr Veng::usize MaxEntries = 1000;
    };
}
