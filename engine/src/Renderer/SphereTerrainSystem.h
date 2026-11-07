#pragma once

#include <array>
#include <unordered_map>
#include <unordered_set>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/SphereTerrain.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/RenderLayer.h>

#include "DrawPlan.h"
#include "SphereTerrainDraw.h"
#include "SphereTerrainSelection.h"
#include "SphereTerrainTransitions.h"

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
        /// @brief The view's render-layer mask (SceneView::VisibleLayers); a terrain whose Layer it
        ///        omits is neither selected nor drawn.
        u32 VisibleLayers = AllRenderLayers;
        /// @brief The cube face the view renders (SceneView::CaptureFaceIndex), or nullopt for a
        ///        flat view.
        ///
        /// A capture renders one face a frame, round-robin, so a selection per face would see a
        /// new frustum every frame and churn its splits, morphs and requests. A cube view instead
        /// selects once per cube cycle, at face 0 (or its first face), with no frustum, and every
        /// face draws that one selection, culled to its own frustum; the six faces never compete
        /// for tiles. Its memory is apart from a flat view's.
        optional<u32> CubeFace;
    };

    /// @brief One renderer's sphere terrains: select each one's tiles for the view, mark and
    ///        request them on its source, cull them per view, and draw them into the g-buffer, the
    ///        translucent pass and the shadow views.
    ///
    /// Each frame Select runs the selection (from the true eye, whatever scale the terrain draws
    /// at) and resolves each terrain's materials; ClaimDrawSlots gives each drawn surface one
    /// DrawData slot for its material and hands each translucent level layer to the frame's
    /// translucent draws; PrepareFrame culls every tile against the main view and every shadow
    /// view and writes the frame slot's records, lists and terrain descriptor sets; the passes
    /// then record one instanced draw of the shared grid per view and surface.
    ///
    /// The selection memory (last frame's splits, the pose, the arrivals and departures, and the
    /// containers each frame refills) is keyed by the source and by whether the view is a cube
    /// face, not by the entity, so a source handed to another entity or scene keeps its hysteresis
    /// and draws what it drew the frame before. A source no Select has seen for a while is
    /// forgotten. A tile's fixed facts and generator words are the pool's (SphereTerrainTileFacts),
    /// computed once while it holds the tile.
    class SphereTerrainSystem
    {
    public:
        /// @brief Creates the shared grid, the shadow pipelines and the per-frame copies.
        static Unique<SphereTerrainSystem> Create(Context& context, AssetManager& assets);

        ~SphereTerrainSystem();

        SphereTerrainSystem(const SphereTerrainSystem&) = delete;
        SphereTerrainSystem& operator=(const SphereTerrainSystem&) = delete;

        /// @brief Selects every visible terrain of a scene whose layer the view draws, and resolves
        ///        what each one draws with.
        /// @param scene  The scene rendered.
        /// @param info   The view.
        void Select(const Scene& scene, const SphereTerrainSelectInfo& info);

        /// @brief The scene-space bound of the shadow-casting tiles within the shadow distance.
        [[nodiscard]] const AABB& GetCasterBounds() const { return m_CasterBounds; }

        /// @brief Gives each drawn surface one draw slot and writes its DrawData record, and
        ///        appends a draw for each seated translucent level layer.
        ///
        /// A translucent layer's draw sorts with the frame's other translucent draws by its
        /// material's priority, then at its sphere's centre distance from the eye, so within a
        /// priority it draws behind every translucent nearer than the centre. Its slot is one of
        /// the slots claimed here, not one of the appended draws'. A layer with a near material
        /// appends a second draw for its near tiles, sorted just nearer than the first, so the
        /// near tiles draw over the far ones.
        /// @param drawData     The frame's DrawData region (already offset by the frame base).
        /// @param budget       The frame's draw budget.
        /// @param translucent  The frame's full-resolution translucent draws, appended to unsorted.
        /// @return The translucent draws appended.
        u32 ClaimDrawSlots(GpuDrawData* drawData, DrawBudget& budget,
                           vector<TranslucentDraw>& translucent);

        /// @brief Culls the selected tiles per view and writes the frame slot's records, lists and
        ///        descriptor sets.
        /// @param frameIndex The frame in flight being recorded.
        /// @param camera     The view's camera.
        /// @param shadows    The frame's shadow views, or null when no shadow pass runs.
        /// @param tileOverlay Whether the records ask the fragment for the tile overlay.
        void PrepareFrame(u32 frameIndex, const CameraView& camera,
                          const ShadowCasterViews* shadows, bool tileOverlay);

        /// @brief Records the main view's terrain draws into the bound g-buffer attachments.
        /// @param cmd          The g-buffer pass's command buffer.
        /// @param push         The frame's surface push (frame base and view constants index).
        /// @param drawDataSet  The DrawData set bound at set 3.
        /// @param shadowReaders Binds each material's pipeline, and the shadow set for one reading
        ///                     the shadow maps.
        void RecordGBuffer(CommandBuffer& cmd, const SurfacePush& push,
                           const Ref<DescriptorSet>& drawDataSet,
                           SurfaceShadowPipelines& shadowReaders) const;

        /// @brief Records one translucent level layer's main-view tiles.
        ///
        /// The translucent pass has bound the layer's pipeline and sets 3 to 5; this binds the
        /// terrain set at TranslucentSphereTerrainSet, pushes the surface push with the layer's
        /// slot and surface, and draws the grid once per tile in the layer's translucent list.
        /// Nothing records when the frame's cull left the layer no tile.
        /// @param cmd      The translucent pass's command buffer.
        /// @param draw     The draw ClaimDrawSlots appended.
        /// @param push     The pass's surface push (frame base and view constants index).
        void RecordTranslucent(CommandBuffer& cmd, const TranslucentDraw& draw,
                               const SurfacePush& push) const;

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

        /// @brief What one source's selection remembers between frames, and the containers each
        ///        frame refills.
        struct Memory
        {
            /// @brief The tiles split last frame.
            std::unordered_set<u64> Splits;
            /// @brief The selection's working containers.
            SphereTerrainSelectionScratch Scratch;
            /// @brief The last selection.
            SphereTerrainSelectionResult Result;
            /// @brief The tiles drawn last frame and the arrivals and departures under way.
            SphereTerrainTransitionState Transitions;
            /// @brief The last selection with its transitions applied: the tiles drawn and their
            ///        floors.
            SphereTerrainTransitionFrame Transition;
            /// @brief The last selection's statistics, transitions applied.
            SphereTerrainSelectionStats Stats;
            /// @brief The frame's records and lists.
            SphereTerrainFrame Frame;
            /// @brief The frame's shadow views in the body-fixed frame.
            vector<SphereTerrainDrawView> ShadowViews;
            /// @brief Whether a cube view's cycle holds a selection the faces after it draw.
            bool CubeSelected = false;
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
            /// @brief Whether it draws in the translucent pass rather than the g-buffer.
            bool Translucent = false;
            /// @brief The resident material its near tiles draw with, or null for none.
            const MaterialInstance* NearMaterial = nullptr;
            /// @brief The drawn distance within which a tile draws with NearMaterial.
            f32 NearDistance = 0.0f;
            /// @brief The near material's DrawData slot, region-relative; valid when NearSeated.
            u32 NearDrawSlot = 0;
            /// @brief Whether ClaimDrawSlots seated the near material.
            bool NearSeated = false;
            /// @brief The resident material its far tiles draw with, or null for none.
            const MaterialInstance* FarMaterial = nullptr;
            /// @brief The drawn metres a pixel spans past which a tile draws with FarMaterial.
            f32 FarFootprint = 0.0f;
            /// @brief The far material's DrawData slot, region-relative; valid when FarSeated.
            u32 FarDrawSlot = 0;
            /// @brief Whether ClaimDrawSlots seated the far material.
            bool FarSeated = false;
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
            /// @brief Per surface, its translucent list's first entry and count in the list buffer.
            std::array<uvec2, SphereTerrainSurfaceCount> TranslucentLists{};
            /// @brief Per surface, its main-view near list's first entry and count.
            std::array<uvec2, SphereTerrainSurfaceCount> NearLists{};
            /// @brief Per surface, its translucent near list's first entry and count.
            std::array<uvec2, SphereTerrainSurfaceCount> NearTranslucentLists{};
            /// @brief Per surface, its main-view far list's first entry and count.
            std::array<uvec2, SphereTerrainSurfaceCount> FarLists{};
            /// @brief Per surface, its translucent far list's first entry and count.
            std::array<uvec2, SphereTerrainSurfaceCount> FarTranslucentLists{};
            /// @brief The frame slot's set-4 descriptor set it binds.
            const DescriptorSet* Set = nullptr;
            /// @brief What its frame was culled from, kept for its records' write.
            SphereTerrainFrameInput Input;
            /// @brief Its first record in the frame slot's record buffer.
            u32 RecordBase = 0;
        };

        /// @brief One source's set-4 set in a frame slot, and what it was written with.
        struct SlotSet
        {
            /// @brief The set.
            Ref<DescriptorSet> Set;
            /// @brief The record buffer it names.
            const Buffer* Records = nullptr;
            /// @brief The list buffer it names.
            const Buffer* Lists = nullptr;
            /// @brief The source's view generation its channel bindings were written at.
            u64 ViewGeneration = ~0ull;
            /// @brief The last prepare that bound it.
            u64 LastPrepared = 0;
        };

        /// @brief One frame in flight's copies.
        struct FrameSlot
        {
            /// @brief The records.
            Ref<Buffer> Records;
            /// @brief The lists.
            Ref<Buffer> Lists;
            /// @brief Per source serial, the set-4 set bound this slot.
            std::unordered_map<u64, SlotSet> Sets;
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
        /// @brief The smallest angle one of the selected view's pixels subtends, in radians.
        f64 m_MinPixelAngle = 0.0;
        /// @brief The selected view's eye in the stable frame.
        dvec3 m_EyeStable{0.0};
        /// @brief The shadow views the frame prepared lists for.
        u32 m_ShadowViewCount = 0;

        /// @brief Per source serial and view kind (MemoryKey), its selection memory.
        std::unordered_map<u64, Memory> m_Memory;
        /// @brief Per entity value, its last selection (in its memory) and statistics.
        std::unordered_map<
            u64, std::pair<const vector<SphereTerrainDrawnTile>*, SphereTerrainSelectionStats>>
            m_Selections;
        /// @brief This frame's selected terrains; elements past the frame's count are dropped, the
        ///        rest reused so their lists keep their capacity.
        vector<Terrain> m_Terrains;
        /// @brief How many of m_Terrains this frame selected.
        usize m_TerrainCount = 0;
        /// @brief The frame's list entries, refilled each prepare.
        vector<u32> m_ListEntries;
        /// @brief Counts the prepares, which stamp the slot sets bound.
        u64 m_PrepareSerial = 0;
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
