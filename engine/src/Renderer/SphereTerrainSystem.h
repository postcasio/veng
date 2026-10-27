#pragma once

#include <array>
#include <unordered_map>
#include <unordered_set>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/SphereTerrain.h>
#include <Veng/Scene/Entity.h>

#include "SphereTerrainDraw.h"

namespace Veng
{
    class AssetManager;
    class CameraView;
    class MaterialInstance;
    class Scene;
}

namespace Veng::Renderer
{
    class SurfaceShadowPipelines;
    class Buffer;
    class CommandBuffer;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class DrawBudget;
    class GraphicsPipeline;
    class PipelineLayout;
    class ShadowCasterViews;
    struct GpuDrawData;
    struct SurfacePush;

    /// @brief What one renderer's Select reads of its frame.
    struct SphereTerrainSelectInfo
    {
        /// @brief The view's camera, in scene space.
        const CameraView* Camera = nullptr;
        /// @brief The view's rendered height in pixels.
        f32 HeightPixels = 1.0f;
        /// @brief The context's frame serial, which ages the memory.
        u64 Frame = 0;
        /// @brief The view's frame delta in seconds, which runs the arrival floors.
        f32 Delta = 0.0f;
        /// @brief The renderer's shadow distance: tiles within it fold into the caster bound, and
        ///        the omnidirectional radius is raised to it.
        f32 ShadowDistance = 0.0f;
    };

    /// @brief One renderer's sphere terrains: select each one's tiles for the view, mark and
    ///        request them on its source, cull them per view, and draw them into the g-buffer and
    ///        the shadow views.
    ///
    /// Each frame Select runs the selection (from the true eye, whatever scale the terrain draws
    /// at) and resolves each terrain's materials; ClaimDrawSlots gives each drawn surface one
    /// DrawData slot for its material; PrepareFrame culls every tile against the main view and
    /// every shadow view and writes the frame slot's records, lists and set-4 descriptor sets;
    /// the passes then record one instanced draw of the shared grid per view and surface.
    ///
    /// The selection memory (last frame's splits, the pose, the arrival floors, the generator's
    /// cached words) is keyed by the source, not the entity, so a source handed to another entity
    /// or scene keeps its hysteresis and draws what it drew the frame before. A source no Select
    /// has seen for a while is forgotten.
    class SphereTerrainSystem
    {
    public:
        /// @brief Creates the shared grid, the shadow pipelines and the per-frame copies.
        static Unique<SphereTerrainSystem> Create(Context& context, AssetManager& assets);

        ~SphereTerrainSystem();

        SphereTerrainSystem(const SphereTerrainSystem&) = delete;
        SphereTerrainSystem& operator=(const SphereTerrainSystem&) = delete;

        /// @brief Selects every visible terrain of a scene for one view, and resolves what each
        ///        one draws with.
        /// @param scene  The scene rendered.
        /// @param info   The view.
        void Select(const Scene& scene, const SphereTerrainSelectInfo& info);

        /// @brief The scene-space bound of the shadow-casting tiles within the shadow distance.
        [[nodiscard]] const AABB& GetCasterBounds() const { return m_CasterBounds; }

        /// @brief Gives each drawn surface one draw slot and writes its DrawData record.
        /// @param drawData  The frame's DrawData region (already offset by the frame base).
        /// @param budget    The frame's draw budget.
        void ClaimDrawSlots(GpuDrawData* drawData, DrawBudget& budget);

        /// @brief Culls the selected tiles per view and writes the frame slot's records, lists and
        ///        descriptor sets.
        /// @param frameIndex The frame in flight being recorded.
        /// @param camera     The view's camera.
        /// @param shadows    The frame's shadow views, or null when no shadow pass runs.
        void PrepareFrame(u32 frameIndex, const CameraView& camera,
                          const ShadowCasterViews* shadows);

        /// @brief Records the main view's terrain draws into the bound g-buffer attachments.
        /// @param cmd          The g-buffer pass's command buffer.
        /// @param push         The frame's surface push (frame base and view constants index).
        /// @param drawDataSet  The DrawData set bound at set 3.
        /// @param shadowReaders Binds each material's pipeline, and the shadow set for one reading
        ///                     the shadow maps.
        void RecordGBuffer(CommandBuffer& cmd, const SurfacePush& push,
                           const Ref<DescriptorSet>& drawDataSet,
                           SurfaceShadowPipelines& shadowReaders) const;

        /// @brief Whether any terrain casts into shadow view @p shadowView this frame.
        [[nodiscard]] bool HasShadowDraws(u32 shadowView) const;

        /// @brief Records one shadow view's terrain casters into its bound tile.
        /// @param cmd         The shadow pass's command buffer.
        /// @param shadowView  The view's index among the frame's shadow views.
        /// @param viewProj    The view's raw light-space view-projection.
        /// @param depthClamp  Whether the pass pancakes casters onto its near plane.
        void RecordShadowView(CommandBuffer& cmd, u32 shadowView, const mat4& viewProj,
                              bool depthClamp) const;

        /// @brief The tiles drawn for an entity last frame, or null when it drew none.
        [[nodiscard]] const vector<SphereTerrainDrawnTile>* FindSelection(Entity entity) const;

        /// @brief An entity's last statistics, or null when it drew none.
        [[nodiscard]] const SphereTerrainSelectionStats* FindStats(Entity entity) const;

        /// @brief The last frame's statistics summed over every terrain.
        [[nodiscard]] const SphereTerrainSelectionStats& GetTotals() const { return m_Totals; }

        /// @brief The frames a source's memory survives without being selected.
        static constexpr u64 MemoryFrames = 120;

    private:
        SphereTerrainSystem(Context& context, AssetManager& assets);

