#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class AssetManager;
    class GuiDriverRegistry;
    class MaterialInstance;
    class Scene;

    namespace Gui
    {
        class Document;
        class DocumentHost;
        class BindingContext;
        class DrawList;
    }

    namespace Renderer
    {
        class Viewport;
    }

    /// @brief Runtime document state a GuiOverlay materializes lazily; defined in Overlay.cpp.
    struct GuiOverlayRuntime;

    /// @brief Where in the compositing chain an overlay's document is drawn.
    enum class GuiOverlayPlacement : u8
    {
        /// @brief Composited after tonemap, over the final LDR image (the default, un-bloomed).
        PostTonemap,
        /// @brief Blended into the scene HDR at the pre-bloom tail anchor, so it blooms at output resolution.
        SceneHdrPreBloom,
    };

    /// @brief How an overlay's document maps into the target it draws on.
    enum class GuiOverlayProjection : u8
    {
        /// @brief The flat screen-space placement: logical points magnified 1:1 by the UI scale (the default).
        ScreenSpace,
        /// @brief Textured onto a flat virtual plane at a static world transform, projected through the live camera.
        WorldAnchored,
    };

    /// @brief A Gui::Document presented on a viewport's screen-space layer stack.
    ///
    /// A reflected scene component authored on an entity, the screen-space peer of GuiSurface: where
    /// GuiSurface maps a document onto a world mesh (HDR, in-scene, glowing through bloom), a
    /// GuiOverlay attaches its document to the presenting viewport's layer stack (LDR, composited
    /// after tonemap, un-bloomed). The Viewport discovers every GuiOverlay in the scene it renders
    /// (its View<GuiOverlay>() loop) and drives the ones it claims (by seat, see TargetSeat), so the
    /// engine owns the load, instantiate, and attach the consumer otherwise hand-rolls; the game
    /// owns only the data binding, through the DocumentHost/BindingContext surface.
    ///
    /// The document lifecycle (a Gui::DocumentHost) and its screen presenter (a Gui::DocumentLayer)
    /// are materialized on the first Drive, which needs the asset manager the driving viewport
    /// supplies. A binding is deferrable before that first Drive: SetContext and SetOnInstantiate are
    /// stored and applied to the host when it is created, so a binding system that runs ahead of the
    /// first render has no ordering hole. The document data-binds like any other (`{obj.field}`);
    /// Interactive gates whether it takes input, and a system may flip it at runtime.
    ///
    /// The runtime is materialized on the first Drive; a component that never drives allocates none.
    struct GuiOverlay
    {
        /// @brief Default-constructs an undriven overlay (its runtime is empty until Drive).
        GuiOverlay();
        /// @brief Releases the runtime host and detaches the presented document from its viewport.
        ~GuiOverlay();
        /// @brief Move-constructs, transferring the runtime state.
        GuiOverlay(GuiOverlay&&) noexcept;
        /// @brief Move-assigns, transferring the runtime state.
        GuiOverlay& operator=(GuiOverlay&&) noexcept;

        GuiOverlay(const GuiOverlay&) = delete;
        GuiOverlay& operator=(const GuiOverlay&) = delete;

        /// @brief The cooked UI document this entity presents on the viewport layer stack.
        AssetHandle<Gui::UIDocument> Document;

        /// @brief The document's z-order in the viewport layer stack; higher composites over lower.
        i32 Layer = 0;

        /// @brief The presentation driver instantiated with this overlay's document; Null = undriven.
        ///
        /// Names a driver in the host-owned GuiDriverRegistry (see Veng/Gui/Driver.h). When set and
        /// the registry resolves it, Drive instantiates the driver on the first drive, owns it for the
        /// runtime's lifetime, re-runs OnInstantiate on any document re-instantiate, and calls
        /// OnUpdate each drive — the engine's per-instance path for a HUD's data binding. Null (the
        /// default) leaves the overlay undriven; a consumer binds it through SetContext instead.
        GuiDriverId Driver = GuiDriverId::Null;

        /// @brief Whether the document takes input, or is display-only.
        ///
        /// False (the default) leaves the overlay display-only: it data-binds and draws but hit-tests
        /// nothing and takes no focus. True routes the claiming viewport's seat input into the
        /// document. A system may flip it at runtime; the next Drive reapplies the change.
        bool Interactive = false;

        /// @brief The seat whose viewport presents this overlay under multi-viewport presentation.
        ///
        /// A scene presented by more than one viewport (split-screen) resolves which viewport claims
        /// this overlay by seat: a viewport claims the overlays whose target seat is its own bound
        /// seat (Viewport::GetSeat). The target seat is the entity's own seat when the GuiOverlay
        /// sits on a Viewer entity; else this TargetSeat; else unbound, in which case the sole (or
        /// primary) presenting viewport claims it. Entity::Null — the default — is the unbound case,
        /// which is every single-viewport HUD. The reference remaps on prefab spawn like any Entity
        /// reference.
        Entity TargetSeat = Entity::Null;

        /// @brief Where this overlay composites: after tonemap (LDR, the default) or in the scene HDR pre-bloom.
        ///
        /// PostTonemap is today's behavior — the document is attached to the viewport's layer stack
        /// and blended over the final image. SceneHdrPreBloom blends the document into the scene HDR
        /// at the pre-bloom tail anchor instead, so it is tonemapped and blooms with the scene at
        /// output resolution; such an overlay never joins the layer stack.
        GuiOverlayPlacement Placement = GuiOverlayPlacement::PostTonemap;

        /// @brief An optional material this overlay's document is composited through (SceneHdrPreBloom only).
        ///
        /// Null (the default, an empty handle) composites the document straight into scene HDR — the
        /// direct blend every overlay took before, byte-for-byte. When set, a SceneHdrPreBloom overlay
        /// is instead composited through the named MaterialInstance: the engine renders the overlay's
        /// document to an intermediate HDR target and runs the material as a fullscreen composite that
        /// samples it, writing the shaped color into scene HDR and, when the material declares a bloom
        /// mask, an amplitude into the renderer's bloom-mask target — so the document can bloom by a
        /// strength it names rather than by how bright it is, decoupled from its drawn luminance.
        ///
        /// The material is a PostProcess-domain MaterialInstance whose fragment samples the rendered
        /// document through a runtime-bound `Document` texture handle (written by the composite each
        /// frame, the PostProcess `Scene`-handle convention) by integer pixel coordinate — the
        /// document is rasterized at the composite resolution, so it reads 1:1. A material declaring
        /// `"bloomMask": true` additionally returns a float SV_Target1 amplitude. Ignored for a
        /// PostTonemap overlay, which never reaches the pre-bloom composite.
        AssetHandle<MaterialInstance> Material;

        /// @brief How this overlay maps into its target: flat screen-space (the default) or world-anchored.
        ///
        /// ScreenSpace is the flat placement — logical points at ScreenSpace's UI scale, the overlay's
        /// existing screen mapping. WorldAnchored textures the document onto a flat virtual plane
        /// posed by AnchorPosition/AnchorRotation (composed onto the carrying entity's world
        /// transform) at SurfaceSize world units and laid out at SurfaceResolution logical points,
        /// projected through the live camera so it foreshortens and slides under look-around like a
        /// real surface — and rides the entity it is authored on. Only SceneHdrPreBloom honors
        /// WorldAnchored.
        GuiOverlayProjection Projection = GuiOverlayProjection::ScreenSpace;

        /// @brief The virtual plane's center, offset from the carrying entity's pose (WorldAnchored only).
        ///
        /// Composed onto the entity's world transform, so it is an entity-local offset — an overlay
        /// authored on a moving entity rides that entity. An entity with no Transform contributes
        /// identity, so on a bare entity this is a plain world-space position.
        vec3 AnchorPosition{0.0f, 0.0f, -1.0f};

        /// @brief The virtual plane's orientation, relative to the carrying entity's pose (WorldAnchored only).
        ///
        /// Composed onto the entity's world transform like AnchorPosition; identity on a bare entity
        /// leaves the plane in world orientation.
        quat AnchorRotation{1.0f, 0.0f, 0.0f, 0.0f};

        /// @brief The virtual plane's world-space width and height, in world units (WorldAnchored only).
        vec2 SurfaceSize{1.0f, 1.0f};

        /// @brief Logical-point extent the document lays out at for a world-anchored plane.
        ///
        /// Splits the two jobs one extent would otherwise do: the document lays out at
        /// SurfaceResolution logical points while the plane occupies SurfaceSize world units, so a
        /// larger world plane does not shrink the text. Ignored for a ScreenSpace overlay, which lays
        /// out at the viewport region divided by the UI scale.
        uvec2 SurfaceResolution{512, 512};

        /// @brief Runtime document state (host, presenter, deferred binding); empty until first use.
        ///
        /// Materialized on the first SetContext/SetOnInstantiate (to hold the deferred binding) or the
        /// first Drive (which additionally builds the GPU-free document host, needing the driving
        /// viewport's asset manager). Public so the component stays standard-layout for reflection.
        mutable Unique<GuiOverlayRuntime> Runtime;

        /// @brief Binds the context the document's `{obj.field}` bindings resolve against.
        ///
        /// The game's view-model, given as a Gui::BindingContext (a reflected data object plus a
        /// handler table). Callable before the first Drive: the context is stored and applied to the
        /// host on instantiate — a deferred bind with no ordering hole — and forwarded immediately
        /// when the host is already live. The context is borrowed; it must outlive the binding (or be
        /// cleared with nullptr first).
        /// @param context  The game-owned binding context, or nullptr to clear.
        void SetContext(Gui::BindingContext* context);

        /// @brief Sets the callback run after the document is instantiated and bound.
        ///
        /// Where a consumer resolves element pointers and does one-time setup; it runs once on the
        /// lazy instantiate and re-runs on any re-instantiate, so cached pointers stay correct.
        /// Callable before the first Drive (stored and applied on instantiate) and forwarded
        /// immediately when the document is already live. An empty function clears the callback.
        /// @param callback  The callback receiving the live document, or an empty function to clear.
        void SetOnInstantiate(function<void(Gui::Document&)> callback);

        /// @brief Returns the live document, or nullptr before the first Drive (or a failed load).
        [[nodiscard]] Gui::Document* GetDocument() const;

        /// @brief Returns the resident composite material instance, or nullptr when none is set or resident.
        ///
        /// The material named by Material, loaded (LoadSync) into the runtime on the first DriveHdr and
        /// cached there. Null when Material names nothing, before the first DriveHdr, or on a failed
        /// load — in which case a SceneHdrPreBloom overlay takes the direct-composite path.
        [[nodiscard]] MaterialInstance* GetCompositeMaterial() const;

        /// @brief Returns the runtime document host, or nullptr before the first Drive materializes it.
        ///
        /// The host is created on the first Drive (it needs the driving viewport's asset manager), so
        /// this is null until then; bind through SetContext/SetOnInstantiate, which are callable
        /// before the host exists.
        [[nodiscard]] Gui::DocumentHost* GetHost() const;

        /// @brief Drives the overlay onto the viewport's layer stack (the Viewport's per-frame call).
        ///
        /// Materializes the host + layer on first use (instantiating the Document recipe and applying
        /// any deferred binding), reapplies Interactive when it changed, and presents the live
        /// document on @p viewport — attaching it to the layer stack at Layer, re-attaching across a
        /// document recreation, and reapplying the interactive flag on a fresh attach. Only the
        /// viewport that claims this overlay calls Drive, so the document never thrashes between
        /// viewports. A failed document load is logged once and leaves the overlay silent — a
        /// recoverable miss, never an abort.
        ///
        /// When Driver is set and @p drivers resolves it, the driver is instantiated on the first
        /// drive (owned in the runtime, destroyed with it), its OnInstantiate re-run whenever the
        /// document (re)instantiates, and its OnUpdate called each drive with the claiming viewport's
        /// real view. An unresolved or Null Driver leaves the overlay undriven.
        /// @param viewport  The claiming viewport to present the document on.
        /// @param assets    The asset manager the document recipe and its fonts load through.
        /// @param scene     The presented scene the overlay lives in, handed to the driver.
        /// @param owner     The entity carrying this overlay, handed to the driver as its instance.
        /// @param drivers   The driver catalog the Driver id resolves against, or nullptr (undriven).
        /// @param audio     The audio engine handed to the driver's frame, or nullptr (silent).
        void Drive(Renderer::Viewport& viewport, AssetManager& assets, Scene& scene, Entity owner,
                   GuiDriverRegistry* drivers, Audio::AudioEngine* audio = nullptr) const;

        /// @brief Drives the overlay's document and builds its geometry into a draw list, off the layer stack.
        ///
        /// The SceneHdrPreBloom path, the counterpart to Drive: it materializes the host, refreshes
        /// bindings, runs the GuiDriver (OnInstantiate on a re-instantiate, OnUpdate each frame) and
        /// the document's component drivers, then lays the document out at @p docExtent logical points
        /// and builds it into @p out. It never attaches the document to a viewport layer stack — the
        /// engine conveys @p out into the pre-bloom pass instead — so an HDR overlay leaves the
        /// post-tonemap composite untouched. A failed document load leaves @p out empty.
        /// @param viewport  The claiming viewport, read for the driver frame (seat, camera, region).
        /// @param assets    The asset manager the document recipe and its fonts load through.
        /// @param scene     The presented scene the overlay lives in, handed to the driver.
        /// @param owner     The entity carrying this overlay, handed to the driver as its instance.
        /// @param drivers   The driver catalog the Driver id resolves against, or nullptr (undriven).
        /// @param audio     The audio engine handed to the driver's frame, or nullptr (silent).
        /// @param docExtent The logical-point extent to lay the document out at.
        /// @param delta     Frame delta seconds advanced into the document's animation clock.
        /// @param out       The draw list the built geometry is appended into (cleared first).
        void DriveHdr(Renderer::Viewport& viewport, AssetManager& assets, Scene& scene,
                      Entity owner, GuiDriverRegistry* drivers, Audio::AudioEngine* audio,
                      vec2 docExtent, f32 delta, Gui::DrawList& out) const;

        /// @brief Detaches the presented document from a viewport's layer stack — the inverse of Drive.
        ///
        /// Removes the live document from @p viewport's layer stack when it is hosted there, leaving the
        /// runtime host and its document intact so the next Drive re-attaches. Idempotent: an undriven
        /// overlay, a document hosted on another viewport, or a document already detached is a no-op.
        /// Used to release an overlay a viewport stopped presenting while its world stays alive (a world
        /// rebind), where ~GuiOverlay's destroy-time detach is the wrong lifetime. Only the document the
        /// engine attached through Drive is touched.
        /// @param viewport  The viewport to detach the document from, if it is hosted there.
        void Detach(Renderer::Viewport& viewport) const;

    private:
        /// @brief Ensures the runtime record exists, holding the deferred binding before first Drive.
        GuiOverlayRuntime& EnsureRuntime() const;

        /// @brief Materializes the document host + presenter on first Drive and applies the binding.
        /// @param assets  The asset manager the host loads its recipe through.
        void EnsureHost(AssetManager& assets) const;
    };
}

