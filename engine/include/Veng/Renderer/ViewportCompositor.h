#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/CaptureSink.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportDevices.h>
#include <Veng/Renderer/ViewportRegion.h>

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class Scene;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Viewport;
    class ModelPortraitPool;
    class SceneCapture;
    class SceneCapturePool;
    class SwapChainCompositePass;
    class CompiledGraph;
    struct CaptureSurface;
    struct ModelPortrait;

    /// @brief What one frame's capture pre-pass did across the scenes the viewports will render.
    struct CaptureSurfaceDriveResult
    {
        /// @brief Scenes whose capture surfaces were driven — each once, by its first presenter.
        u32 ScenesDriven = 0;
        /// @brief Capture surfaces driven across those scenes.
        u32 SurfacesDriven = 0;
        /// @brief Captures built new this pass, at most ViewportCompositor::MaxNewCapturesPerFrame.
        u32 CapturesBuilt = 0;
        /// @brief Captures taken from the compositor's pool of released ones instead of building.
        u32 CapturesReused = 0;
        /// @brief Surfaces left unmaterialized and undriven because the build budget was spent.
        u32 SurfacesDeferred = 0;
        /// @brief Disabled surfaces, whose runtime is released and left empty.
        u32 SurfacesDisabled = 0;
    };

    /// @brief What one frame's pre-pass did with the model portraits of the scenes the viewports render.
    struct ModelPortraitDriveResult
    {
        /// @brief Portraits driven across the claimed scenes.
        u32 PortraitsDriven = 0;
        /// @brief Portraits rendered this frame, within the view budget.
        u32 PortraitsRendered = 0;
        /// @brief Renderers built new this pass, sharing ViewportCompositor::MaxNewCapturesPerFrame.
        u32 RenderersBuilt = 0;
        /// @brief Renderers taken from the compositor's pool of released ones instead of building.
        u32 RenderersReused = 0;
        /// @brief Portraits left without a renderer because the build budget was spent.
        u32 PortraitsDeferred = 0;
        /// @brief Disabled portraits, whose runtime is released and left empty.
        u32 PortraitsDisabled = 0;
    };

    /// @brief Renders the registered viewports and composites them to the swapchain each frame.
    ///
    /// Owns the render-order viewport drive-list and the capture drive-list, plus the gather +
    /// swapchain-composite tail (the GatherPass assembling the Presented viewports into one
    /// full-window target and the SwapChainCompositePass placing it behind the ImGui overlay). The
    /// gather runs only when the placements need assembling: a last placement covering the window
    /// is sampled by the composite directly, and no placements composite a black stand-in. A
    /// viewport or capture registers here in registration order, which is render order — a producer
    /// registered before its consumer renders first — and hands a back-reference, so dropping its
    /// owning Unique self-unregisters it. The compositor never owns a viewport or capture; the
    /// drive-lists hold non-owning pointers whose lifetime stays with the caller.
    ///
    /// It also drives the scenes' authored CaptureSurface and ModelPortrait components, from the
    /// viewports that present them (see RenderRegistered), and owns the pools their released
    /// captures and portrait renderers return to.
    ///
    /// Borrows the Context for the swapchain and the per-frame command buffer. The tail is built
    /// only through InitializeTail (present alongside an ImGui overlay); without it RenderRegistered
    /// still renders every viewport and Composite is a no-op, the headless path that reads a
    /// managed viewport's offscreen target back directly.
    class VE_API ViewportCompositor
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
        /// Constructs the GatherPass and SwapChainCompositePass and the 1×1 black-scene and
        /// transparent-overlay stand-ins, compiles the passes' graphs, and registers
        /// a swapchain-invalidation callback that resizes the gather, re-points the composite at the
        /// ImGui layer's recreated offscreen image, re-targets the swapchain on a format/color-space
        /// change, and recompiles both graphs. Called once when an ImGui overlay is present.
        /// @param assets  The asset manager the passes build their pipelines and targets through.
        /// @param imgui   The ImGui overlay the composite draws over.
        void InitializeTail(AssetManager& assets, ImGuiLayer& imgui);

        /// @brief Whether the gather + composite tail exists, so Composite runs and serves capture sinks.
        /// @return True once InitializeTail has run.
        [[nodiscard]] bool HasTail() const { return m_Gather != nullptr; }

        /// @brief Registers a viewport into the render-order drive-list rendered each frame.
        ///
        /// Stores a non-owning pointer in registration order (which is render order), hands the
        /// viewport a back-reference so dropping its owning Unique self-unregisters it, and hands it
        /// the compositor's device engines (SetDevices) for the Gui drivers it drives and, when one
        /// is set, the compositor's look resolver (SetLookResolver).
        /// Double-registering a viewport is a fatal assert.
        /// @param viewport  The viewport to drive; its lifetime stays with the caller.
        void RegisterViewport(Viewport& viewport);

        /// @brief Sets the device engines every registered viewport hands the Gui drivers it drives.
        ///
        /// Applied to every viewport already registered and to every later registration, so a driver
        /// in any viewport on this compositor reaches sound and rumble. The engines are borrowed and
        /// must outlive every drive; pass an empty set to withdraw them.
        /// @param devices  The device engines, any of which may be null.
        void SetDevices(const ViewportDevices& devices);

        /// @brief Returns the device engines handed to every registered viewport.
        [[nodiscard]] const ViewportDevices& GetDevices() const { return m_Devices; }

        /// @brief Sets the look resolver every registered viewport runs over its scene's RenderLook.
        ///
        /// Applied to every viewport already registered and to every later registration, so a host
        /// composing something over each scene's authored look (a player's chosen quality) reaches
        /// every viewport it drives; a viewport wanting another sets its own after registering. An
        /// empty function stops handing one to later registrations and leaves each registered
        /// viewport mapping the look alone.
        /// @param resolver  The resolver, or an empty function.
        void SetLookResolver(LookResolver resolver);

        /// @brief Has every registered viewport resolve its scene's look again at its next render.
        ///
        /// What a host calls when its resolver's own inputs moved (Viewport::InvalidateLook on each).
        void InvalidateLooks();

        /// @brief Registers a scene capture into the drive-list rendered ahead of the viewports.
        ///
        /// Captures render before every viewport, so a material sampling a capture's output reads this
        /// frame's result. The same ownership model as RegisterViewport: the caller keeps the owning
        /// Unique and dropping it self-unregisters. Double-registering a capture is a fatal assert.
        /// @param capture  The capture to drive; its lifetime stays with the caller.
        void RegisterCapture(SceneCapture& capture);

        /// @brief Renders every registered capture then every registered viewport into Sample layout.
        ///
        /// First a capture pre-pass drives the CaptureSurface components of every scene a registered
        /// viewport will render this frame (Viewport::WillRender) — each scene once, by the first such
        /// viewport in registration order, at that viewport's interpolation fraction — and of every
        /// scene a rendering viewport names as its pending destination (Viewport::SetPendingScene), so
        /// a swap presents warm maps. A scene no viewport renders drives nothing. The pre-pass builds
        /// at most MaxNewCapturesPerFrame captures, takes a released capture of a surface's
        /// configuration from the pool first (GetCapturePool), registers each newly materialized
        /// capture, and releases a disabled surface's runtime. Its tally is GetCaptureSurfaceDrive.
        /// The same pass drives each claimed scene's ModelPortrait components: it gives each a
        /// renderer — from GetPortraitPool, or built under the same one-per-frame budget the captures
        /// spend — brings its private scene current, and queues it when it owes a render; its tally
        /// is GetModelPortraitDrive.
        ///
        /// The queued portraits render first, so a Gui driver reading a portrait's output in this
        /// frame's viewport drive paints this frame's render; then the captures (each into its own
        /// target); then every viewport in registration order
        /// (each doing its own Execute + Sample barrier), so every output is in Sample layout before a
        /// later consumer samples it.
        ///
        /// The portraits and the captures spend only the part of the frame's view budget they can
        /// leave the viewports — one slot each is reserved. A portrait the budget cannot seat keeps
        /// its last render and owes another; the captures round-robin across frames when they
        /// outnumber what is left, so a capture set larger than the budget refreshes in turn rather
        /// than the frame failing (see BindlessRegistry::MaxViewsPerFrame).
        /// @param cmd  The command buffer to record into.
        void RenderRegistered(CommandBuffer& cmd);

        /// @brief Gathers the Presented viewports and composites them behind the ImGui overlay.
        ///
        /// When the last Presented viewport's region is the whole window, the composite samples its
        /// output directly — the gather's opaque, full-UV, linear lookup into it would produce the
        /// same pixels — and with no Presented viewport it samples a 1×1 black stand-in; either way
        /// the gather is skipped. Otherwise it rebinds the gather's placement list ({ output, region }
        /// per Presented viewport — rebound only when a viewport's output view identity or region
        /// changed) and runs the gather. The composite's source is re-pointed only when it changes.
        /// When the ImGui layer drew nothing this frame its image is replaced by a 1×1 transparent
        /// stand-in. Then the composite runs into the current swapchain image. A no-op without the
        /// tail (no InitializeTail).
        /// @param cmd  The command buffer to record into.
        void Composite(CommandBuffer& cmd);

        /// @brief Installs a sink the frame is composited into a second time.
        ///
        /// Every Composite asks each installed sink for this frame's target and — when it supplies
        /// one — runs a further SwapChainCompositePass into that image, in the target's colour space
        /// and with or without the application's overlay. Each sink owns its own capture pass, so a
        /// recording and a one-off still capture coexist without rebuilding each other's pass. The
        /// presented composite is untouched, so what the window shows and what a sink receives differ
        /// only in encoding and overlay.
        ///
        /// The compositor never owns a sink: a sink must stay alive while installed. While any sink
        /// is installed the compositor forwards the context's frame retirements to every installed
        /// sink. Adding a sink that is already installed is a no-op.
        /// @param sink  The sink to composite into.
        /// @warning Never call from inside CaptureSink::AcquireTarget — that runs mid-composite. A sink
        ///          may add or remove sinks from its OnSlotRetired.
        void AddCaptureSink(CaptureSink& sink);

        /// @brief Removes an installed sink and releases its capture pass and cached target views.
        ///
        /// Removing a sink that is not installed is a no-op. A sink removed during a frame
        /// retirement's dispatch receives no further OnSlotRetired, including in that dispatch.
        /// @param sink  The sink to remove.
        /// @warning Never call from inside CaptureSink::AcquireTarget — that runs mid-composite.
        void RemoveCaptureSink(CaptureSink& sink);

        /// @brief Whether @p sink is currently installed.
        /// @param sink  The sink to look for.
        /// @return True while @p sink is installed.
        [[nodiscard]] bool HasCaptureSink(const CaptureSink& sink) const;

        /// @brief The most captures one frame's pre-pass builds new; a surface past it waits a frame.
        ///
        /// Building a capture builds a whole face renderer, so a scene presented for the first time
        /// with several capture surfaces would otherwise pay for all of them in one frame. A surface
        /// handed a released capture from the pool builds nothing and is not counted.
        static constexpr u32 MaxNewCapturesPerFrame = 1;

        /// @brief The view slots this frame's renders want kept from work recorded ahead of them.
        ///
        /// Every registered viewport's claim (two for one whose half-res translucent layer is
        /// active), plus one per registered capture and per portrait driven last frame, that share
        /// capped at half of what the viewports leave. GeneratedTextureService defers a
        /// view-claiming tick that would cut into it (GeneratedTextureFrameHint::ViewReserve).
        [[nodiscard]] u32 GetBackgroundViewReserve() const;

        /// @brief Returns what the last RenderRegistered's capture pre-pass did.
        [[nodiscard]] const CaptureSurfaceDriveResult& GetCaptureSurfaceDrive() const
        {
            return m_CaptureSurfaceDrive;
        }

        /// @brief Returns what the last RenderRegistered's pre-pass did with the model portraits.
        [[nodiscard]] const ModelPortraitDriveResult& GetModelPortraitDrive() const
        {
            return m_PortraitDrive;
        }

        /// @brief Returns the pool a destroyed or disabled portrait's renderer returns to.
        [[nodiscard]] ModelPortraitPool& GetPortraitPool() const { return *m_PortraitPool; }

        /// @brief Returns the pool a destroyed or disabled surface's capture returns to.
        ///
        /// A capture surface the pre-pass materialized hands its capture here when it goes, and the
        /// next surface asking for the same configuration takes it before any build is considered.
        [[nodiscard]] SceneCapturePool& GetCapturePool() const { return *m_CapturePool; }

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
        /// @brief Drives the capture surfaces of the scenes the viewports will render this frame.
        ///
        /// Claims each scene once, for the first viewport that will render it or names it pending
        /// (ClaimCaptureScenes), then drives every surface in each claimed scene. Records its tally in
        /// m_CaptureSurfaceDrive.
        void DriveCaptureSurfaces();

        /// @brief Drives every CaptureSurface in one scene, materializing and registering as needed.
        /// @param scene   The claimed scene, taken mutably for the surfaces' material clones.
        /// @param alpha   The interpolation fraction its claiming viewport draws it at.
        /// @param assets  The asset manager of the claiming viewport.
        /// @param built   The captures this pass has built so far; incremented on a build.
        void DriveSceneCaptures(Scene& scene, f32 alpha, AssetManager& assets, u32& built);

        /// @brief Drives every ModelPortrait in one scene, materializing and queueing as needed.
        /// @param scene   The claimed scene.
        /// @param assets  The asset manager of the claiming viewport.
        /// @param built   The captures and portrait renderers this pass has built so far.
        void DriveScenePortraits(Scene& scene, AssetManager& assets, u32& built);

        /// @brief Gives a portrait with no renderer one, from the pool or newly built.
        /// @param portrait  The portrait to materialize.
        /// @param assets    The asset manager the renderer is built against.
        /// @param built     The builds this pass has spent; incremented on a build.
        /// @return True when the portrait now holds a renderer; false when the build budget is spent.
        bool MaterializePortrait(const ModelPortrait& portrait, AssetManager& assets, u32& built);

        /// @brief Renders the queued portraits against the view budget left over from the viewports.
        /// @param cmd  The command buffer to record into.
        void DrivePortraits(CommandBuffer& cmd);

        /// @brief Gives an unmaterialized surface its capture, from the pool or newly built.
        ///
        /// A pooled capture of the surface's configuration is taken first and costs no build; past
        /// that, a capture is built only while @p built is under MaxNewCapturesPerFrame.
        /// @param surface  The surface to materialize; it holds no capture yet.
        /// @param assets   The asset manager the capture is built against.
        /// @param built    The captures this pass has built so far; incremented on a build.
        /// @return True when the surface now holds a capture; false when the build budget is spent.
        bool MaterializeCapture(const CaptureSurface& surface, AssetManager& assets, u32& built);

        /// @brief Renders the registered captures against the view budget left over from the viewports.
        ///
        /// Reserves the viewports' view slots (ReservedViewSlots), then drives captures round-robin from
        /// m_CaptureCursor while the registry has a slot to spare, leaving the cursor on the first
        /// capture it could not afford so the next frame resumes there. Warns once per compositor when
        /// a frame cannot drive them all.
        /// @param cmd  The command buffer to record into.
        void DriveCaptures(CommandBuffer& cmd);

        /// @brief The view slots the registered viewports' renders claim this frame, which the
        ///        portrait and capture drives leave for them: one each, two for a viewport whose
        ///        half-res translucent layer is active.
        [[nodiscard]] u32 ReservedViewSlots() const;

        /// @brief Borrowed render context for the swapchain and per-frame command buffer.
        Context& m_Context;

        /// @brief Non-owning, ordered list of viewports rendered each frame; registration order is render order.
        vector<Viewport*> m_Viewports;

        /// @brief The device engines handed to every registered viewport.
        ViewportDevices m_Devices;

        /// @brief The look resolver handed to every registered viewport; empty hands none.
        LookResolver m_LookResolver;

        /// @brief Non-owning, ordered list of scene captures rendered ahead of the viewports.
        vector<SceneCapture*> m_Captures;

        /// @brief Released scene captures held for reuse by a surface of the same configuration.
        ///
        /// Shared because each surface holds it weakly, so a surface outliving the compositor drops
        /// its capture instead of returning it to a dead pool.
        Ref<SceneCapturePool> m_CapturePool;

        /// @brief The last capture pre-pass's tally.
        CaptureSurfaceDriveResult m_CaptureSurfaceDrive;

        /// @brief Released portrait renderers held for reuse by a portrait of the same configuration.
        ///
        /// Shared because each portrait holds it weakly, as the capture pool is.
        Ref<ModelPortraitPool> m_PortraitPool;

        /// @brief The last pre-pass's portrait tally.
        ModelPortraitDriveResult m_PortraitDrive;

        /// @brief The portraits the pre-pass queued to render this frame; valid only within it.
        vector<const ModelPortrait*> m_PortraitQueue;

        /// @brief Latch for the spent-portrait-budget warning, so it is logged once per compositor.
        bool m_WarnedPortraitBudget = false;

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

        /// @brief One installed sink and the capture pass that writes its targets.
        struct CaptureSinkSlot
        {
            /// @brief The installed sink. Borrowed, never owned.
            CaptureSink* Sink = nullptr;

            /// @brief The composite writing this sink's targets; built on the first target taken.
            Unique<SwapChainCompositePass> Composite;

            /// @brief Compiled capture-composite graph, re-Compile()d whenever the pass is rebuilt.
            Unique<CompiledGraph> Graph;

            /// @brief Whether the built pass blends the overlay; rebuilt when the sink's choice changes.
            bool IncludesOverlay = false;

            /// @brief Target format the pass is currently targeted at.
            Format TargetFormat = Format::Undefined;

            /// @brief Colour space the pass is currently encoding for.
            DisplayColorSpace ColorSpace = DisplayColorSpace::SrgbNonlinear;

            /// @brief Views of the sink's target images, keyed by image, so a recycled target is
            ///        viewed once rather than every frame it comes back around.
            map<const Image*, Ref<ImageView>> Views;
        };

        /// @brief Runs each installed sink's capture composite into its target for this frame.
        ///
        /// Asks every sink for a target, lazily builds or rebuilds its pass to match the target's
        /// overlay choice, re-targets it on a format or colour-space change, and executes it against
        /// a view of the target image. A target whose extent differs from the presented extent is
        /// refused rather than stretched into.
        /// @param cmd  The command buffer to record into.
        void CompositeToSinks(CommandBuffer& cmd);

        /// @brief Runs one sink's capture composite into the target it supplies for this frame.
        /// @param cmd   The command buffer to record into.
        /// @param slot  The installed sink and its pass.
        void CompositeToSink(CommandBuffer& cmd, CaptureSinkSlot& slot);

        /// @brief Builds @p slot's capture composite pass and its compiled graph.
        ///
        /// Sourced from the scene and overlay the presented composite currently reads, and kept on
        /// them as they change (ApplySceneSource, ApplyOverlaySource), so every composite writes the
        /// identical frame.
        /// @param slot            The sink whose pass is built.
        /// @param includeOverlay  Whether the pass blends the application's overlay.
        /// @param format          Colour format of the targets it will render into.
        /// @param colorSpace      Colour space the pass encodes for.
        void BuildCapturePass(CaptureSinkSlot& slot, bool includeOverlay, Format format,
                              DisplayColorSpace colorSpace);

        /// @brief Forwards a retired frame slot to every sink still installed when its turn comes.
        /// @param frameSlot  The frame-in-flight slot that retired.
        void DispatchSlotRetired(u32 frameSlot);

        /// @brief Points the presented and capture composites at @p source, if it is not already.
        /// @param source  The view the composites sample as the scene.
        void ApplySceneSource(const Ref<ImageView>& source);

        /// @brief Swaps the composites' overlay between the ImGui layer and the transparent stand-in.
        ///
        /// Re-registers the overlay only when the choice changes.
        /// @param transparent  True to blend the transparent stand-in, false to blend the layer.
        void ApplyOverlaySource(bool transparent);

        /// @brief The view the composites currently sample as the scene.
        ///
        /// The gather's output, the covering viewport's output, or the black stand-in.
        Ref<ImageView> m_SceneSource;

        /// @brief A 1×1 opaque black image: the scene when there are no Presented viewports.
        Ref<ImageView> m_BlackView;

        /// @brief A 1×1 transparent image: the overlay when the ImGui layer drew nothing.
        Ref<ImageView> m_TransparentView;

        /// @brief Whether the composites currently blend the transparent stand-in as the overlay.
        bool m_OverlayTransparent = false;

        /// @brief Last placement list pushed to the gather; rebinds only when it changes.
        ///
        /// Guards against per-frame bindless churn: the gather's slots are re-registered only on a
        /// frame where a Presented viewport's output view identity or region differs from this.
        /// Emptied, with the gather's own list, on a frame that skips the gather.
        vector<CompositePlacement> m_GatheredPlacements;

        /// @brief The asset manager the tail's passes build through; null until InitializeTail.
        AssetManager* m_Assets = nullptr;

        /// @brief The ImGui overlay the tail composites over; null until InitializeTail.
        ImGuiLayer* m_ImGui = nullptr;

        /// @brief The installed capture sinks, in installation order, each with its own pass.
        ///
        /// Held by pointer so a slot keeps its address while the vector grows or shrinks.
        vector<Unique<CaptureSinkSlot>> m_CaptureSinks;

        /// @brief Handle of the frame-retired callback forwarded to the sinks; 0 when none is installed.
        Context::FrameRetiredHandle m_CaptureRetiredHandle = 0;
    };
}
