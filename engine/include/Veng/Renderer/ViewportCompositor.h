#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/CaptureSink.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/ViewportRegion.h>

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Viewport;
    class SceneCapture;
    class SwapChainCompositePass;
    class CompiledGraph;

    /// @brief Renders the registered viewports and composites them to the swapchain each frame.
    ///
    /// Owns the render-order viewport drive-list and the capture drive-list, plus the gather +
    /// swapchain-composite tail (the GatherPass assembling the Presented viewports into one
    /// full-window target and the SwapChainCompositePass placing it behind the ImGui overlay). A
    /// viewport or capture registers here in registration order, which is render order — a producer
    /// registered before its consumer renders first — and hands a back-reference, so dropping its
    /// owning Unique self-unregisters it. The compositor never owns a viewport or capture; the
    /// drive-lists hold non-owning pointers whose lifetime stays with the caller.
    ///
    /// Borrows the Context for the swapchain and the per-frame command buffer. The tail is built
    /// only through InitializeTail (present alongside an ImGui overlay); without it RenderRegistered
    /// still renders every viewport and Composite is a no-op, the headless path that reads a
    /// managed viewport's offscreen target back directly.
    class ViewportCompositor
    {
    public:
        /// @brief Constructs a compositor borrowing @p context; the gather + composite tail stays off until InitializeTail.
        ///
        /// @param context  The render context, borrowed for the swapchain and command buffer; must outlive the compositor.
        explicit ViewportCompositor(Context& context);

        /// @brief Destroys the compositor, releasing its gather + composite tail.
        ///
        /// Resets the gather, composite, and compiled graphs and clears the placement cache (which
        /// retains a Ref to each Presented viewport's output view), so the outputs retire once their
        /// viewports drop rather than outliving the context's allocator; the context it borrows must
        /// still be live. The drive-lists are non-owning, so no registered viewport or capture is
        /// destroyed; each self-unregisters when its own owner drops it.
        ~ViewportCompositor();

        ViewportCompositor(const ViewportCompositor&) = delete;
        ViewportCompositor& operator=(const ViewportCompositor&) = delete;

        /// @brief Builds the gather pass and swapchain composite tail, wiring the resize re-target.
        ///
        /// Constructs the GatherPass and SwapChainCompositePass, compiles their graphs, and registers
        /// a swapchain-invalidation callback that resizes the gather, re-points the composite at the
        /// ImGui layer's recreated offscreen image, re-targets the swapchain on a format/color-space
        /// change, and recompiles both graphs. Called once when an ImGui overlay is present.
        /// @param assets  The asset manager the passes build their pipelines and targets through.
        /// @param imgui   The ImGui overlay the composite draws over.
        void InitializeTail(AssetManager& assets, ImGuiLayer& imgui);

        /// @brief Registers a viewport into the render-order drive-list rendered each frame.
        ///
        /// Stores a non-owning pointer in registration order (which is render order) and hands the
        /// viewport a back-reference so dropping its owning Unique self-unregisters it. Double-registering
        /// a viewport is a fatal assert.
        /// @param viewport  The viewport to drive; its lifetime stays with the caller.
        void RegisterViewport(Viewport& viewport);

        /// @brief Registers a scene capture into the drive-list rendered ahead of the viewports.
        ///
        /// Captures render before every viewport, so a material sampling a capture's output reads this
        /// frame's result. The same ownership model as RegisterViewport: the caller keeps the owning
        /// Unique and dropping it self-unregisters. Double-registering a capture is a fatal assert.
        /// @param capture  The capture to drive; its lifetime stays with the caller.
        void RegisterCapture(SceneCapture& capture);

        /// @brief Renders every registered capture then every registered viewport into Sample layout.
        ///
        /// Captures render first (each into its own target), then every viewport in registration order
        /// (each doing its own Execute + Sample barrier), so every output is in Sample layout before a
        /// later consumer samples it.
        ///
        /// The capture drive spends only the part of the frame's view budget it can leave the
        /// viewports — one slot each is reserved — and round-robins across frames when the captures
        /// outnumber what is left, so a capture set larger than the budget refreshes in turn rather
        /// than the frame failing (see BindlessRegistry::MaxViewsPerFrame).
        /// @param cmd  The command buffer to record into.
        void RenderRegistered(CommandBuffer& cmd);

        /// @brief Gathers the Presented viewports and composites them behind the ImGui overlay.
        ///
        /// Rebinds the gather's placement list ({ output, region } per Presented viewport — rebound
        /// only when a viewport's output view identity or region changed), runs the gather, then the
        /// composite into the current swapchain image. A no-op without the tail (no InitializeTail).
        /// @param cmd  The command buffer to record into.
        void Composite(CommandBuffer& cmd);

        /// @brief Installs the sink the frame is composited into a second time, or clears it.
        ///
        /// With a sink set, every Composite asks it for this frame's target and — when it supplies
        /// one — runs a second SwapChainCompositePass into that image, in the target's colour space
        /// and with or without the application's overlay. The presented composite is untouched, so
        /// what the window shows and what the sink receives differ only in encoding and overlay.
        ///
        /// The compositor never owns the sink: a sink must outlive its installation. Setting a sink
        /// registers a frame-retired callback on the context and forwards it to the sink; clearing
        /// one removes that callback and releases the capture pass and its cached target views.
        /// Passing the sink that is already installed is a no-op.
        /// @param sink  The sink to composite into, or null to clear.
        void SetCaptureSink(CaptureSink* sink);

        /// @brief Returns the render-order viewport drive-list.
        ///
        /// The registration-order list of every driven viewport; a consumer walks it to resolve the
        /// viewport presenting a given scene or to route input into a hosted document.
        /// @return The non-owning viewport drive-list.
        [[nodiscard]] const vector<Viewport*>& GetViewports() const { return m_Viewports; }

        /// @brief Resolves a normalized window Layout to a pixel region against the current render extent.
        ///
        /// round(Layout · Context::GetRenderExtent()): the swapchain framebuffer extent windowed
        /// (larger than the logical window on a HiDPI display), the ContextInfo::HeadlessExtent
        /// headless. The single layout→pixel path a window-tracking viewport's region resolves
        /// through, so its initial region and its resize tracking agree.
        /// @param layout  The normalized window placement to resolve.
        /// @return The pixel region for the layout at the current render extent.
        [[nodiscard]] ViewportRegion ResolveLayout(const ViewportLayout& layout) const;

        /// @brief Re-resolves the region and UI scale of every registered window-tracking viewport.
        ///
        /// Iterates the drive-list; for each viewport carrying a Layout (Viewport::GetLayout),
        /// re-resolves its region (ResolveLayout, SetRegion debouncing the SceneRenderer::Resize to
        /// the next Render) and re-stamps its UI scale from the window's content scale (1.0 headless).
        /// A viewport with an absolute region is untouched. Called at registration and as the
        /// swapchain-invalidation reaction, so window-tracking viewports need no per-frame re-apply.
        void ResolveTrackingLayouts();

    private:
        /// @brief Renders the registered captures against the view budget left over from the viewports.
        ///
        /// Reserves one view slot per registered viewport, then drives captures round-robin from
        /// m_CaptureCursor while the registry has a slot to spare, leaving the cursor on the first
        /// capture it could not afford so the next frame resumes there. Warns once per compositor when
        /// a frame cannot drive them all.
        /// @param cmd  The command buffer to record into.
        void DriveCaptures(CommandBuffer& cmd);

        /// @brief Borrowed render context for the swapchain and per-frame command buffer.
        Context& m_Context;

        /// @brief Non-owning, ordered list of viewports rendered each frame; registration order is render order.
        vector<Viewport*> m_Viewports;

        /// @brief Non-owning, ordered list of scene captures rendered ahead of the viewports.
        vector<SceneCapture*> m_Captures;

        /// @brief Index into m_Captures the next frame's bounded capture drive resumes at.
        usize m_CaptureCursor = 0;

        /// @brief Latch for the spent-capture-budget warning, so it is logged once per compositor.
        bool m_WarnedCaptureBudget = false;

        /// @brief The gather pass assembling the Presented viewports; present only with the tail.
        Unique<GatherPass> m_Gather;

        /// @brief The swapchain composite tail; present only with the tail.
        Unique<SwapChainCompositePass> m_Composite;

        /// @brief Compiled gather graph, re-Compile()d on swapchain resize.
        Unique<CompiledGraph> m_GatherGraph;

        /// @brief Compiled composite graph, re-Compile()d on swapchain resize.
        Unique<CompiledGraph> m_CompositeGraph;

        /// @brief Runs the capture composite into the sink's target for this frame, if it wants one.
        ///
        /// Asks the sink for a target, lazily builds or rebuilds the capture pass to match the
        /// target's overlay choice, re-targets it on a format or colour-space change, and executes
        /// it against a view of the target image. A target whose extent differs from the presented
        /// extent is refused rather than stretched into.
        /// @param cmd  The command buffer to record into.
        void CompositeToSink(CommandBuffer& cmd);

        /// @brief Builds the capture composite pass and its compiled graph for @p includeOverlay.
        ///
        /// Sourced from the same gather output the presented composite reads, so both composite the
        /// identical assembled frame.
        /// @param includeOverlay  Whether the pass blends the application's overlay.
        /// @param format          Colour format of the targets it will render into.
        /// @param colorSpace      Colour space the pass encodes for.
        void BuildCapturePass(bool includeOverlay, Format format, DisplayColorSpace colorSpace);

        /// @brief Releases the capture composite, its graph, and the cached target views.
        void ReleaseCapturePass();

        /// @brief Last placement list pushed to the gather; rebinds only when it changes.
        ///
        /// Guards against per-frame bindless churn: the gather's slots are re-registered only on a
        /// frame where a Presented viewport's output view identity or region differs from this.
        vector<CompositePlacement> m_GatheredPlacements;

        /// @brief The asset manager the tail's passes build through; null until InitializeTail.
        AssetManager* m_Assets = nullptr;

        /// @brief The ImGui overlay the tail composites over; null until InitializeTail.
        ImGuiLayer* m_ImGui = nullptr;

        /// @brief The installed capture sink, or null. Borrowed, never owned.
        CaptureSink* m_CaptureSink = nullptr;

        /// @brief Handle of the frame-retired callback forwarded to the sink; 0 when none.
        Context::FrameRetiredHandle m_CaptureRetiredHandle = 0;

        /// @brief The second composite writing the sink's targets; built on the first target taken.
        Unique<SwapChainCompositePass> m_CaptureComposite;

        /// @brief Compiled capture-composite graph, re-Compile()d whenever the pass is rebuilt.
        Unique<CompiledGraph> m_CaptureGraph;

        /// @brief Whether the built capture pass blends the overlay; rebuilt when the sink's
        ///        choice changes.
        bool m_CaptureIncludesOverlay = false;

        /// @brief Target format the capture pass is currently targeted at.
        Format m_CaptureFormat = Format::Undefined;

        /// @brief Colour space the capture pass is currently encoding for.
        DisplayColorSpace m_CaptureColorSpace = DisplayColorSpace::SrgbNonlinear;

        /// @brief Views of the sink's target images, keyed by image, so a recycled target is viewed
        ///        once rather than every frame it comes back around.
        map<const Image*, Ref<ImageView>> m_CaptureViews;
    };
}
