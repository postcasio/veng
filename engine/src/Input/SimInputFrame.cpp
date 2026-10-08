#include <Veng/Input/SimInputFrame.h>

#include <Veng/Input.h>

namespace Veng
{
    void SimInputFrame::BeginFrame(Input& input)
    {
        // Dropped here rather than when the frame's first simulation finishes, since a driver
        // stepping later in the same frame may still consume the motion.
        if (!m_Active)
        {
            input.DropSimDeltas();
        }
        input.BeginFrame(!m_Active || m_Stepped);
        m_Active = false;
        m_Stepped = false;
        m_Pointer = {};
        m_PointerScene = nullptr;
    }

    void SimInputFrame::SetPointer(const PointerRouting& routing, const Scene* scene)
    {
        m_Pointer = routing;
        m_PointerScene = scene;
    }

    PointerRouting SimInputFrame::GetPointer(const Scene& scene) const
    {
        return &scene == m_PointerScene ? m_Pointer : PointerRouting{};
    }

    void SimInputFrame::BeginSimStep(Input& input, const Scene& scene) const
    {
        if (m_PointerScene == nullptr || &scene == m_PointerScene)
        {
            input.BeginSimTick();
        }
        input.BeginGamepadSimTick();
    }

    void SimInputFrame::Report(const bool stepped)
    {
        m_Active = true;
        m_Stepped = m_Stepped || stepped;
    }
}
