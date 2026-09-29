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
/// The loader compares host vs. module values before calling VengModuleRegister.
/// Guarded with #ifndef so a target can force a mismatch via -D for testing.
#ifndef VENG_MODULE_ABI_VERSION
#define VENG_MODULE_ABI_VERSION 33u
#endif

/// @brief Emits the VengModuleAbiVersion() export; place in exactly one TU per module.
#define VE_EXPORT_MODULE_ABI()                                                                     \
    extern "C" VE_MODULE_EXPORT Veng::u32 VengModuleAbiVersion()                                   \
    {                                                                                              \
        return VENG_MODULE_ABI_VERSION;                                                            \
    }
