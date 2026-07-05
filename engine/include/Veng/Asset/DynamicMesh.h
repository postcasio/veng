#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Mesh.h>

namespace Veng
{
    class AssetManager;

    /// @brief A runtime mesh whose geometry rebuilds asynchronously behind a stable front handle.
    ///
    /// Wraps the AssetManager::Build<Mesh> streaming path for geometry that changes over time
    /// (procedural overlays, chunked geometry, editor visualizations). Rebuild() queues new
    /// geometry; at most one build is in flight at a time, and a newer Rebuild replaces queued
    /// geometry rather than stacking builds — latest wins. Update(), called once per frame,
    /// promotes a completed build to the front handle and starts the queued one. GetMesh()
    /// therefore always returns the newest *resident* geometry: a consumer keeps drawing the
    /// previous mesh while a rebuild streams in on the task system, and the swap is a handle
    /// assignment, never a frame with no mesh. A replaced mesh retires through the per-frame
    /// deferred-destruction path, so swapping mid-frame is safe.
    ///
    /// A failed build factory (logged by the manager) never becomes resident and parks the
    /// in-flight slot; the Mesh build path has no recoverable failure mode, so this arises
    /// only from a fatal condition that aborts anyway.
    class VE_API DynamicMesh
    {
    public:
        /// @brief Constructs an idle dynamic mesh with no geometry.
        /// @param assets The manager the async builds run through; must outlive this object.
        /// @param name   Debug name carried by every built mesh and its buffers.
        DynamicMesh(AssetManager& assets, string name);

        /// @brief Queues an async rebuild with new geometry, replacing any not-yet-started one.
        ///
        /// Starts the build immediately when none is in flight; otherwise the geometry waits
        /// and the first Update() after the in-flight build lands starts it. Successive calls
        /// between starts keep only the newest geometry.
        /// @param data CPU geometry for the new mesh; moved into the build.
        /// @pre data has at least one vertex and one index — an empty rebuild is API misuse
        ///      (hide a mesh by clearing its consumer's handle, or call Reset()).
        void Rebuild(MeshData data);

        /// @brief Promotes a completed build to the front handle and starts the queued rebuild, if any.
        ///
        /// Call once per frame.
        /// @return True when the front handle changed this call, so a consumer can reassign
        ///         its MeshRenderer only on a real swap.
        bool Update();

        /// @brief Returns the newest resident mesh; empty until the first rebuild completes.
        [[nodiscard]] const AssetHandle<Mesh>& GetMesh() const { return m_Front; }

        /// @brief Whether a rebuild is in flight or queued.
        [[nodiscard]] bool IsRebuildPending() const { return m_InFlight || m_Queued.has_value(); }

        /// @brief Drops the front mesh, detaches from any in-flight build, and clears queued geometry.
        ///
        /// The detached build finishes on its worker and its result retires unreferenced;
        /// the object is reusable immediately.
        void Reset();

    private:
        /// @brief Starts the queued geometry's async build.
        void StartQueuedBuild();

        /// @brief The manager the async builds run through.
        AssetManager& m_Assets;
        /// @brief Debug name carried by every built mesh.
        string m_Name;
        /// @brief The newest resident mesh, returned by GetMesh().
        AssetHandle<Mesh> m_Front;
        /// @brief The in-flight build's pending handle; promoted to the front when resident.
        AssetHandle<Mesh> m_Back;
        /// @brief Geometry waiting for the in-flight build to land (the latest Rebuild wins).
        optional<MeshData> m_Queued;
        /// @brief Whether m_Back holds an in-flight build (a pending handle is not
        /// distinguishable from an empty one through the handle alone).
        bool m_InFlight = false;
    };
}
