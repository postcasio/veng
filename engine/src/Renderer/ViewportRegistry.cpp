#include <Veng/Renderer/ViewportRegistry.h>

#include <atomic>

namespace Veng::Renderer
{
    namespace
    {
        /// @brief Bit position of the salt within an id; the counter occupies the bits below it.
        constexpr u64 SaltShift = 32;

        /// @brief Next salt handed to a constructed registry.
        ///
        /// A distinct non-zero seed per registry keeps two registries' ids separable in their salt
        /// bits even when one is constructed at a freed one's address. Starts at 1 so the first
        /// registry's ids are non-zero (zero is the invalid id). Atomic so a test spinning up
        /// registries off the render thread stays honest.
        std::atomic<u64> s_NextSalt{1};
    }

    ViewportRegistry::ViewportRegistry() : m_Salt(s_NextSalt.fetch_add(1) << SaltShift) {}

    ViewportId ViewportRegistry::Mint(Viewport& viewport)
    {
        ++m_Counter;
        const ViewportId id{.Value = m_Salt | m_Counter};
        m_Viewports.emplace(id.Value, &viewport);
        return id;
    }

    void ViewportRegistry::Retire(ViewportId id)
    {
        m_Viewports.erase(id.Value);
    }

    const Viewport* ViewportRegistry::Resolve(ViewportId id) const
    {
        if (!id.IsValid() || (id.Value >> SaltShift) != (m_Salt >> SaltShift))
        {
            return nullptr;
        }

        const auto it = m_Viewports.find(id.Value);
        return it == m_Viewports.end() ? nullptr : it->second;
    }
}
