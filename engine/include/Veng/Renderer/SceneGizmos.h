#pragma once

#include <span>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Veng.h>

namespace Veng
{
    class Scene;
}

namespace Veng::Renderer
{
    class DebugDraw;

    /// @brief One selectable family of otherwise-invisible scene content, as a bit.
    ///
    /// **Most of what a scene contains draws nothing.** A light, a camera, a collider, a mesh
    /// socket, an interaction volume, an audio source, a capture probe and a bare transform are all
    /// real, placed, load-bearing state with no geometry of their own — so the only way to see
    /// where any of them actually *is* is to draw a stand-in. That is what this pass does, and the
    /// families are separable because a scene carrying hundreds of one kind is unreadable while a
    /// different question is being asked of it.
    ///
    /// The bits are a stable set an interface may iterate rather than restate: SceneGizmoGroupTable
    /// pairs each with a name, so a consumer's checkbox list is the engine's own list and a family
    /// added here appears in it with no change on the consuming side.
    enum class SceneGizmo : u32
    {
        /// @brief Nothing.
        None = 0u,
        /// @brief Every Light: an icon, and the wireframe of the volume or surface it emits from.
        Lights = 1u << 0u,
        /// @brief Every Camera: an icon and its view frustum, near-clamped so it stays readable.
        Cameras = 1u << 1u,
        /// @brief Every Collider: the primitive's own oriented wireframe, sensors in their own hue.
        Colliders = 1u << 2u,
        /// @brief Every MeshSocket on every resident mesh: an axis triad at the socket's own pose.
        Sockets = 1u << 3u,
        /// @brief Every Interactable's range sphere, dimmed while the interactable is disabled.
        Interaction = 1u << 4u,
        /// @brief Every spatial AudioSource's min/max distance spheres, and the listener.
        Audio = 1u << 5u,
        /// @brief Every CaptureSurface: the probe's own origin and the axes it captures along.
        Probes = 1u << 6u,
        /// @brief Every entity that draws nothing at all: a small axis triad at its transform.
        Empties = 1u << 7u,
        /// @brief Every family above.
        All = Lights | Cameras | Colliders | Sockets | Interaction | Audio | Probes | Empties,
    };

    /// @brief Union of two gizmo sets.
    [[nodiscard]] constexpr SceneGizmo operator|(const SceneGizmo a, const SceneGizmo b)
    {
        return static_cast<SceneGizmo>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    /// @brief Intersection of two gizmo sets.
    [[nodiscard]] constexpr SceneGizmo operator&(const SceneGizmo a, const SceneGizmo b)
    {
        return static_cast<SceneGizmo>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    /// @brief The set @p a without the members of @p b.
    [[nodiscard]] constexpr SceneGizmo operator~(const SceneGizmo a)
    {
        return static_cast<SceneGizmo>(~static_cast<u32>(a) & static_cast<u32>(SceneGizmo::All));
    }

    /// @brief Whether @p groups contains every bit of @p bit.
    /// @param groups  The selected set.
    /// @param bit     The family (or families) to test for.
    /// @return True when every bit of @p bit is selected.
    [[nodiscard]] constexpr bool HasGizmo(const SceneGizmo groups, const SceneGizmo bit)
    {
        return (static_cast<u32>(groups) & static_cast<u32>(bit)) == static_cast<u32>(bit) &&
               bit != SceneGizmo::None;
    }

    /// @brief A gizmo family paired with the name an interface offers it under.
    struct SceneGizmoGroupInfo
    {
        /// @brief The family's bit.
        SceneGizmo Bit = SceneGizmo::None;
        /// @brief Short display name, e.g. "Lights".
        const char* Name = "";
        /// @brief One line saying what the family draws, for a tooltip.
        const char* Description = "";
    };

    /// @brief The gizmo families, in a stable display order.
    ///
    /// A consumer builds its selection interface from this rather than listing the families itself,
    /// so a family added to SceneGizmo appears in every consumer's interface unchanged.
    /// @return One entry per family, excluding None and All.
    [[nodiscard]] std::span<const SceneGizmoGroupInfo> SceneGizmoGroupTable();

    /// @brief The optional art and the picking behaviour a gizmo pass draws with.
    ///
    /// **The icons are optional and the engine ships none.** `DebugDraw`'s billboard takes a
    /// bindless slot the consumer supplies, and this holds the same: an invalid handle draws a
    /// small wireframe marker in the icon's place, so a consumer with no icon art still sees
    /// everything's position and orientation and only loses the pictogram.
    struct SceneGizmoStyle
    {
        /// @brief Icon sampled for a light's billboard; invalid draws a wireframe marker instead.
        TextureHandle LightIcon;
        /// @brief Icon sampled for a camera's billboard; invalid draws a wireframe marker instead.
        TextureHandle CameraIcon;
        /// @brief Billboard edge length, in world units at the billboard's own depth.
        f32 IconSize = 0.6f;
        /// @brief Marker size for a family with no icon, and for a socket's or an empty's triad.
        f32 MarkerSize = 0.15f;
        /// @brief Whether a billboard writes its entity's pick id, making the icon selectable.
        ///
        /// An editor wants this; a game drawing the layer to look at it does not, and a pick id
        /// written by a viewport with no picking pass is inert either way.
        bool Pickable = false;
    };

    /// @brief Draws a stand-in for every selected family of invisible content in @p scene.
    ///
    /// Accumulates into @p debug, which the renderer clears at the start of each Execute — so this
    /// is called once per frame the layer should appear, like every other debug-draw consumer.
    /// Pure scene-query plus glm: no device, no asset loads, nothing retained between calls.
    ///
    /// Every family is skipped whole when its bit is clear, so the cost of an unselected family is
    /// one branch rather than a walk.
    /// @param scene   The scene to walk.
    /// @param debug   The accumulator to draw into.
    /// @param groups  The families to draw; SceneGizmo::None draws nothing.
    /// @param style   Icons, sizes, and whether billboards are pickable.
    void DrawSceneGizmos(const Scene& scene, DebugDraw& debug, SceneGizmo groups,
                         const SceneGizmoStyle& style = {});
}
