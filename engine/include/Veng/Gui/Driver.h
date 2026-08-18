#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    namespace Gui
    {
        class Document;
    }

    /// @brief Stable identity of a registered GuiDriver, authored exactly like a SystemId/ActionId.
    ///
    /// A GuiDriver subclass declares one through VE_GUI_DRIVER; GuiDriverRegistry keys its catalog on
    /// it and a GuiOverlay/GuiSurface names the driver it wants by it. A u64 leaf in the
    /// SystemId/ActionId id family, minted with `vengc generate-id` (a hardcoded literal for engine
    /// drivers). Null is the reserved empty id — a component whose Driver is Null is undriven.
    enum class GuiDriverId : u64
    {
        /// @brief The empty id, distinct from every minted driver id; an undriven component.
        Null = 0
    };

    /// @brief The per-frame services a GuiDriver's OnUpdate reads.
    ///
    /// Borrowed for the duration of the call. Assembled by the driven component's Drive from the
    /// claiming viewport, so a driver reads the presenting viewport's real view (camera/region/UI
    /// scale) and the scene it lives in without holding a Renderer::Viewport. The scene is mutable: a
    /// driver may read scene state and stamp request/command components and ViewOutput-tagged
    /// components (see the driver boundary below), but never a replicated or Sim-input component.
    struct GuiDriverFrame
    {
        /// @brief The live document this driver drives (the driven component's instantiated tree).
        Gui::Document& Document;
        /// @brief The presented scene the driven component lives in; mutable within the driver boundary.
        Scene& Scene;
        /// @brief The entity carrying the driven GuiOverlay/GuiSurface — the driver's own instance.
        ///
        /// What makes a driver's authored configuration reachable: the component the driver was
        /// named on sits here, so its siblings on the same entity (its own config component, its
        /// MeshRenderer, its Transform) are read through the scene rather than searched for. Never
        /// null while a driver runs — a driver exists because a component on this entity named it.
        Entity Owner = Entity::Null;
        /// @brief The claiming viewport's bound seat, or Entity::Null when the viewport is unbound.
        Entity Seat = Entity::Null;
        /// @brief Frame delta time in seconds.
        f32 Delta = 0.0f;
        /// @brief Interpolation fraction into the next Sim tick, in [0, 1) — the render gather's own.
        ///
        /// The fraction the gather blends each drawn entity's transform by, so a driver placing a
        /// marker against a moving body reads the pose the renderer is drawing this frame rather
        /// than the tick pose it will be drawn ahead of. Zero for a static or un-ticked scene.
        f32 Alpha = 0.0f;
        /// @brief The presenting viewport's resolved camera, region, and UI scale this frame.
        SystemViewInfo View;
    };

    /// @brief A named, per-instance presentation binding the engine drives from component data.
    ///
    /// The ergonomic path for binding a HUD: a driver owns its Gui::BindingContext / view-model and
    /// the one-time element resolution a consumer otherwise hand-rolls in a find-and-bind system.
    /// Both document-carrying components name one: GuiOverlay (screen space) and GuiSurface (a
    /// document on a world mesh). Their Drive instantiates the driver named by the component's Driver
    /// id, owns it for the runtime's lifetime, re-runs OnInstantiate whenever the document
    /// (re)instantiates, and calls OnUpdate each drive. Two claimed instances of one overlay
    /// (split-screen) are two driver instances with independent view-models, so the per-instance
    /// state a per-world system would have keyed by entity dissolves.
    ///
    /// **Where in the frame the driver runs differs between the two, and it is the one asymmetry.**
    /// An overlay composites after the scene, so its driver runs after the render gather and its
    /// component stamps are read by the next frame's gather. A surface is sampled *by* the scene, so
    /// its document must be rendered before the gather and its driver therefore runs ahead of it —
    /// the same frame's render sees what it stamped. Neither may add or remove the component it is
    /// driven from, which would disturb the walk it is being driven inside.
    ///
    /// The boundary is concrete and checkable: a driver reads scene state, stamps request/command
    /// components, and beyond those may write only a component tagged VE_VIEW_OUTPUT — derived,
    /// view-owned state gameplay may read but no simulation or wire owns. A driver never writes a
    /// VE_REPLICATED or a Sim-input component, and never advances authoritative simulation — that
    /// stays components + systems. The bare SetContext / find-and-bind system pattern remains fully
    /// supported; the driver is the ergonomic path, not the only one.
    class GuiDriver
    {
    public:
        /// @brief Virtual destructor; drivers are owned through GuiDriver pointers.
        virtual ~GuiDriver() = default;

        /// @brief Once per (re)instantiate: resolve elements, build the view-model, SetContext.
        ///
        /// Runs on the first drive that instantiates the document and again on any re-instantiate
        /// (exactly like GuiOverlay::SetOnInstantiate), so cached element pointers stay valid. The
        /// default does nothing.
        /// @param document  The freshly instantiated live document.
        /// @param scene     The presented scene the driven component lives in.
        /// @param seat      The claiming viewport's seat, or Entity::Null when unbound.
        virtual void OnInstantiate(Gui::Document& document, Scene& scene, Entity seat)
        {
            (void)document;
            (void)scene;
            (void)seat;
        }

        /// @brief Once per frame while the document is attached, after the scene's View phase.
        ///
        /// Where a driver feeds its view-model and stamps its request/command/ViewOutput components.
        /// The default does nothing.
        /// @param frame  The per-frame services (document, scene, owner, seat, timing, resolved view).
        virtual void OnUpdate(const GuiDriverFrame& frame) { (void)frame; }
    };

    /// @brief Identity trait every registered GuiDriver subclass specialises, via VE_GUI_DRIVER.
    ///
    /// Unspecialised by default — a GuiDriver subclass declares its identity through the
    /// VE_GUI_DRIVER macro, which emits a stable GuiDriverId and a display name. GuiDriverRegistry
    /// reads the trait, so a driver registered without a VE_GUI_DRIVER fails to compile.
    /// @tparam T The concrete GuiDriver subclass.
    template <class T>
    struct VengGuiDriver;

    /// @brief The stable GuiDriverId of a registered driver, read as a compile-time constant off its trait.
    /// @tparam T The concrete GuiDriver subclass.
    /// @return The authored GuiDriverId.
    template <class T>
    constexpr GuiDriverId GuiDriverIdOf()
    {
        return VengGuiDriver<T>::Id;
    }

    /// @brief The display name of a registered driver, read off its trait.
    /// @tparam T The concrete GuiDriver subclass.
    /// @return The authored display name.
    template <class T>
    string GuiDriverNameOf()
    {
        return VengGuiDriver<T>::Name();
    }
}