        /// @brief What one source's selection remembers between frames.
        struct Memory
        {
            /// @brief The tiles split last frame.
            std::unordered_set<u64> Splits;
            /// @brief The tiles drawn last frame.
            std::unordered_set<u64> Drawn;
            /// @brief Per arriving tile, the clock time its arrival began.
            std::unordered_map<u64, f64> Arrivals;
            /// @brief This frame's arrival floors, per tile.
            std::unordered_map<u64, f32> Floors;
            /// @brief The generator's words per tile, for the tiles drawn or parenting a drawn one.
            std::unordered_map<u64, std::array<u32, MaxSphereTerrainTileWords>> TileData;
            /// @brief The seconds this source has been selected over.
            f64 Clock = 0.0;
            /// @brief Last frame's centre in the stable frame.
            dvec3 PrevCenter{0.0};
            /// @brief Last frame's orientation.
            glm::dquat PrevOrientation{1.0, 0.0, 0.0, 0.0};
            /// @brief Last frame's floating origin.
            dvec3 PrevOrigin{0.0};
            /// @brief Whether the pose above was recorded.
            bool HasPrevious = false;
            /// @brief The frame the source was last selected.
            u64 LastFrame = 0;
        };

        /// @brief One surface a terrain draws: its own, or a level layer.
        struct Surface
        {
            /// @brief The resident material, or null when the surface is not drawn.
            const MaterialInstance* Material = nullptr;
            /// @brief The layer's height; zero for the terrain's own surface.
            f32 Height = 0.0f;
            /// @brief Its DrawData slot, region-relative; valid when Seated.
            u32 DrawSlot = 0;
            /// @brief Whether ClaimDrawSlots seated it.
            bool Seated = false;
        };

        /// @brief One selected terrain this frame.
        struct Terrain
        {
            /// @brief Its entity.
            Entity Owner;
            /// @brief Its source.
            Ref<SphereTerrainSource> Source;
            /// @brief Its memory.
            Memory* Mem = nullptr;
            /// @brief Its surfaces: the terrain's own, then each level layer.
            std::array<Surface, SphereTerrainSurfaceCount> Surfaces{};
            /// @brief The level layers drawn.
            u32 LayerCount = 0;
            /// @brief Its centre in the stable frame.
            dvec3 Center{0.0};
            /// @brief Its orientation.
            glm::dquat Orientation{1.0, 0.0, 0.0, 0.0};
            /// @brief The scale about the eye it draws at.
            f32 Scale = 1.0f;
            /// @brief Whether it casts.
            bool CastsShadows = true;
            /// @brief Per view, per surface, its list's first entry and count in the list buffer.
            vector<std::array<uvec2, SphereTerrainSurfaceCount>> Lists;
            /// @brief The frame slot's set-4 descriptor set it binds.
            const DescriptorSet* Set = nullptr;
        };

        /// @brief One frame in flight's copies.
        struct FrameSlot
        {
            /// @brief The records.
            Ref<Buffer> Records;
            /// @brief The lists.
            Ref<Buffer> Lists;
            /// @brief Per source serial, the set-4 set bound this slot.
            std::unordered_map<u64, Ref<DescriptorSet>> Sets;
        };

        /// @brief Builds the depth-only terrain pipeline, with or without depth clamp.
        Ref<GraphicsPipeline> BuildShadowPipeline(AssetManager& assets, bool depthClamp);

        /// @brief Reports an entity's undrawable material once.
        void Report(Entity entity, const string& why);

        Context& m_Context;

        /// @brief The shared grid's indices.
        Ref<Buffer> m_GridIndices;
        /// @brief The terrain set's layout, which every set written here is allocated from.
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief The shadow pipelines' layout.
        Ref<PipelineLayout> m_ShadowLayout;
        /// @brief The depth-only terrain pipeline without depth clamp (punctual faces).
        Ref<GraphicsPipeline> m_ShadowPipeline;
        /// @brief The depth-only terrain pipeline with depth clamp where the device has it.
        Ref<GraphicsPipeline> m_ShadowClampPipeline;

        /// @brief One copy per frame in flight.
        vector<FrameSlot> m_Slots;
        /// @brief The frame slot prepared last.
        u32 m_FrameIndex = 0;
        /// @brief Whether a frame was prepared since the last Select.
        bool m_Prepared = false;
        /// @brief Whether the last Select's surfaces are still to claim their draw slots.
        bool m_Claimable = false;
        /// @brief The camera position the frame's records are relative to.
        vec3 m_CameraPosition{0.0f};
        /// @brief Where the selected scene's origin stands in the stable frame.
        dvec3 m_FrameOrigin{0.0};
        /// @brief The shadow views the frame prepared lists for.
        u32 m_ShadowViewCount = 0;

        /// @brief Per source serial, its selection memory.
        std::unordered_map<u64, Memory> m_Memory;
        /// @brief Per entity value, its last selection and statistics.
        std::unordered_map<u64,
                           std::pair<vector<SphereTerrainDrawnTile>, SphereTerrainSelectionStats>>
            m_Selections;
        /// @brief This frame's selected terrains.
        vector<Terrain> m_Terrains;
        /// @brief The shadow-casting tiles' bound within the shadow distance.
        AABB m_CasterBounds = AABB::Empty();
        /// @brief Entities already reported for an undrawable material.
        vector<Entity> m_Reported;
        /// @brief The last frame's summed statistics.
        SphereTerrainSelectionStats m_Totals;
    };

    /// @brief The generated-texture priority a selection request takes.
    [[nodiscard]] i32 SphereTerrainRequestPriority(bool split, bool inFrustum, f32 urgency);
}
