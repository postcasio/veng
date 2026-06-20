#pragma once
#include <Veng/Veng.h>

namespace Veng
{
    class ImGuiLayer;

    /// @brief A sampled image registered with the ImGui Vulkan backend so it can be drawn
    ///        with `ImGui::Image`.
    ///
    /// Created and owned through `ImGuiLayer::CreateTexture`; the raw descriptor set lives
    /// in `Native` (see `Veng/Renderer/Native.h`).
    class ImGuiTexture
    {
    public:
        /// @brief Destroys the texture, invoking `ImGuiLayer::DestroyTexture` for deferred cleanup.
        ~ImGuiTexture();

        /// @brief Returns the opaque texture id passed to `ImGui::Image`.
        [[nodiscard]] u64 GetTextureId() const;

        /// @brief Vulkan descriptor set backing this texture registration.
        struct Native;

        /// @brief Returns the backend native descriptor set.
        [[nodiscard]] Native& GetNative() const;

    private:
        /// @brief Constructs the texture; only `ImGuiLayer` may construct instances.
        /// @param native  Owning wrapper around the allocated descriptor set.
        /// @param layer   Layer that allocated the descriptor set, called on destruction.
        ImGuiTexture(Unique<Native> native, ImGuiLayer& layer);

        /// @brief Owning wrapper around the Vulkan descriptor set.
        Unique<Native> m_Native;
        /// @brief Layer that allocated this texture, called on destruction.
        ImGuiLayer& m_Layer;

        friend class ImGuiLayer;
    };
}
