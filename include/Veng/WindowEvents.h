#pragma once

#include <Veng/Veng.h>
#include <Veng/Event.h>

namespace Veng
{
    class WindowResizeEvent final : public Event
    {
    public:
        WindowResizeEvent(u32 width, u32 height) :
            m_Width(width),
            m_Height(height)
        {
        }

        EVENT(WindowResize);

        [[nodiscard]] u32 GetWidth() const { return m_Width; }
        [[nodiscard]] u32 GetHeight() const { return m_Height; }

    private:
        const u32 m_Width;
        const u32 m_Height;
    };

    class WindowCloseEvent final : public Event
    {
    public:
        WindowCloseEvent() = default;

        EVENT(WindowClose);
    };
}