VE_LEAF(::Veng::GuiDriverId, 0x9EE0192EFE831F09ULL, ::Veng::FieldClass::Scalar);

/// @brief Declares a GuiDriver subclass's catalog identity by specialising VengGuiDriver\<T\>.
///
/// Emits a stable GuiDriverId and a display name, so GuiDriverRegistry::Register stores
/// `{ GuiDriverId, Name, factory }` and the catalog enumerates and resolves the driver without
/// instantiating it. Authored exactly like a VE_SYSTEM: the id is a hardcoded 0x…ULL literal for
/// engine drivers or a `vengc generate-id` value for game drivers, and two drivers claiming one id
/// is a fatal collision assert at registration.
/// @param Type        The concrete GuiDriver subclass.
/// @param IdLiteral   The authored GuiDriverId (uppercase hex 0x…ULL).
/// @param NameLiteral The display name string literal.
#define VE_GUI_DRIVER(Type, IdLiteral, NameLiteral)                                                \
    template <>                                                                                    \
    struct ::Veng::VengGuiDriver<Type>                                                             \
    {                                                                                              \
        static constexpr ::Veng::GuiDriverId Id = static_cast<::Veng::GuiDriverId>(IdLiteral);     \
        static ::Veng::string Name() { return (NameLiteral); }                                     \
    }
