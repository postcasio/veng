#pragma once

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Input/InputConsumer.h>

struct ImDrawData;

namespace Veng
{
    class AssetManager;
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
        /// @brief File ImGui persists its layout to; nullopt disables layout persistence.
        ///
        /// `Application` fills this when the caller leaves it unset, with
        /// `UserConfigDir(ApplicationInfo::Name) / "imgui.ini"` — so a layout survives a restart
        /// and lands somewhere the process may actually write. ImGui's own default is the
        /// *relative* path `imgui.ini`, which resolves against the working directory: inside a
        /// macOS application bundle that is the bundle itself, so the app would rewrite its own
        /// sealed contents. Reaching nullopt here therefore means no writable configuration
        /// directory could be resolved, and no layout is persisted rather than one being written
        /// somewhere unpredictable.
        optional<path> IniPath;
    };

    /// @brief Owns the ImGui/imnodes contexts, the renderer that draws ImGui's lists, and the
    ///        offscreen image the UI renders into.
    ///
    /// The renderer is the engine's own: one graphics pipeline per target format over ImGui's
    /// vertex layout, a per-frame-in-flight vertex and index ring, and textures addressed through
    /// the bindless registry, so an `ImTextureID` packs a bindless texture slot and sampler slot.
    /// ImGui's own textures (the dynamic font atlas among them) are created, updated and destroyed
    /// on request as engine `Image`s, their uploads recorded into the frame's command buffer.
    ///
    /// The application opts in by setting `ApplicationInfo::ImGui`; headless and non-UI consumers
    /// never construct one and pay nothing for ImGui at runtime.
    ///
    /// It is an `InputConsumer` the app registers first with the `InputRouter`, so it is offered
    /// every UI-owned event ahead of any later consumer and reads the cursor-capture signal to
    /// suspend its cursor poll while the cursor is captured.
    class VE_API ImGuiLayer : public InputConsumer
    {
    public:
        /// @brief Creates an `ImGuiLayer`, initializing ImGui and its renderer.
        /// @param info     Font configuration.
        /// @param context  Renderer context owning the device.
        /// @param assets   Asset manager the renderer's core shaders load through; it need not
        ///                 outlive the layer.
        /// @param window   GLFW window ImGui reads input from.
        static Unique<ImGuiLayer> Create(const ImGuiLayerInfo& info, Renderer::Context& context,
                                         AssetManager& assets, Window& window);

        /// @brief Destroys the ImGui layer, shutting down ImGui and releasing its textures.
        ~ImGuiLayer() override;

        ImGuiLayer(const ImGuiLayer&) = delete;
        ImGuiLayer& operator=(const ImGuiLayer&) = delete;

        /// @brief Begins a new ImGui frame.
        ///
        /// Call once per frame before building any UI, after the frame's events are forwarded
        /// (ImGui's NewFrame consumes the events ForwardEvent queued into the backend). The first
        /// frame after a run of SkipFrame calls clears ImGui's held key and mouse state, since the
        /// releases that arrived while it ran no frame were discarded.
        void BeginFrame();

        /// @brief Runs no ImGui frame this frame, in place of BeginFrame and Render.
        ///
        /// The frame pays nothing for ImGui: no platform or ImGui NewFrame, no Render, no overlay
        /// pass. HasDrawnOutput reports false, so a compositor blends a transparent overlay, and
        /// ImGui's capture flags read false, so nothing treats the layer as holding the mouse or
        /// keyboard while it is not running. Events forwarded this frame are discarded.
        void SkipFrame();

        /// @brief Whether an ImGui frame is open — BeginFrame has run and Render has not.
        ///
        /// False on a frame SkipFrame stood in for, when no ImGui call may be made.
        /// @return True while UI may be built for this frame.
        [[nodiscard]] bool IsFrameOpen() const { return m_FrameOpen; }

        /// @brief Forwards one window event into the ImGui GLFW backend.
        ///
        /// The engine owns the GLFW callbacks (the backend is initialized with callbacks off),
        /// so the InputRouter calls this to hand ImGui the events routed to the UI layer — and
        /// withholds the ones gameplay focus swallows. Translates the engine event to the
        /// backend's chain-callback so keymap/mods/char handling stay the backend's job. The
        /// overlay is cooperative except over the keyboard it holds: while ImGui wants the keyboard
        /// (a text box or other widget is active), it accepts key presses, repeats and typed
        /// characters, so no Gui field edits with them and the router claims the key against role
        /// navigation. Every other event passes on. The router's snapshot fold is unaffected either
        /// way.
        /// @param event  The event to forward; non-input events relevant to ImGui (focus) included.
        /// @return True for a key event while ImGui wants the keyboard, false otherwise.
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
        ///
        /// Ends the ImGui frame first and records ImGui's pending texture uploads. When the
        /// resulting draw data is empty (no command lists, or no vertices) it records nothing on
        /// the output image — no clear, no store, no layout transition — and the output image keeps
        /// whatever it last held. HasDrawnOutput reports which happened.
        /// @param cmd  Command buffer the render pass is recorded into.
        void Render(Renderer::CommandBuffer& cmd);

        /// @brief Returns whether the last Render drew the UI into the output image.
        ///
        /// False when that frame's draw data was empty: the output image was not written, and a
        /// compositor substitutes a transparent overlay rather than sampling it.
        /// @return True when the output image holds the last rendered frame's UI.
        [[nodiscard]] bool HasDrawnOutput() const { return m_DrewOutput; }

        /// @brief Pushes the active `UI::Theme` into the live ImGui and imnodes styles.
        ///
        /// Called once at construction. A host that swaps the theme at runtime
        /// (`UI::SetTheme`) calls this afterward to refresh the rendered style.
        void ApplyTheme();

        /// @brief Returns the offscreen image the UI is rendered into.
        ///
        /// It holds this frame's UI only when HasDrawnOutput is true.
        [[nodiscard]] Ref<Renderer::Image> GetOutputImage() const { return m_Image; }

        /// @brief Registers a sampler/image pair with the bindless registry and returns an owning wrapper.
        ///
        /// The texture holds its own view of the image, so the image stays alive while the texture
        /// does. The sampler is matched by description to a registry-shared sampler slot.
        /// @param sampler    Sampler the texture is accessed through.
        /// @param imageView  Image view to register; a 2D view.
        Ref<ImGuiTexture> CreateTexture(const Renderer::Sampler& sampler,
                                        const Renderer::ImageView& imageView);

        /// @brief Deregisters a texture, releasing its bindless slot once in-flight frames retire.
        /// @param texture  Texture previously created with `CreateTexture`.
        void DestroyTexture(const ImGuiTexture& texture);

    private:
        /// @brief Internal constructor; called only by `Create`.
        ImGuiLayer(const ImGuiLayerInfo& info, Renderer::Context& context, AssetManager& assets,
                   Window& window);

        /// @brief Allocates the offscreen image.
        void CreateResources();

        /// @brief Releases the offscreen image.
        void ReleaseResources();

        /// @brief The renderer's pipelines, vertex and index rings, and ImGui's own textures.
        struct DrawResources;

        /// @brief Creates, updates and destroys ImGui's own textures as the draw data requests.
        /// @param cmd       Command buffer the uploads are recorded into.
        /// @param drawData  The frame's draw data, whose texture list carries the requests.
        void UpdateTextures(Renderer::CommandBuffer& cmd, const ImDrawData& drawData);

        /// @brief Records the draw data's command lists into the open rendering scope.
        /// @param cmd       Command buffer rendering into the output image.
        /// @param drawData  The frame's non-empty draw data.
        void RecordDrawData(Renderer::CommandBuffer& cmd, const ImDrawData& drawData);

        /// @brief Renderer context providing the device and retire queue.
        Renderer::Context& m_Context;

        /// @brief Borrowed window; its GLFWwindow backs the event forwarders.
        Window& m_Window;

        /// @brief The renderer's state, defined out of line so this header pulls in no renderer.
        Unique<DrawResources> m_Draw;

        /// @brief Offscreen image the UI is rendered into.
        Ref<Renderer::Image> m_Image;
        /// @brief View of `m_Image` the UI renders through.
        Ref<Renderer::ImageView> m_ImageView;

        /// @brief Backing store for `ImGuiIO::IniFilename`.
        ///
        /// ImGui borrows that pointer for the context's lifetime rather than copying the string,
        /// so the characters have to outlive the context; empty when no ini is persisted.
        string m_IniPath;

        /// @brief Tracks whether UI was rendered in the current frame.
        ///
        /// When false at the next `BeginFrame`, the stale frame is ended to keep ImGui's
        /// internal state consistent.
        bool m_RenderedThisFrame = true;

        /// @brief Whether the last Render recorded drawing into the output image.
        bool m_DrewOutput = false;

        /// @brief True between BeginFrame and Render.
        bool m_FrameOpen = false;

        /// @brief Set by SkipFrame; the next BeginFrame clears held input state and resets it.
        bool m_Skipped = false;
    };
}
