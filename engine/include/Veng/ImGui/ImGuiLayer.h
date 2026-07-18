#pragma once

#include <Veng/Veng.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Input/InputConsumer.h>

namespace Veng
{
    class Window;
    class Event;
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
    /// @brief Construction parameters for `ImGuiLayer`.
    struct ImGuiLayerInfo
    {
        /// @brief Path to the default UI font; overrides the embedded engine default (Roboto) when set.
        optional<path> DefaultFontPath;
        /// @brief Path to an icon font merged on top of the default; overrides the embedded
        ///        engine default (FontAwesome Solid) when set.
        optional<path> IconFontPath;
    };

    /// @brief Owns the ImGui/imnodes contexts, the Vulkan backend, a dedicated descriptor pool,
    ///        and the offscreen image the UI renders into.
    ///
    /// The application opts in by setting `ApplicationInfo::ImGui`; headless and non-UI consumers
    /// never construct one and pay nothing for ImGui at runtime.
    ///
    /// It is an `InputConsumer` the app registers first with the `InputRouter`, so it is offered
    /// every UI-owned event ahead of any later consumer and reads the cursor-capture signal to
    /// suspend its cursor poll while the cursor is captured.
    class ImGuiLayer : public InputConsumer
    {
    public:
        /// @brief Creates an `ImGuiLayer`, initializing ImGui and the Vulkan backend.
        /// @param info     Font configuration.
        /// @param context  Renderer context owning the device.
        /// @param window   GLFW window ImGui reads input from.
        static Unique<ImGuiLayer> Create(const ImGuiLayerInfo& info, Renderer::Context& context,
                                         Window& window);

        /// @brief Destroys the ImGui layer, shutting down ImGui and the Vulkan backend.
        ~ImGuiLayer() override;

        ImGuiLayer(const ImGuiLayer&) = delete;
        ImGuiLayer& operator=(const ImGuiLayer&) = delete;

        /// @brief Begins a new ImGui frame.
        ///
        /// Call once per frame before building any UI, after the frame's events are forwarded
        /// (ImGui's NewFrame consumes the events ForwardEvent queued into the backend).
        void BeginFrame();

        /// @brief Forwards one window event into the ImGui GLFW backend.
        ///
        /// The engine owns the GLFW callbacks (the backend is initialized with callbacks off),
        /// so the InputRouter calls this to hand ImGui the events routed to the UI layer — and
        /// withholds the ones gameplay focus swallows. Translates the engine event to the
        /// backend's chain-callback so keymap/mods/char handling stay the backend's job. The
        /// overlay is cooperative: it always returns false so a later consumer still sees the
        /// event and the router's snapshot fold is unaffected.
        /// @param event  The event to forward; non-input events relevant to ImGui (focus) included.
        /// @return False always: the overlay does not stop the event's fall-through.
        bool ForwardEvent(const Event& event) override;

        /// @brief Suspends or resumes ImGui's mouse handling from the router's capture signal.
        ///
        /// While the cursor is captured the GLFW backend still polls the (disabled, virtual)
        /// cursor position in NewFrame, which would otherwise drift ImGui hover states even though
        /// no mouse events are forwarded; disabling the mouse while captured stops that.
        /// Toggles `ImGuiConfigFlags_NoMouse`.
        /// @param captured  True while the OS cursor is captured (disables ImGui's mouse).
        void OnCursorCaptured(bool captured) override;

        /// @brief Renders the built UI into the output image, leaving it sampleable for compositing.
        /// @param cmd  Command buffer the render pass is recorded into.
        void Render(Renderer::CommandBuffer& cmd);

        /// @brief Pushes the active `UI::Theme` into the live ImGui and imnodes styles.
        ///
        /// Called once at construction. A host that swaps the theme at runtime
        /// (`UI::SetTheme`) calls this afterward to refresh the rendered style.
        void ApplyTheme();

        /// @brief Returns the offscreen image the UI is rendered into.
        [[nodiscard]] Ref<Renderer::Image> GetOutputImage() const { return m_Image; }

        /// @brief Registers a sampler/image pair with the ImGui Vulkan backend and returns an owning wrapper.
        /// @param sampler    Sampler the texture is accessed through.
        /// @param imageView  Image view to register.
        Ref<ImGuiTexture> CreateTexture(const Renderer::Sampler& sampler,
                                        const Renderer::ImageView& imageView);

        /// @brief Deregisters a texture, deferring the descriptor-set free until in-flight frames retire.
        /// @param texture  Texture previously created with `CreateTexture`.
        void DestroyTexture(const ImGuiTexture& texture);

    private:
        /// @brief Internal constructor; called only by `Create`.
        ImGuiLayer(const ImGuiLayerInfo& info, Renderer::Context& context, Window& window);

        /// @brief Allocates the descriptor pool and offscreen image.
        void CreateResources();

        /// @brief Releases the descriptor pool and offscreen image.
        void ReleaseResources();

        /// @brief Deferred texture removal: a descriptor set freed while command buffers that
        ///        reference it are still in flight; the free runs after the retire window elapses.
        struct PendingTextureRemoval;

        /// @brief Renderer context providing the device and retire queue.
        Renderer::Context& m_Context;

        /// @brief Borrowed window; its GLFWwindow backs the event forwarders.
        Window& m_Window;

        /// @brief ImGui descriptor pool, forward-declared so this public header pulls in no Vulkan.
        ///
        /// Destroyed out-of-line after the backend shuts down.
        Unique<Renderer::DescriptorPool> m_DescriptorPool;

        /// @brief Offscreen image the UI is rendered into.
        Ref<Renderer::Image> m_Image;
        /// @brief View of `m_Image` used by the backend.
        Ref<Renderer::ImageView> m_ImageView;

        /// @brief Textures awaiting deferred destruction.
        vector<PendingTextureRemoval> m_PendingTextureRemovals;

        /// @brief Tracks whether UI was rendered in the current frame.
        ///
        /// When false at the next `BeginFrame`, the stale frame is ended to keep ImGui's
        /// internal state consistent.
        bool m_RenderedThisFrame = true;
    };
}
