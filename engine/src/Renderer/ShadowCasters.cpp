#include "ShadowCasters.h"

#include "DrawGather.h"
#include "LodSelection.h"

#include <Veng/Asset/Mesh.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Renderer/SceneView.h>
#include <Veng/Scene/SceneBroadphase.h>

namespace Veng::Renderer
{
    ShadowCasterViews::ShadowCasterViews(Context& context, const u32 framesInFlight)
        : m_Batch(context, "Shadow Depth", framesInFlight)
    {
    }

    ShadowCasterViews::~ShadowCasterViews() = default;

    void ShadowCasterViews::Begin(const SceneView& view, const bool frustumCull,
                                  const ShadowLodSettings& lod)
    {
        m_View = &view;
        m_Lod = lod;
        m_ViewLods.assign(view.Visible.size(), 0);
        m_LodStamp.assign(view.Visible.size(), 0);
        m_Candidates = view.Broadphase->GetSubMeshCandidates();
        m_FrustumCull = frustumCull;
        m_SkinnedPosed = view.SkinningPalette != nullptr && view.SkinnedPaletteBases != nullptr;
        m_Classes.assign(m_Candidates.size(), CasterClass::Unclassified);
        m_Batch.Begin(m_Candidates);
        m_Skinned.clear();
        m_SkinnedEnds.clear();
    }

    ShadowCasterViews::CasterClass ShadowCasterViews::Classify(const u32 candidateId) const
    {
        const SubMeshCandidate& c = m_Candidates[candidateId];
        const VisibleMesh& item = m_View->Visible[c.MeshCandidate];
        if (!item.CastsShadows || !CastsShadow(item.Materials, *item.Mesh, c.SubMeshIndex))
        {
            return CasterClass::None;
        }
        if (!item.Mesh->IsSkinned())
        {
            return CasterClass::Static;
        }
        return m_SkinnedPosed ? CasterClass::Skinned : CasterClass::None;
    }

    u32 ShadowCasterViews::AddView(const mat4& cullViewProj, const f32 minExtent,
                                   const mat4& lodViewProj)
    {
        VE_PROFILE_SCOPE("Shadow/Cull");
        const u32 viewIndex = m_Batch.AddView();
        LodView lodView = LodView::FromViewProjection(lodViewProj, m_Lod.Bias);
        lodView.ScreenScale *= m_Lod.ScreenScale;
        const LodPolicy lodPolicy{.Forced = m_Lod.Forced, .Hysteresis = 0.0f};

        m_CullScratch.clear();
        if (m_FrustumCull)
        {
            m_View->Broadphase->Cull(Frustum::FromViewProjection(cullViewProj), m_CullScratch);
        }
        else
        {
            for (u32 i = 0; i < m_Candidates.size(); ++i)
            {
                m_CullScratch.push_back(i);
            }
        }

        for (const u32 id : m_CullScratch)
        {
            CasterClass& casterClass = m_Classes[id];
            if (casterClass == CasterClass::Unclassified)
            {
                casterClass = Classify(id);
            }
            if (casterClass == CasterClass::None)
            {
                continue;
            }
            const SubMeshCandidate& c = m_Candidates[id];
            const VisibleMesh& item = m_View->Visible[c.MeshCandidate];
            if (glm::length(item.WorldBounds.Size()) < minExtent)
            {
                continue;
            }
            if (casterClass == CasterClass::Static)
            {
                // One selection per mesh per view, shared by its submeshes.
                if (m_LodStamp[c.MeshCandidate] != viewIndex + 1)
                {
                    m_LodStamp[c.MeshCandidate] = viewIndex + 1;
                    m_ViewLods[c.MeshCandidate] =
                        static_cast<u8>(ResolveMeshLod(item, lodView, lodPolicy, NoPreviousLod));
                }
                const u32 lod = m_ViewLods[c.MeshCandidate];
                if (item.Mesh->GetLodRange(lod, c.SubMeshIndex).IndexCount > 0)
                {
                    m_Batch.Add(viewIndex, id, *item.Mesh, c.SubMeshIndex, lod);
                }
            }
            else
            {
                m_Skinned.push_back(id);
            }
        }
        m_SkinnedEnds.push_back(static_cast<u32>(m_Skinned.size()));
        return viewIndex;
    }

    void ShadowCasterViews::Build(const u32 frameIndex)
    {
        {
            VE_PROFILE_SCOPE("Shadow/Instance");
            m_Batch.Build();
        }
        VE_PROFILE_SCOPE("Shadow/Upload");
        m_Batch.Upload(frameIndex);
    }

    std::span<const u32> ShadowCasterViews::GetSkinned(const u32 view) const
    {
        const u32 begin = view == 0 ? 0 : m_SkinnedEnds[view - 1];
        return std::span<const u32>(m_Skinned).subspan(begin, m_SkinnedEnds[view] - begin);
    }
}
