#pragma once

#include <Veng/Veng.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    /// @brief Forward-declared editor registry; libveng never sees its definition.
    ///
    /// A non-editor host passes Editor = nullptr. Held by pointer so the incomplete type suffices.
    class EditorRegistry;

    /// @brief Forward-declared so the reference member needs no include here.
    class SystemRegistry;

    /// @brief Forward-declared so the pointer member needs no include here.
    class GuiDriverRegistry;

    /// @brief Forward-declared so the reference member needs no include here.
    class AssetTypeRegistry;

    /// @brief Forward-declared so the reference member needs no include here.
    class AssetLoaderRegistry;

    /// @brief The host-side module contract: the registries a loaded module writes into.
    ///
    /// The host owns these for the module's whole lifetime. Registration is GPU-free
    /// (a factory + reflected type descriptors + asset-type identities and loader factories),
    /// so no live Context/AssetManager is required; the host threads them into the Application
    /// it later constructs.
    struct VengModuleHost
    {
        /// @brief Receives the module's Application factory.
        ApplicationRegistry& App;
        /// @brief Receives the module's component/type descriptors.
        TypeRegistry& Types;
        /// @brief Receives the module's SceneSystem registrations, in run order.
        SystemRegistry& Systems;
        /// @brief Receives the module's asset-type identities, names, and display metadata.
        ///
        /// The sole seam an asset type registers its identity through, reachable from every host
        /// (launcher, cooker, editor). A cook module registers importers only, so one id can never
        /// arrive twice in the editor, where both images load — and a duplicate id is fatal.
        AssetTypeRegistry& AssetTypes;
        /// @brief Receives the module's AssetLoader factories, one per asset type it defines.
        ///
        /// Instantiated by the AssetManager at construction. A host with no live AssetManager (the
        /// cooker) passes a throwaway whose registrations are inert and discarded.
        AssetLoaderRegistry& AssetLoaders;
        /// @brief Receives the module's GuiDriver registrations (per-instance UI presentation drivers).
        GuiDriverRegistry* Drivers;
        /// @brief Non-null only when loaded by the editor host.
        EditorRegistry* Editor;
    };
}

extern "C"
{
    /// @brief Entry point exported by every game/editor module.
    ///
    /// The host dlsym()s this name, calls it once after load, and the module
    /// registers its factory and types into the provided host registries.
    /// C ABI ensures the symbol resolves robustly across module boundaries.
    VE_MODULE_EXPORT void VengModuleRegister(Veng::VengModuleHost* host);
}

