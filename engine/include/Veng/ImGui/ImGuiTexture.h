#pragma once
#include <Veng/Veng.h>

namespace Veng
{
    class ImGuiLayer;

    /// @brief A sampled image registered with the ImGui layer so it can be drawn with
    ///        `ImGui::Image`.
    ///
    /// Created and owned through `ImGuiLayer::CreateTexture`; its bindless texture and sampler
    /// slots live in `Native`.
    class VE_API ImGuiTexture
    {
    public:
        /// @brief Destroys the texture, invoking `ImGuiLayer::DestroyTexture` for deferred cleanup.
        ~ImGuiTexture();

        /// @brief Returns the opaque texture id passed to `ImGui::Image`; never zero.
        [[nodiscard]] u64 GetTextureId() const;

        /// @brief The bindless slots backing this texture registration.
        struct Native;

        /// @brief Returns the registration's bindless slots.
        [[nodiscard]] Native& GetNative() const;

    private:
        /// @brief Constructs the texture; only `ImGuiLayer` may construct instances.
        /// @param native  The registration's bindless slots.
        /// @param layer   Layer that registered the texture, called on destruction.
        ImGuiTexture(Unique<Native> native, ImGuiLayer& layer);

        /// @brief The registration's bindless slots.
        Unique<Native> m_Native;
        /// @brief Layer that allocated this texture, called on destruction.
        ImGuiLayer& m_Layer;

        friend class ImGuiLayer;
    };
}
