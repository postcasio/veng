#include <Veng/Renderer/SceneCapturePool.h>

#include <Veng/Renderer/SceneCapture.h>

#include <cstddef>

namespace Veng::Renderer
{
    SceneCapturePool::SceneCapturePool(const usize capacity) : m_Capacity(capacity) {}

    SceneCapturePool::~SceneCapturePool() = default;

    Unique<SceneCapture> SceneCapturePool::Take(const SceneCaptureInfo& info)
    {
        // The most recently returned first: it has sat idle the shortest time.
        for (usize i = m_Held.size(); i-- > 0;)
        {
            if (m_Held[i]->IsConfiguredFor(info))
            {
                Unique<SceneCapture> capture = std::move(m_Held[i]);
                m_Held.erase(m_Held.begin() + static_cast<std::ptrdiff_t>(i));
                return capture;
            }
        }
        return nullptr;
    }

    void SceneCapturePool::Return(Unique<SceneCapture> capture)
    {
        if (capture == nullptr)
        {
            return;
        }
        capture->DetachFromDriveList();
        if (m_Capacity == 0)
        {
            return;
        }
        if (m_Held.size() >= m_Capacity)
        {
            m_Held.erase(m_Held.begin());
        }
        capture->ResetForReuse();
        m_Held.push_back(std::move(capture));
    }

    void SceneCapturePool::Clear()
    {
        m_Held.clear();
    }
}