VE_ENUM(::Veng::GuiOverlayPlacement, 0x2A7C7C857C323E8CULL)
VE_ENUMERATOR(PostTonemap)
VE_ENUMERATOR(SceneHdrPreBloom)
VE_ENUM_END();

VE_ENUM(::Veng::GuiOverlayProjection, 0x9BEC8506284EF46BULL)
VE_ENUMERATOR(ScreenSpace)
VE_ENUMERATOR(WorldAnchored)
VE_ENUM_END();

VE_REFLECT(::Veng::GuiOverlay, 0xC703A9C84AC4BA09ULL)
VE_FIELD(Document, .DisplayName = "Document")
VE_FIELD(Layer, .DisplayName = "Layer")
VE_FIELD(Driver, .DisplayName = "Driver")
VE_FIELD(Interactive, .DisplayName = "Interactive")
VE_FIELD(TargetSeat, .DisplayName = "Target Seat")
VE_FIELD(Placement, .DisplayName = "Placement")
VE_FIELD(Material, .DisplayName = "Material")
VE_FIELD(Projection, .DisplayName = "Projection")
VE_FIELD(AnchorPosition, .DisplayName = "Anchor Position")
VE_FIELD(AnchorRotation, .DisplayName = "Anchor Rotation")
VE_FIELD(SurfaceSize, .DisplayName = "Surface Size", .Display = {.Min = 0.001})
VE_FIELD(SurfaceResolution, .DisplayName = "Surface Resolution", .Display = {.Min = 1})
VE_REFLECT_END();
