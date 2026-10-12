#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/MeshLod.h>
#include <Veng/Scene/Visibility.h>

#include "DepthInstancing.h"

namespace Veng::Renderer
{
    class Context;
    struct SceneView;

    /// @brief How the shadow views select a caster's level of detail this frame.
    struct ShadowLodSettings
    {
        /// @brief Levels of bias, as SceneRendererSettings::LodBias.
        f32 Bias = 0.0f;
        /// @brief The forced level every caster draws at, or AutomaticLod to select per view.
        i32 Forced = AutomaticLod;
        /// @brief The camera view's SceneView::LodScreenScale, applied to every shadow view's sizes.
        f32 ScreenScale = 1.0f;
    };

    /// @brief One shadow view as added: what it culls against, measures size in, and its tile size.
    struct ShadowViewInfo
    {
        /// @brief The world -> clip matrix its casters are culled against.
        mat4 CullViewProj{1.0f};
        /// @brief The world -> clip matrix it rasterizes through.
        mat4 LodViewProj{1.0f};
        /// @brief Its tile's edge in texels.
        u32 Resolution = 0;
    };

    /// @brief Every shadow view's casters for one frame: culled per view, grouped once, uploaded once.
    ///
    /// The renderer opens it once per Execute, and each shadow pass then adds its views (the
    /// directional cascades, then the punctual faces) before it is built. AddView culls a view's
    /// casters through the broadphase tree and keeps its static ones in a DepthInstanceBatch shared
    /// by every view, and its skinned ones in a per-view list. Whether a candidate casts at all
    /// (its MeshRenderer flag, an opaque material, a skinned mesh's pose) does not depend on the
    /// view, so it is decided once per candidate per frame, on the first view that reaches it. The
    /// passes then record from the one batch and its one instance-id upload, each view at its own
    /// offset.
    class ShadowCasterViews
    {
    public:
        /// @brief Constructs an empty set of views.
        /// @param context        Renderer context.
        /// @param framesInFlight Number of frames the instance-id buffers span.
        ShadowCasterViews(Context& context, u32 framesInFlight);
        ~ShadowCasterViews();

        ShadowCasterViews(const ShadowCasterViews&) = delete;
        ShadowCasterViews& operator=(const ShadowCasterViews&) = delete;

        /// @brief Clears the previous frame's views and starts this frame's.
        /// @param view        The frame's view: its broadphase, visible meshes and skinning palette.
        /// @param frustumCull Whether views cull; off, every view keeps every caster.
        /// @param lod         How casters select their level of detail.
        /// @pre @p view outlives the frame's AddView and Build calls.
        void Begin(const SceneView& view, bool frustumCull, const ShadowLodSettings& lod);

        /// @brief Adds one shadow view, culling its casters and selecting each one's level.
        ///
        /// A static caster draws at the level the view's own projection selects for it, with no
        /// hysteresis: a shadow view keeps no memory between frames, and its projection holds still
        /// while its light and the caster do.
        /// @param cullViewProj The world → clip matrix whose frustum the casters are culled against.
        /// @param minExtent    A caster whose world bound's diagonal is shorter than this is
        ///                     skipped; 0 skips none.
        /// @param lodViewProj  The world → clip matrix the view rasterizes through, which its
        ///                     level-of-detail selection measures screen size in.
        /// @param resolution   The view's tile edge in texels.
        /// @return The view's index among this frame's views.
        u32 AddView(const mat4& cullViewProj, f32 minExtent, const mat4& lodViewProj,
                    u32 resolution);

        /// @brief Groups every view's static casters and uploads their instance ids.
        /// @param frameIndex The frame in flight being recorded.
        void Build(u32 frameIndex);

        /// @brief The static casters of every view.
        [[nodiscard]] const DepthInstanceBatch& GetBatch() const { return m_Batch; }

        /// @brief A view's skinned caster candidate ids, in broadphase order.
        [[nodiscard]] std::span<const u32> GetSkinned(u32 view) const;

        /// @brief Number of views added since Begin.
        [[nodiscard]] u32 GetViewCount() const { return static_cast<u32>(m_SkinnedEnds.size()); }

        /// @brief Every view added since Begin, in index order.
        [[nodiscard]] std::span<const ShadowViewInfo> GetViews() const { return m_Views; }

        /// @brief How this frame's views select levels of detail.
        [[nodiscard]] const ShadowLodSettings& GetLodSettings() const { return m_Lod; }

    private:
        /// @brief How a candidate takes part in the shadow views, decided once per frame.
        enum class CasterClass : u8
        {
            /// @brief Not yet reached by a view this frame.
            Unclassified,
            /// @brief Casts no shadow: opted out, no opaque material, or a skinned mesh not posed.
            None,
            /// @brief Drawn instanced through the batch.
            Static,
            /// @brief Drawn one at a time through the skinning palette.
            Skinned,
        };

        /// @brief Decides a candidate's class.
        [[nodiscard]] CasterClass Classify(u32 candidateId) const;

        /// @brief The frame's view.
        const SceneView* m_View = nullptr;
        /// @brief The frame's submesh candidates.
        std::span<const SubMeshCandidate> m_Candidates;
        /// @brief Whether views cull this frame.
        bool m_FrustumCull = true;
        /// @brief Whether the frame carries a skinning palette to pose skinned casters with.
        bool m_SkinnedPosed = false;
        /// @brief How casters select their level of detail this frame.
        ShadowLodSettings m_Lod;
        /// @brief Per visible mesh, its level in the view being added; valid where m_LodStamp
        ///        equals that view's index plus one.
        vector<u8> m_ViewLods;
        /// @brief Per visible mesh, which view (index plus one) m_ViewLods was resolved for.
        vector<u32> m_LodStamp;
        /// @brief Per candidate, its class this frame.
        vector<CasterClass> m_Classes;
        /// @brief Frustum-query scratch, reused per view.
        vector<u32> m_CullScratch;
        /// @brief Every view's static casters.
        DepthInstanceBatch m_Batch;
        /// @brief Every view's skinned caster candidate ids, view after view.
        vector<u32> m_Skinned;
        /// @brief Per view, the end of its range in m_Skinned.
        vector<u32> m_SkinnedEnds;
        /// @brief Every view added since Begin.
        vector<ShadowViewInfo> m_Views;
    };
}
