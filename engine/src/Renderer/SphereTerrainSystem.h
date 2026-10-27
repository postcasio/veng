#pragma once

#include <unordered_map>
#include <unordered_set>

#include <Veng/Veng.h>
#include <Veng/Renderer/SphereTerrain.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class CameraView;
    class Scene;
}

namespace Veng::Renderer
{
    /// @brief One renderer's sphere terrains: per Execute, gather the scene's terrains, select
    ///        each one's tiles for the view, and mark and request them on its source.
    ///
    /// The selection memory (last frame's splits) is keyed by the source, not the entity, so a
    /// source handed to another entity or scene keeps its hysteresis. A source no gather has seen
    /// for a while is forgotten.
    class SphereTerrainSystem
    {
    public:
        /// @brief Selects every visible terrain of a scene for one view.
        /// @param scene         The scene rendered.
        /// @param camera        The view's camera, in scene space.
        /// @param heightPixels  The view's rendered height in pixels.
        /// @param frame         The context's frame serial, which ages the memory.
        void Execute(const Scene& scene, const CameraView& camera, f32 heightPixels, u64 frame);

        /// @brief The tiles drawn for an entity last Execute, or null when it drew none.
        [[nodiscard]] const vector<SphereTerrainDrawnTile>* FindSelection(Entity entity) const;

        /// @brief An entity's last selection statistics, or null when it drew none.
        [[nodiscard]] const SphereTerrainSelectionStats* FindStats(Entity entity) const;

        /// @brief The last Execute's statistics summed over every terrain.
        [[nodiscard]] const SphereTerrainSelectionStats& GetTotals() const { return m_Totals; }

        /// @brief The frames a source's memory survives without being gathered.
        static constexpr u64 MemoryFrames = 120;

    private:
        /// @brief What one source's selection remembers between frames.
        struct Memory
        {
            /// @brief The tiles split last frame.
            std::unordered_set<u64> Splits;
            /// @brief The frame the source was last gathered.
            u64 LastFrame = 0;
        };

        /// @brief One entity's last selection.
        struct Selection
        {
            /// @brief The tiles drawn.
            vector<SphereTerrainDrawnTile> Drawn;
            /// @brief What the selection did.
            SphereTerrainSelectionStats Stats;
        };

        /// @brief Per source serial, its selection memory.
        std::unordered_map<u64, Memory> m_Memory;
        /// @brief Per entity value, its last selection.
        std::unordered_map<u64, Selection> m_Selections;
        /// @brief The last Execute's summed statistics.
        SphereTerrainSelectionStats m_Totals;
    };

    /// @brief The generated-texture priority a selection request takes.
    [[nodiscard]] i32 SphereTerrainRequestPriority(bool split, bool inFrustum, f32 urgency);
}
