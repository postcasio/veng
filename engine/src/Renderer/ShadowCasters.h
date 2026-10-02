#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Scene/Visibility.h>

#include "DepthInstancing.h"

namespace Veng::Renderer
{
    class Context;
    struct SceneView;

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
        /// @pre @p view outlives the frame's AddView and Build calls.
        void Begin(const SceneView& view, bool frustumCull);

        /// @brief Adds one shadow view, culling its casters.
        /// @param cullViewProj The world → clip matrix whose frustum the casters are culled against.
        /// @param minExtent    A caster whose world bound's diagonal is shorter than this is
        ///                     skipped; 0 skips none.
        /// @return The view's index among this frame's views.
        u32 AddView(const mat4& cullViewProj, f32 minExtent);

        /// @brief Groups every view's static casters and uploads their instance ids.
        /// @param frameIndex The frame in flight being recorded.
        void Build(u32 frameIndex);

        /// @brief The static casters of every view.
        [[nodiscard]] const DepthInstanceBatch& GetBatch() const { return m_Batch; }

        /// @brief A view's skinned caster candidate ids, in broadphase order.
        [[nodiscard]] std::span<const u32> GetSkinned(u32 view) const;

        /// @brief Number of views added since Begin.
        [[nodiscard]] u32 GetViewCount() const { return static_cast<u32>(m_SkinnedEnds.size()); }

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
    };
}