/// @brief ABI version token baked into both host and module at compile time.
///
/// Bumped whenever VengModuleHost's layout, or the layout of anything a module passes through
/// it or reads from it, changes — a stale module registering a FieldDescriptor or AssetTypeInfo
/// of the wrong size, or reading a SystemContext whose fields have shifted, is exactly the silent
/// corruption this token exists to turn into a loud rejection. Version 13 adds SystemContext::World
/// (the runner handle of the ticking world), which shifts the fields after it for a stale module's
/// per-tick reads. Version 14 grows GuiDriverFrame with Root (the subtree root a driver drives — a
/// component boundary, or the document root) and gives GuiDriver::OnInstantiate a boundary
/// parameter: the frame is host-constructed and handed to a module-registered driver each drive,
/// and the driver's vtable is what a module subclasses, so a stale module reads the frame's trailing
/// fields shifted and overrides the wrong OnInstantiate slot. Version 15 grows GraphicsResolveOutput
/// with the Global facet (the machine-global renderer state a resolve produces): the struct is
/// host-constructed and handed by reference to a module-registered OnResolveGraphics override, so a
/// stale module would fill a short struct and the engine would read the facet past its end.
/// Version 16 grows that same Global facet with the texture-quality mip-skip level, the second
/// machine-global apply target the resolve produces, so a stale module fills a short facet the
/// same way. Version 17 changes the reflected AudioSource component: its Bus field's type moves
/// from the removed AudioBus enum to a bus-name string resolved to a BusId at load, so a stale
/// module would register a component descriptor whose Bus field carries the wrong leaf type.
/// Version 18 grows the Application vtable with the OnResolveAudio resolve seam and grows
/// ApplicationInfo with AudioSettingsSchema: the module subclasses Application (its vtable is what
/// the engine calls through) and constructs the ApplicationInfo it hands back, so a stale module
/// would lay out the vtable and the info struct short of what the engine reads.
/// Version 19 grows the builtin GuiOverlay component with a placement, a projection, and the
/// world-anchored plane transform/size/resolution fields: a module instantiates GuiOverlay
/// (Add<GuiOverlay>) with the engine's layout, so a stale module built against the shorter struct
/// would size and lay out the component short of what the engine reads and writes.
/// Version 20 grows the builtin Sky component with an optional runtime-only lighting source (a
/// radiance cube-view the IBL tier derives from, distinct from the displayed source): a module
/// instantiates Sky (Add<Sky>) with the engine's layout, so a stale module built against the
/// shorter struct would size and lay out the component short of what the engine reads and writes.
/// Version 21 splits the renderer's single allocation extent in two: SceneRendererInfo grows a
/// RenderExtent (the scene's own allocation, separate from the tail's) and SceneView grows
/// SceneColorExtent mid-struct. A module constructs a SceneRendererInfo to create a renderer and a
/// SceneView to drive one, so a stale module would hand the engine a short info struct and read the
/// per-frame extents at shifted offsets.
/// Version 22 adds the localization service to the module surface: ApplicationInfo grows a
/// LocaleIndex asset id and SystemContext grows a Localization& service (bound to the engine's
/// always-owned service). A module constructs the ApplicationInfo it hands back and reads the
/// SystemContext each tick, so a stale module would lay out the info struct short of what the engine
/// reads and read the per-tick fields after Localization at shifted offsets.
/// Version 23 changes the GuiDriver instantiation seam: OnInstantiate takes a single
/// GuiDriverContext in place of its four parameters and GuiDriverFrame grows a trailing
/// localization borrow. Both structs are host-constructed and handed to a module-registered driver,
/// and the driver's vtable is what a module subclasses, so a stale module would override a slot
/// whose signature no longer matches and read the frame short of what the engine fills.
/// Version 24 adds two Application virtuals — OnWorldPresented and OnWorldPresentAbandoned — beside
/// OnWorldArrival, and a DrawsCursor field to GuiOverlay. A module subclasses Application, so a
/// stale module's vtable is short of the slots the host dispatches through, and it lays out the
/// component the host reflects and spawns short of the field the host reads.
/// Version 25 widens BindlessRegistry::MaterialParamStride from 1024 to 1280 bytes. A module's
/// materials are cooked against the stride and its shaders index the one parameter buffer at
/// `index * MaterialParamStride`, so a stale module reads every material's block from the wrong
/// offset — a silent mis-shade of every draw rather than a fault.
/// Version 26 replaces that indexing scheme outright: a material's parameter block is a byte-offset
/// suballocation of BindlessRegistry::MaterialArenaBytes rather than a fixed slot, the selector a
/// draw pushes is a byte offset, and a shader loads its block at that offset with no multiply. A
/// module's shaders carry the arithmetic and its MaterialInstance reads the offset, so a stale
/// module scales an offset by a stride that no longer exists — the same silent mis-shade version 25
/// names, at every draw rather than at a moved one.
/// Version 27 grows MaterialField with the index of a texture field's paired sampler field, which
/// the material resolves once from its own schema. A module reads the field table through
/// Material::GetFields and addresses a field by its index in it, so a stale module lays the struct
/// out short and reads every field after the first at a shifted offset.
/// Version 28 grows MaterialField again, with the element count and stride that let one field be an
/// array of N scalars or vectors rather than N numbered members. A module reads the field table
/// through Material::GetFields and writes a field through the setters that address an element by
/// that stride, so a stale module lays the struct out short and reads the fields after Size at a
/// shifted offset.
/// Version 29 grows Gui::Style with the arc silhouette fields (Shape, ArcStart, ArcSweep,
/// ArcThickness, ArcCapStyle) and the stroke fields (Stroke, StrokeWidth, StrokeTrim), and
/// Gui::Element with the Polyline's Points. A module reads and writes an element and its style
/// through Gui::Document, so a stale module lays both out short and reads every field after the
/// first addition at a shifted offset.
/// Version 30 grows Localization::Localization and LocaleCatalog with the locale's elision table.
/// A module reaches the service through SystemContext and reads its generation and number
/// separators through inline accessors, so a stale module reads the members after the table at
/// shifted offsets.
/// Version 31 grows MouseMovedEvent with the cursor basis its position is measured in, and Input
/// with the basis of the position it last saw. A module reads both through the event and the
/// Input it is handed, so a stale module reads the members after the additions at shifted offsets.
/// Version 32 keys InputRouter's focus stacks, cursor seat and viewport associations by SeatRef (a
/// world plus a Viewer entity) rather than by the entity alone, and changes Application's held
/// focus-request tokens from a per-seat map to a list. A module subclasses Application and reads
/// the router through inline accessors, so a stale module lays out its own members after a base of
/// the wrong size and reads the cursor seat at a shifted offset.
/// Version 33 adds the BehaviorTask::OnAbort virtual after OnExit. A module subclasses
/// BehaviorTask and the engine's tree walk dispatches through its vtable, so a stale module's task
/// is short of the slot the host calls when it aborts a running leaf.
/// Version 34 grows TypeInfo with ServerOwned. A module registers its component types through
/// TypeRegistry::Register<T>(), which it instantiates and so builds the record at its own layout, so
/// a stale module would insert a short record the host reads the new flag and every later member of
/// at shifted offsets.
/// Version 35 grows GameNetInfo with MaxRewindSeconds and SceneSimulation with the SystemId of each
/// system it holds. A module constructs the ApplicationInfo that carries a GameNetInfo and reads a
/// SceneSimulation's pause and start state through inline accessors, so a stale module lays the
/// info struct out short and reads those flags at shifted offsets.
/// Version 36 grows Material and MaterialInfo with the translucent blend, ahead of the pipeline and
/// shader handles, and Scene with the effect pool it owns. A module builds a MaterialInfo for a
/// runtime material and reads a material's pipeline and a scene's services through inline
/// accessors, so a stale module lays both structs out short and reads the members after the
/// additions at shifted offsets.
/// Version 37 grows ReplicationServer's per-connection state with the set of entities whose spawn
/// was refused. A module that holds a ReplicationServer destroys it through the inline destructor
/// it instantiates, so a stale module would tear down the connection map at the old element layout.
/// Version 38 makes EffectDesc's sprite optional and grows it with an optional ribbon. A module
/// builds the EffectDesc it hands SpawnTransientEffect, so a stale module would pass a struct of
/// the old layout that the engine reads its sprite, ribbon and light from at shifted offsets.
/// Version 39 grows Scene with the seat release log it owns. A module reads a scene's services
/// through inline accessors, so a stale module reads the members after the addition at shifted
/// offsets.
/// Version 40 adds the Application::OnWorldDeparted virtual after OnWorldPresentAbandoned. A module
/// subclasses Application, so a stale module carries a vtable short of the slot the host dispatches
/// through when a world leaves presentation.
/// Version 41 grows ApplicationInfo with the profiler's construction parameters, World with its
/// interned profiler scope names, and the profiler's inline scope timer with the begin it now takes
/// from EnterScope. A module constructs the ApplicationInfo it hands back, reads a world through
/// inline accessors, and instantiates the scope macros, so a stale module lays the info struct out
/// short and calls the scope entry point under its old signature.
/// Version 42 grows Animation with its cached root-motion bone and SkinnedPose with its change
/// counter and input key. A module that builds an Animation in code, or reads a SkinnedPose, lays
/// the struct out short, so the engine would read the new members past its end.
/// Version 43 adds the SceneSystem::GetTickPolicy virtual after GetPhase, grows SystemContext with
/// LastStepThisFrame, WorldOpenInfo and GameWorldInfo with the per-frame step cap and wall-clock
/// budget, and SimClock (held by World) with its budget and step-cost estimate. A module subclasses
/// SceneSystem, builds SystemContexts and both info structs, and reads a world through inline
/// accessors, so a stale module carries a vtable short of the slot the simulation dispatches through
/// and lays every one of those structs out short.
/// Version 44 grows Scene with its topology version and world-transform cache, SceneBroadphase with
/// its refit state, and BVH with its build scratch and recorded costs. A module reads a scene
/// through inline accessors and a renderer that holds a broadphase, so a stale module lays all three
/// out short and reads the members after the additions at shifted offsets.
/// Version 45 grows VisibleMesh with its normal matrix, SceneBroadphase with its cull bitset, and
/// CommandBuffer with its draw counter (holding the bound pipeline in place of a copy of its formats),
/// and changes SceneRenderer's per-entity frame state from hashed maps to flat tables. A module reads
/// visible meshes through a SceneView, holds a renderer, and records through a CommandBuffer whose
/// inline bind helpers it instantiates, so a stale module lays all four out at the old layout.
/// Version 46 grows SceneView with the punctual shadow face mask, SceneRendererSettings with the
/// cascade caster size threshold, and SceneRenderer with its shadow-view count. A module builds
/// SceneViews and settings and holds a renderer, so a stale module lays all three out short.
/// Version 47 grows TypeInfo with its registry ordinal and TypeRegistry with its serial, replaces
/// Scene's hashed pool map with an ordinal-indexed table, and moves the component pool into a
/// public header so a component access inlines into the calling unit. A module registers types
/// and reads components through those inline paths, so a stale module lays all three out at the
/// old layout and looks its pools up in a table the host no longer has.
/// Version 48 grows WorldRunner with its scene-retiring hook. A module reaches the runner through
/// the Application it subclasses, so a stale module lays the runner out short.
/// Version 49 grows SceneRendererSettings with LightTileCulling, SceneRenderer with its light-tile
/// cull, and the view-constants block (BindlessRegistry::ViewConstantsStride, 704 → 720 bytes) with
/// where a view's per-tile light masks live. A module builds settings, holds a renderer, and cooks
/// shaders that index the view-constants buffer by that stride, so a stale module lays the first two
/// out short and reads every view block after the first at the wrong offset.
/// Version 50 grows SceneRendererSettings with GBufferShadingOverride and SceneRenderer with the
/// override's pipeline owner, so a stale module lays both out short.
/// Version 51 grows Gui::Element with the work its Document caches across frames (ElementRetained),
/// Gui::Document with its re-resolve queues and retained build, Gui::DrawList with per-run texture
/// keys and recorded glyphs, and ShapedGlyph with its pen. A module drives documents and builds draw
/// lists, so a stale one lays every element, document, and list out short.
/// Version 52 grows AssetLoader with the two-phase protocol (ParsesOffThread, Parse) and makes Load
/// overridable rather than pure, AssetCacheEntry with Failed, WorldRunner with its capture pool, and
/// WorldCaptureDriveInfo/Result with the capture build budget. A module subclasses AssetLoader, so a
/// stale one carries a vtable short of the slots the manager dispatches through.
/// Version 53 adds the Application::IsImGuiFrameWanted virtual, ImGuiLayer's frame-skip state,
/// the presented-frame request latch in Context::Native, and McpTool::BeforeFrame. A module
/// subclasses Application and registers MCP tools by value, so a stale one carries a vtable short of
/// the slot the frame dispatches through and hands the server a short tool.
/// Version 54 grows Material and MaterialInfo with whether the fragment samples the scene-colour
/// grab, and RenderGraph::Pass with whether a skipped frame leaves its outputs unread. A module
/// builds a MaterialInfo for a runtime material and declares graph passes, so a stale module lays
/// both out short.
/// Version 55 grows ManagedViewportSet with the level-look resolver every level-configured viewport
/// resolves through and its bound-viewport record with the look it was resolved from, adds
/// Application's level-look resolve core, and drops LevelOverlay's copies of its look and knobs. A
/// module reaches the set through the Application it subclasses and holds LevelOverlay handles by
/// value, so a stale module lays both out at the old layout.
/// Version 56 grows ApplicationInfo and ContextInfo with the queue-submit mode, and Context with
/// the mode it resolved. A module builds the ApplicationInfo its Application is constructed from,
/// so a stale module hands the host a short one.
/// Version 57 has AudioEngine::PlayGenerator take a shared Ref<IAudioGenerator> and widens
/// AudioEngine's voice and deferred-free records to hold it. A module reaches the engine through
/// SystemContext::Audio and reads its inline accessors, so a stale module reads it at the old layout.
/// Version 58 grows Gui::Style with FillTint and Gui::Element with StateAge. A module reads and
/// writes an element and its style through Gui::Document, so a stale module lays both out short and
/// reads every field after the additions at a shifted offset.
/// Version 59 grows Gui::Style with FillAgeStates, so a stale module lays a style out short.
/// Version 60 grows RibbonPath with its Placement. A module builds RibbonPath in code, so a stale
/// module leaves the byte the renderer reads the placement from unwritten.
/// Version 61 grows Renderer::CaptureSurface with Enabled ahead of its settings, MeshRenderer and
/// VisibleMesh with SortPriority, and Ribbon, Trail and RibbonPath with Layer, so a stale module that
/// builds any of them in code writes its fields short of where the engine reads them.
/// Version 62 grows Light with SpecularScale, so a stale module that builds a light in code lays it
/// out short and the engine reads the polygon vertices at a shifted offset.
/// Version 63 grows RibbonPath with Occluded, so a stale module that builds a path in code leaves
/// the byte the gather reads its occlusion from unwritten.
/// Version 64 grows GamepadState with the touchpad, Misc and paddle controls, the pad's type and
/// name, Input with the per-pad touchpad Sim latch, InputRouter with the virtual-gamepad sink and
/// its queued edit, and Application with its pad backend and slot states. A module reads pads
/// through an Input it is handed and subclasses Application, so a stale module reads every pad
/// field after the arrays at a shifted offset and lays the application out short.
/// Version 65 grows Binding with its Threshold and Exponent, and Input with each pad's raw axes and
/// deadzones. A module builds bindings in code and reads pads through an Input it is handed, so a
/// stale module lays a binding out short and reads Input's members after the pad table at a
/// shifted offset.
/// Version 66 grows Binding with its Modifier and ModifierThreshold, so a stale module that builds a
/// binding in code lays it out short and the resolver reads its modifier from past the end.
/// Version 67 grows InputAction with its Role, RepeatDelay and RepeatRate, InputContextStack with
/// Exclusive, InputRouter with its claimed keys, InputConsumer with the ForwardRole virtual, and
/// Application with its default UI context and role resolver. A module builds actions and stacks in
/// code, implements consumers and subclasses Application, so a stale module lays all of them out
/// short and carries a consumer vtable short of the slot the role dispatch calls through.
/// Version 68 grows ActionRole with ReleaseFocus and InputRouter's focus entries with their
/// suspension, and moves the focus release off a fixed router key onto that role. A stale module
/// declares no release action and relies on a key the router no longer reads, so nothing it binds
/// frees a captured cursor.
/// Version 69 grows SystemContext with its Haptics engine between Audio and Localization, and
/// Application with its haptics engine. A module builds contexts, is handed them each tick and
/// subclasses Application, so a stale module reads every context field after Audio at a shifted
/// offset and lays the application out short.
/// Version 70 grows Light with AngularRadius between Radius and TwoSided, so a stale module that
/// builds a light in code lays it out short and the engine reads every field after Radius at a
/// shifted offset.
/// Version 71 grows SceneView and Viewport::ViewState with the specular anti-aliasing variance and
/// threshold after SsrMaxRoughness. A module builds views and view states in code, so a stale module
/// lays both out short and the renderer reads every field after SsrMaxRoughness at a shifted offset.
/// Version 72 grows StoreFamily with FlushIntervalSeconds and makes Store::Flush return nothing, its
/// write moving to a background job. A module registers families in code and flushes its stores, so
/// a stale module passes a family short and reads a result Flush no longer returns.
/// Version 73 grows ShapedGlyph with the size and span it was shaped at, and the shaped-run cache
/// every Gui::Element carries with each run's span ends and sizes, so a paragraph of styled spans
/// shapes as one run. A module's drivers read elements and shaped runs, so a stale module reads
/// every Element field after the cache, and every glyph past the first, at a shifted offset.
/// Version 74 replaces Application's one-flag input edge latch with an InputEdgeLatch that a driver
/// stepping its own clock reports into (ReportSimFrame). A module subclasses Application, so a stale
/// module lays the application out short.
/// The loader compares host vs. module values before calling VengModuleRegister.
/// Guarded with #ifndef so a target can force a mismatch via -D for testing.
#ifndef VENG_MODULE_ABI_VERSION
#define VENG_MODULE_ABI_VERSION 74u
#endif

/// @brief Emits the VengModuleAbiVersion() export; place in exactly one TU per module.
#define VE_EXPORT_MODULE_ABI()                                                                     \
    extern "C" VE_MODULE_EXPORT Veng::u32 VengModuleAbiVersion()                                   \
    {                                                                                              \
        return VENG_MODULE_ABI_VERSION;                                                            \
    }
