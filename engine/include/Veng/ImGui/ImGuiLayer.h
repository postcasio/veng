#pragma once

#include <Veng/Veng.h>
#include <Veng/ImGui/ImGuiTexture.h>

namespace Veng
{
    class Window;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class Image;
    class ImageView;
    class Sampler;
    class DescriptorPool;
}

namespace Veng
{
    struct ImGuiLayerInfo
    {
        optional<path> DefaultFontPath;
        optional<path> IconFontPath;
    };

    // Owns everything ImGui: the ImGui/imnodes contexts, the Vulkan backend, a
    // dedicated descriptor pool, and the offscreen image the UI renders into.
    // The application opts in by setting ApplicationInfo::ImGui; headless and
    // non-UI consumers never construct one and pay nothing for ImGui at runtime.
    class ImGuiLayer
    {
    public:
        static Unique<ImGuiLayer> Create(const ImGuiLayerInfo& info, Renderer::Context& context, Window& window);

        ~ImGuiLayer();

        ImGuiLayer(const ImGuiLayer&) = delete;
        ImGuiLayer& operator=(const ImGuiLayer&) = delete;

        // Begin a new ImGui frame. Call once per frame before building any UI.
        void BeginFrame();

        // Render the built UI into the output image, leaving it sampleable for
        // compositing into the swap chain.
        void Render(Renderer::CommandBuffer& cmd);

        // The offscreen image the UI is rendered into.
        [[nodiscard]] Ref<Renderer::Image> GetOutputImage() const { return m_Image; }

        Ref<ImGuiTexture> CreateTexture(const Renderer::Sampler& sampler, const Renderer::ImageView& imageView);
        void DestroyTexture(const ImGuiTexture& texture);

    private:
        ImGuiLayer(const ImGuiLayerInfo& info, Renderer::Context& context, Window& window);

        void CreateResources();
        void DisposeResources();

        // A texture's descriptor set freed while command buffers that reference it
        // are still in flight; the free is deferred until the retire window elapses.
        struct PendingTextureRemoval;

        Renderer::Context& m_Context;

        // ImGui descriptor pool — forward-declared and held by pointer so this
        // public header pulls in no Vulkan; destroyed out-of-line after the
        // backend shuts down.
        Unique<Renderer::DescriptorPool> m_DescriptorPool;

        Ref<Renderer::Image> m_Image;
        Ref<Renderer::ImageView> m_ImageView;

        vector<PendingTextureRemoval> m_PendingTextureRemovals;

        // When the app builds no UI for a frame, BeginFrame ends the stale frame
        // so ImGui's internal state stays consistent.
        bool m_RenderedThisFrame = true;
    };
}
