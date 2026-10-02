// Depth-caster grouping across several views. A depth pass groups the union of every view's
// casters once and has each view filter that grouping through its mask; the property is that a
// view's draws come out exactly as they would from grouping that view's casters alone. The scene
// is a row of casters along one axis, culled through the broadphase by adjacent orthographic
// slabs the way cascades split a light's depth, with casters placed across the slab boundaries so
// several of them belong to two views. Device-free: the meshes carry bounds and index ranges only.

#include <doctest/doctest.h>

#include <algorithm>
#include <span>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneBroadphase.h>
#include <Veng/Task/TaskSystem.h>

#include <glm/gtc/matrix_transform.hpp>

#include "Renderer/DepthInstancing.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    void RegisterBuiltins(TypeRegistry& types)
    {
        types.Register<Name>("Name");
        types.Register<Transform>("Transform");
        types.Register<Hierarchy>("Hierarchy");
        types.Register<MeshRenderer>("MeshRenderer");
    }

    // A device-free mesh: one submesh per index range given, every submesh carrying the unit box.
    Ref<Mesh> RangesMesh(const std::span<const uvec2> ranges)
    {
        const AABB box{.Min = vec3(-0.5f), .Max = vec3(0.5f)};
        vector<SubMesh> subMeshes;
        for (const uvec2 range : ranges)
        {
            subMeshes.push_back(
                SubMesh{.IndexOffset = range.x, .IndexCount = range.y, .Bounds = box});
        }
        return Mesh::Create(MeshInfo{.Name = "ranges", .SubMeshes = subMeshes, .Bounds = box});
    }

    // One draw as a pass records it: the index range and the caster records it instances.
    struct Draw
    {
        const Mesh* SourceMesh;
        u32 IndexCount;
        u32 FirstIndex;
        vector<u32> Records;

        bool operator==(const Draw&) const = default;
    };

    vector<Draw> ViewDraws(const DepthCasterGrouping& grouping, const u32 view)
    {
        const std::span<const u32> ids = grouping.GetInstanceIds();
        vector<Draw> draws;
        for (const DepthDraw& draw : grouping.GetViewDraws(view))
        {
            const std::span<const u32> records =
                ids.subspan(draw.FirstInstance, draw.InstanceCount);
            draws.push_back(Draw{
                .SourceMesh = draw.SourceMesh,
                .IndexCount = draw.IndexCount,
                .FirstIndex = draw.FirstIndex,
                .Records = vector<u32>(records.begin(), records.end()),
            });
        }
        return draws;
    }

    // Adds one view's broadphase survivors to the grouping, as a shadow view keeps its casters.
    void AddSurvivors(DepthCasterGrouping& grouping, const u32 view,
                      const SceneBroadphase& broadphase, const std::span<const u32> survivors)
    {
        const std::span<const SubMeshCandidate> candidates = broadphase.GetSubMeshCandidates();
        for (const u32 id : survivors)
        {
            const SubMeshCandidate& c = candidates[id];
            grouping.Add(view, id, *broadphase.GetCandidates()[c.MeshCandidate].Mesh,
                         c.SubMeshIndex);
        }
    }
}

TEST_CASE("depth caster grouping: grouping every view once draws each view as grouping it alone")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);
    const AssetManager manager(context, tasks, types);
    const Unique<Scene> scene = Scene::Create(types);

    const std::array<uvec2, 1> single{uvec2(0, 36)};
    const std::array<uvec2, 2> split{uvec2(0, 12), uvec2(12, 24)};
    const AssetHandle<Mesh> meshA = manager.Adopt<Mesh>(RangesMesh(single));
    const AssetHandle<Mesh> meshB = manager.Adopt<Mesh>(RangesMesh(split));

    // Depth along -z; the slab boundaries below sit at 10, 20 and 30, and the unit boxes at
    // 9.8, 10, 10.2, 19.9, 20 and 29.7 straddle them.
    const std::array<vec3, 10> positions{
        vec3(-3.0f, 0.0f, -5.0f),  vec3(3.0f, 0.0f, -9.8f),  vec3(0.0f, 2.0f, -10.0f),
        vec3(-6.0f, 0.0f, -10.2f), vec3(1.0f, 1.0f, -15.0f), vec3(4.0f, -2.0f, -19.9f),
        vec3(-2.0f, 3.0f, -20.0f), vec3(5.0f, 5.0f, -25.0f), vec3(0.0f, 0.0f, -29.7f),
        vec3(7.0f, 1.0f, -14.0f),
    };
    // Created deepest first, so a view's casters do not join the shared list in the order the
    // view alone would sort them.
    for (usize i = 0; i < positions.size(); ++i)
    {
        const Entity e = scene->CreateEntity();
        scene->Add<Transform>(e, Transform{.Position = positions[positions.size() - 1 - i]});
        scene->Add<MeshRenderer>(e, MeshRenderer{.Mesh = i % 3 == 0 ? meshB : meshA});
    }

    SceneBroadphase broadphase;
    broadphase.Sync(*scene);
    REQUIRE(broadphase.GetCandidates().size() == positions.size());

    // Three adjacent slabs, then one past every caster, which keeps nothing.
    const std::array<vec2, 4> slabs{vec2(0.0f, 10.0f), vec2(10.0f, 20.0f), vec2(20.0f, 30.0f),
                                    vec2(40.0f, 50.0f)};
    vector<vector<u32>> survivors(slabs.size());
    for (usize v = 0; v < slabs.size(); ++v)
    {
        broadphase.Cull(Frustum::FromViewProjection(
                            glm::ortho(-20.0f, 20.0f, -20.0f, 20.0f, slabs[v].x, slabs[v].y)),
                        survivors[v]);
    }

    DepthCasterGrouping shared;
    shared.Begin(broadphase.GetSubMeshCandidates());
    for (usize v = 0; v < slabs.size(); ++v)
    {
        const u32 view = shared.AddView();
        AddSurvivors(shared, view, broadphase, survivors[v]);
    }
    shared.Build();
    REQUIRE(shared.GetViewCount() == slabs.size());

    u32 straddlers = 0;
    for (u32 id = 0; id < broadphase.GetSubMeshCandidates().size(); ++id)
    {
        const auto keeps = [id](const vector<u32>& kept)
        { return std::ranges::find(kept, id) != kept.end(); };
        straddlers += std::ranges::count_if(survivors, keeps) > 1 ? 1 : 0;
    }
    // The scene is what the property needs: casters kept by two views, instanced runs.
    REQUIRE(straddlers >= 3);

    for (u32 v = 0; v < slabs.size(); ++v)
    {
        DepthCasterGrouping alone;
        alone.Begin(broadphase.GetSubMeshCandidates());
        AddSurvivors(alone, alone.AddView(), broadphase, survivors[v]);
        alone.Build();

        const vector<Draw> fromShared = ViewDraws(shared, v);
        CHECK(fromShared == ViewDraws(alone, 0));

        // Every caster the view kept is drawn exactly once in it.
        vector<u32> drawn;
        for (const Draw& draw : fromShared)
        {
            drawn.insert(drawn.end(), draw.Records.begin(), draw.Records.end());
        }
        vector<u32> kept;
        for (const u32 id : survivors[v])
        {
            kept.push_back(broadphase.GetSubMeshCandidates()[id].MeshCandidate);
        }
        std::ranges::sort(drawn);
        std::ranges::sort(kept);
        CHECK(drawn == kept);
    }
    CHECK(shared.GetViewDraws(3).empty());
}
