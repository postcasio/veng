#include "SphereTerrainSystem.h"

#include "SurfaceShadowPipelines.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "DrawBudget.h"
#include "GpuBlocks.h"
#include "SceneFloatingOrigin.h"
#include "ShadowCasters.h"
#include "SphereTerrainLayout.h"
#include "SphereTerrainResidency.h"
#include "SphereTerrainSelection.h"

namespace Veng::Renderer
{
    namespace
    {
        constexpr AssetId ShadowDepthTerrainVertId{0xBB4E25CF3570CDD5ULL};

        // Shadow depth targets: the cascade and punctual atlases are both D32.
        constexpr Format ShadowFormat = Format::D32Sfloat;

        // shadow_depth_terrain.vert's push block.
        struct ShadowTerrainPush
        {
            mat4 ViewProj;
            vec4 Camera;
            u32 Surface;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };
        static_assert(sizeof(ShadowTerrainPush) == 96, "ShadowTerrainPush must match the shader");

        // The plane that admits everything, standing in for a plane a view does not cull by.
        constexpr glm::dvec4 Open{0.0, 0.0, 0.0, 1.0};

        u64 EntityValue(const Entity entity)
        {
            return (static_cast<u64>(entity.Generation) << 32) | entity.Index;
        }

        u64 GrownSize(const u64 needed, const u64 floor)
        {
            u64 size = floor;
            while (size < needed)
            {
                size *= 2;
            }
            return size;
        }

        // Scene-space planes rewritten about the eye in the body-fixed frame:
        // n.(cam + R q) + w = (R^T n).q + (n.cam + w). Planes past @p count admit everything.
        std::array<glm::dvec4, 6> BodyPlanes(const Frustum& frustum, const usize count,
                                             const glm::dquat& toBody, const dvec3& cameraScene)
        {
            std::array<glm::dvec4, 6> planes{};
            planes.fill(Open);
            for (usize i = 0; i < count; i++)
            {
                const dvec3 normal = dvec3(vec3(frustum.Planes[i]));
                planes[i] = glm::dvec4(toBody * normal,
                                       glm::dot(normal, cameraScene) + frustum.Planes[i].w);
            }
            return planes;
        }

        void Accumulate(SphereTerrainSelectionStats& total, const SphereTerrainSelectionStats& add)
        {
            total.DrawnTiles += add.DrawnTiles;
            for (usize level = 0; level < total.DrawnPerLevel.size(); level++)
            {
                total.DrawnPerLevel[level] += add.DrawnPerLevel[level];
            }
            total.WaitingSplits += add.WaitingSplits;
            total.HeldForBalance += add.HeldForBalance;
            total.SplitForBalance += add.SplitForBalance;
            total.HeldForBudget += add.HeldForBudget;
            total.HeldForFrustum += add.HeldForFrustum;
            total.BudgetedTiles += add.BudgetedTiles;
            total.TileBudget = std::min(total.TileBudget, add.TileBudget);
            total.Requests += add.Requests;
            total.Visited += add.Visited;
            total.MainViewTiles += add.MainViewTiles;
            total.CulledByFrustum += add.CulledByFrustum;
            total.CulledByHorizon += add.CulledByHorizon;
            total.ShadowViewTiles += add.ShadowViewTiles;
            total.Triangles += add.Triangles;
            total.ArrivingTiles += add.ArrivingTiles;
            total.SkippedTerrains += add.SkippedTerrains;
        }

        // The tiles a source's pool can give a renderer's selection: its capacity less the pinned
        // top and a reserve for the tiles a moving eye stops wanting, which stay unevictable for
        // two frames.
        u32 TileBudget(const SphereTerrainSource& source)
        {
            const u32 pinnedLevel = source.GetPinnedLevel();
            const u32 pinned = 2u * ((1u << (2u * (pinnedLevel + 1u))) - 1u);
            const u32 capacity = source.GetCapacity();
            const u32 reserve = std::max(16u, capacity / 16u);
            return capacity > pinned + reserve ? capacity - pinned - reserve : 0u;
        }
    }

    i32 SphereTerrainRequestPriority(const bool split, const bool inFrustum, const f32 urgency)
    {
        // Splits the view waits on first, those in view ahead of those behind, each ordered by how
        // far its parent's error exceeds the threshold; prefetch last.
        if (!split)
        {
            return inFrustum ? 1000 : 0;
        }
        const i32 scaled = static_cast<i32>(std::clamp(urgency, 0.0f, 100.0f) * 1000.0f);
        return (inFrustum ? 400000 : 200000) + scaled;
    }

    Unique<SphereTerrainSystem> SphereTerrainSystem::Create(Context& context, AssetManager& assets)
    {
        return Unique<SphereTerrainSystem>(new SphereTerrainSystem(context, assets));
    }

    SphereTerrainSystem::SphereTerrainSystem(Context& context, AssetManager& assets)
        : m_Context(context)
    {
        const std::array<u16, SphereTerrainGridIndexCount> indices =
            BuildSphereTerrainGridIndices();
        m_GridIndices = Buffer::Create(context, {
                                                    .Name = "Sphere Terrain Grid Indices",
                                                    .Size = sizeof(indices),
                                                    .Usage = BufferUsage::Index,
                                                    .HostMapped = true,
                                                });
        std::memcpy(m_GridIndices->GetMappedData(), indices.data(), sizeof(indices));

        m_SetLayout = CreateSphereTerrainSetLayout(context);
        const Ref<DescriptorSetLayout> none = DescriptorSetLayout::Create(
            context, {.Name = "Sphere Terrain Shadow Set 3 Layout", .Bindings = {}});
        m_ShadowLayout = PipelineLayout::Create(
            context, {
                         .Name = "Sphere Terrain Shadow Layout",
                         .DescriptorSetLayouts = {none, m_SetLayout},
                         .PushConstantRanges = {PushConstantRange::Of<ShadowTerrainPush>(
                             ShaderStage::Vertex)},
                     });
        m_ShadowPipeline = BuildShadowPipeline(assets, false);
        m_ShadowClampPipeline =
            context.IsDepthClampSupported() ? BuildShadowPipeline(assets, true) : m_ShadowPipeline;
        m_Slots.resize(context.GetMaxFramesInFlight());
    }

    SphereTerrainSystem::~SphereTerrainSystem() = default;

    Ref<GraphicsPipeline> SphereTerrainSystem::BuildShadowPipeline(AssetManager& assets,
                                                                   const bool depthClamp)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthTerrainVertId);
        VE_ASSERT(vs.has_value(), "SphereTerrainSystem: shadow vertex shader load failed: {}",
                  vs.has_value() ? "" : vs.error().Detail);
        // Terrain is an open surface rather than a closed caster, so both faces cast: the face
        // toward the light stands in front of whatever it shades.
        return GraphicsPipeline::Create(
            m_Context,
            {
                .Name = depthClamp ? "Sphere Terrain Shadow Clamp Pipeline"
                                   : "Sphere Terrain Shadow Pipeline",
                .ColorAttachments = {},
                .DepthAttachmentFormat = ShadowFormat,
                .PipelineLayout = m_ShadowLayout,
                .ShaderStages = {{.Stage = ShaderStage::Vertex, .Module = vs->Get()->Module}},
                .CullMode = CullMode::None,
                .DepthTestEnable = true,
                .DepthWriteEnable = true,
                .DepthClampEnable = depthClamp,
            });
    }

    void SphereTerrainSystem::Report(const Entity entity, const string& why)
    {
        if (std::ranges::find(m_Reported, entity) != m_Reported.end())
        {
            return;
        }
        m_Reported.push_back(entity);
        Log::Error("SphereTerrain on entity {}: {}.", entity.Index, why);
    }

    void SphereTerrainSystem::Select(const Scene& scene, const SphereTerrainSelectInfo& info)
    {
        VE_PROFILE_SCOPE("Render/SphereTerrainSelect");
        VE_ASSERT(info.Camera != nullptr, "SphereTerrainSystem::Select needs the view's camera");
        m_Selections.clear();
        m_Terrains.clear();
        m_Totals = {};
        m_CasterBounds = AABB::Empty();
        m_Prepared = false;
        m_Claimable = true;

        const CameraView& camera = *info.Camera;
        const FloatingOrigin origin = SceneFloatingOrigin(scene);
        m_FrameOrigin = origin.Position;
        const dvec3 cameraScene = dvec3(camera.GetPosition());
        const dvec3 eyeStable = origin.Position + cameraScene;
        const mat4 projection = camera.Projection();
        const f64 pixelsPerRadian = 0.5 * info.HeightPixels * std::abs(projection[1][1]);
        const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());

        for (auto [entity, terrain] : scene.View<SphereTerrain>())
        {
            if (!terrain.Visible || terrain.Source == nullptr)
            {
                continue;
            }
            SphereTerrainSource& source = *terrain.Source;
            const glm::dquat toBody = glm::inverse(terrain.Orientation);
            // The side planes alone: where the near and far planes stand, which a scale about the
            // eye moves, has no say in what is selected.
            const std::array<glm::dvec4, 6> planes = BodyPlanes(frustum, 4, toBody, cameraScene);

            SphereTerrainSelectionInput input{
                .Radius = source.GetRadius(),
                .MinHeight = source.GetMinHeight(),
                .MaxHeight = source.GetMaxHeight(),
                .MaxLevel = source.GetMaxLevel(),
                .Eye = toBody * (eyeStable - terrain.Center),
                .PixelsPerRadian = pixelsPerRadian,
                .TargetPixelsPerQuad = std::max(terrain.TargetPixelsPerQuad, 1.0f),
                .Frustum = &planes,
                .PinnedLevel = source.GetPinnedLevel(),
                .TileBudget = TileBudget(source),
                .Tiles = &source.GetResidency(),
            };
            input.Settings.OmnidirectionalRadius =
                std::max(input.Settings.OmnidirectionalRadius, info.ShadowDistance);
            for (u32 level = 0; level <= CubeSphere::MaxLevel; level++)
            {
                input.LevelHeightBounds[level] = source.GetGenerator().GetLevelHeightBound(level);
            }

            Memory& memory = m_Memory[source.GetSerial()];
            memory.LastFrame = info.Frame;
            input.PreviousSplits = &memory.Splits;

            SphereTerrainSelectionResult result = SelectSphereTerrain(input);
            for (const CubeSphereTileKey key : result.Wanted)
            {
                source.Want(key);
            }
            for (const SphereTerrainSelectionRequest& request : result.Requests)
            {
                source.Request(request.Key, SphereTerrainRequestPriority(
                                                request.Kind == SphereTerrainRequestKind::Split,
                                                request.InFrustum, request.Urgency));
            }
            memory.Splits = std::move(result.Splits);

            // A tile drawn this frame under one drawn last frame arrives: a split taken past its
            // distance would otherwise show its children short of their parent's shape the frame
            // they land, so their morph starts from the parent's and runs down to their own.
            memory.Clock += info.Delta;
            std::unordered_set<u64> drawn;
            drawn.reserve(result.Drawn.size());
            for (const SphereTerrainDrawnTile& tile : result.Drawn)
            {
                drawn.insert(tile.Key.Value);
                if (memory.Drawn.empty() || memory.Drawn.contains(tile.Key.Value) ||
                    memory.Arrivals.contains(tile.Key.Value))
                {
                    continue;
                }
                for (CubeSphereTileKey up = tile.Key; up.GetLevel() > 0;)
                {
                    up = up.GetParent();
                    if (memory.Drawn.contains(up.Value))
                    {
                        memory.Arrivals[tile.Key.Value] = memory.Clock;
                        break;
                    }
                }
            }
            memory.Floors.clear();
            std::erase_if(memory.Arrivals,
                          [&](const auto& arrival)
                          {
                              const f64 floor = 1.0 - ((memory.Clock - arrival.second) /
                                                       SphereTerrainArrivalSeconds);
                              if (floor <= 0.0 || !drawn.contains(arrival.first))
                              {
                                  return true;
                              }
                              memory.Floors[arrival.first] = static_cast<f32>(floor);
                              return false;
                          });
            // The generator's words are kept for the tiles drawn and the parents they read.
            std::erase_if(memory.TileData,
                          [&](const auto& entry)
                          {
                              const CubeSphereTileKey key =
                                  CubeSphereTileKey::FromValue(entry.first);
                              const bool parents =
                                  key.GetLevel() < CubeSphere::MaxLevel &&
                                  std::ranges::any_of(
                                      std::array{0u, 1u, 2u, 3u}, [&](const u32 c)
                                      { return drawn.contains(key.GetChild(c).Value); });
                              return !drawn.contains(entry.first) && !parents;
                          });
            memory.Drawn = std::move(drawn);
            result.Stats.ArrivingTiles = static_cast<u32>(memory.Floors.size());

            Terrain entry{
                .Owner = entity,
                .Source = terrain.Source,
                .Mem = &memory,
                .Center = terrain.Center,
                .Orientation = terrain.Orientation,
                .Scale = terrain.Scale,
                .CastsShadows = terrain.CastsShadows,
            };
            const auto Resolve = [&](const AssetHandle<MaterialInstance>& handle,
                                     const string& what) -> const MaterialInstance*
            {
                if (!handle.IsLoaded())
                {
                    return nullptr;
                }
                const MaterialInstance* material = handle.Get();
                const Material& parent = *material->GetParent().Get();
                if (parent.GetDomain() != MaterialDomain::Surface || !parent.IsSphereTerrain())
                {
                    Report(entity, fmt::format("{} material '{}' does not declare "
                                               "\"sphereTerrain\", so it is not drawn",
                                               what, parent.GetName()));
                    return nullptr;
                }
                return material;
            };
            entry.Surfaces[0].Material = Resolve(terrain.Material, "its");
            result.Stats.SkippedTerrains = entry.Surfaces[0].Material == nullptr ? 1u : 0u;
            if (terrain.LevelLayers.size() > MaxSphereTerrainLevelLayers)
            {
                Report(entity,
                       fmt::format("it carries {} level layers, past the {} drawn",
                                   terrain.LevelLayers.size(), MaxSphereTerrainLevelLayers));
            }
            entry.LayerCount = static_cast<u32>(
                std::min<usize>(terrain.LevelLayers.size(), MaxSphereTerrainLevelLayers));
            for (u32 layer = 0; layer < entry.LayerCount; layer++)
            {
                Surface& surface = entry.Surfaces[1 + layer];
                surface.Height = terrain.LevelLayers[layer].Height;
                surface.Material = Resolve(terrain.LevelLayers[layer].Material,
                                           fmt::format("its layer {}", layer));
            }

            // The cascades fit to the casters near the eye, not to a terrain reaching the horizon.
            if (terrain.Scale == 1.0f && terrain.CastsShadows && info.ShadowDistance > 0.0f)
            {
                const dvec3 centerScene = terrain.Center - origin.Position;
                for (const SphereTerrainDrawnTile& tile : result.Drawn)
                {
                    const SphereTerrainTileBox box = ComputeSphereTerrainTileBox(
                        tile.Key, source.GetRadius(), tile.MinHeight, tile.MaxHeight);
                    if (SphereTerrainBoxDistance(box, input.Eye) > info.ShadowDistance)
                    {
                        continue;
                    }
                    for (const dvec3& corner : SphereTerrainBoxCorners(box))
                    {
                        m_CasterBounds.Expand(vec3(centerScene + (terrain.Orientation * corner)));
                    }
                }
            }

            Accumulate(m_Totals, result.Stats);
            m_Selections[EntityValue(entity)] = {std::move(result.Drawn), result.Stats};
            m_Terrains.push_back(std::move(entry));
        }

        std::erase_if(m_Memory, [&](const auto& entry)
                      { return entry.second.LastFrame + MemoryFrames < info.Frame; });

        VE_PROFILE_COUNTER("Render/SphereTerrainDrawn", static_cast<f64>(m_Totals.DrawnTiles));
        VE_PROFILE_COUNTER("Render/SphereTerrainWaitingSplits",
                           static_cast<f64>(m_Totals.WaitingSplits));
    }

    void SphereTerrainSystem::ClaimDrawSlots(GpuDrawData* drawData, DrawBudget& budget)
    {
        // A frame that did not select (the lean depth-and-normal path) draws no terrain.
        if (!m_Claimable)
        {
            return;
        }
        m_Claimable = false;
        u32 wanted = 0;
        for (const Terrain& terrain : m_Terrains)
        {
            for (const Surface& surface : terrain.Surfaces)
            {
                wanted += surface.Material != nullptr ? 1u : 0u;
            }
        }
        for (Terrain& terrain : m_Terrains)
        {
            for (Surface& surface : terrain.Surfaces)
            {
                if (surface.Material == nullptr)
                {
                    continue;
                }
                u32 slot = 0;
                if (!budget.TryClaimSlot(slot))
                {
                    budget.RecordDropped(DrawPhase::SphereTerrain, wanted);
                    return;
                }
                wanted--;
                const mat4 identity(1.0f);
                drawData[slot] = GpuDrawData{
                    .World = identity,
                    .NormalColumn0 = vec4(1.0f, 0.0f, 0.0f, 0.0f),
                    .NormalColumn1 = vec4(0.0f, 1.0f, 0.0f, 0.0f),
                    .NormalColumn2 = vec4(0.0f, 0.0f, 1.0f, 0.0f),
                    .MaterialOffset = surface.Material->GetMaterialSelector(),
                    .EntityIndex = terrain.Owner.Index,
                    .PrevWorld = identity,
                };
                surface.DrawSlot = slot;
                surface.Seated = true;
            }
        }
    }

    void SphereTerrainSystem::PrepareFrame(const u32 frameIndex, const CameraView& camera,
                                           const ShadowCasterViews* shadows, const bool tileOverlay)
    {
        VE_PROFILE_SCOPE("Render/SphereTerrainPrepare");
        m_FrameIndex = frameIndex;
        m_Prepared = true;
        FrameSlot& slot = m_Slots[frameIndex];
        m_CameraPosition = camera.GetPosition();
        const dvec3 cameraScene = dvec3(m_CameraPosition);
        const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());
        const std::span<const ShadowViewInfo> shadowViews =
            shadows != nullptr ? shadows->GetViews() : std::span<const ShadowViewInfo>{};
        m_ShadowViewCount = static_cast<u32>(shadowViews.size());
        vector<Frustum> shadowFrusta;
        shadowFrusta.reserve(shadowViews.size());
        for (const ShadowViewInfo& view : shadowViews)
        {
            shadowFrusta.push_back(Frustum::FromViewProjection(view.CullViewProj));
        }

        vector<SphereTerrainDrawRecord> records;
        vector<u32> lists;
        vector<SphereTerrainDrawView> bodyShadows(shadowViews.size());
        for (Terrain& terrain : m_Terrains)
        {
            terrain.Lists.clear();
            terrain.Set = nullptr;
            Memory& memory = *terrain.Mem;
            const bool anySurface = std::ranges::any_of(terrain.Surfaces, [](const Surface& surface)
                                                        { return surface.Material != nullptr; });
            const auto selection = m_Selections.find(EntityValue(terrain.Owner));
            if (!anySurface || selection == m_Selections.end())
            {
                memory.HasPrevious = false;
                continue;
            }
            const SphereTerrainSource& source = *terrain.Source;
            const glm::dquat toBody = glm::inverse(terrain.Orientation);

            SphereTerrainFrameInput input{
                .Drawn = selection->second.first,
                .Radius = source.GetRadius(),
                .MinHeight = source.GetMinHeight(),
                .MaxHeight = source.GetMaxHeight(),
                .Orientation = terrain.Orientation,
                .Scale = terrain.Scale,
                .CastsShadows = terrain.CastsShadows,
                .LayerCount = terrain.LayerCount,
                .ArrivalFloors = &memory.Floors,
                .TileOverlay = tileOverlay,
            };
            for (u32 layer = 0; layer < terrain.LayerCount; layer++)
            {
                input.LayerHeights[layer] = terrain.Surfaces[1 + layer].Height;
            }
            input.Main = {.Planes = BodyPlanes(frustum, 4, toBody, cameraScene), .Horizon = true};
            for (usize v = 0; v < shadowFrusta.size(); v++)
            {
                bodyShadows[v] = {.Planes = BodyPlanes(shadowFrusta[v], 6, toBody, cameraScene)};
            }
            input.Shadows = bodyShadows;

            // The stable frame and the scene share axes, so the eye-relative corners need only the
            // camera's scene position and the origin the scene stands at.
            const dvec3 origin = m_FrameOrigin;
            input.Eye = toBody * (origin + cameraScene - terrain.Center);
            input.CenterFromEye = terrain.Center - origin - cameraScene;
            input.PrevCenterFromCamera = (memory.HasPrevious ? memory.PrevCenter - memory.PrevOrigin
                                                             : terrain.Center - origin) -
                                         cameraScene;
            input.PrevOrientation =
                memory.HasPrevious ? memory.PrevOrientation : terrain.Orientation;
            SphereTerrainGenerator& generator = source.GetGenerator();
            input.TileData =
                [&memory, &generator](const CubeSphereTileKey key, const std::span<u32> words)
            {
                auto [it, inserted] = memory.TileData.try_emplace(key.Value);
                if (inserted)
                {
                    generator.WriteTileDrawData(key, it->second);
                }
                std::ranges::copy(it->second, words.begin());
            };

            const SphereTerrainFrame frame = BuildSphereTerrainFrame(input);
            memory.PrevCenter = terrain.Center;
            memory.PrevOrientation = terrain.Orientation;
            memory.PrevOrigin = origin;
            memory.HasPrevious = true;

            const u32 base = static_cast<u32>(records.size());
            records.insert(records.end(), frame.Records.begin(), frame.Records.end());
            terrain.Lists.resize(frame.Lists.size());
            SphereTerrainSelectionStats& stats = selection->second.second;
            for (usize view = 0; view < frame.Lists.size(); view++)
            {
                for (u32 surface = 0; surface < SphereTerrainSurfaceCount; surface++)
                {
                    const vector<u32>& list = frame.Lists[view][surface];
                    const bool drawnSurface = terrain.Surfaces[surface].Material != nullptr;
                    terrain.Lists[view][surface] = {static_cast<u32>(lists.size()),
                                                    drawnSurface ? static_cast<u32>(list.size())
                                                                 : 0u};
                    if (!drawnSurface)
                    {
                        continue;
                    }
                    for (const u32 index : list)
                    {
                        lists.push_back(base + index);
                    }
                    const u32 count = static_cast<u32>(list.size());
                    (view == 0 ? stats.MainViewTiles : stats.ShadowViewTiles) += count;
                    stats.Triangles += static_cast<u64>(count) * SphereTerrainTileTriangles;
                }
            }
            stats.CulledByFrustum = frame.CulledByFrustum;
            stats.CulledByHorizon = frame.CulledByHorizon;
            m_Totals.MainViewTiles += stats.MainViewTiles;
            m_Totals.ShadowViewTiles += stats.ShadowViewTiles;
            m_Totals.Triangles += stats.Triangles;
            m_Totals.CulledByFrustum += stats.CulledByFrustum;
            m_Totals.CulledByHorizon += stats.CulledByHorizon;
        }

        VE_PROFILE_COUNTER("Render/SphereTerrainMainViewTiles",
                           static_cast<f64>(m_Totals.MainViewTiles));
        VE_PROFILE_COUNTER("Render/SphereTerrainTriangles", static_cast<f64>(m_Totals.Triangles));
        if (records.empty())
        {
            return;
        }
        const u64 recordBytes = records.size() * sizeof(SphereTerrainDrawRecord);
        const u64 listBytes = lists.size() * sizeof(u32);
        if (slot.Records == nullptr || slot.Records->GetSize() < recordBytes)
        {
            slot.Records = Buffer::Create(m_Context, {
                                                         .Name = "Sphere Terrain Records",
                                                         .Size = GrownSize(recordBytes, 256 * 1024),
                                                         .Usage = BufferUsage::Storage,
                                                         .HostMapped = true,
                                                     });
        }
        if (slot.Lists == nullptr || slot.Lists->GetSize() < listBytes)
        {
            slot.Lists = Buffer::Create(m_Context, {
                                                       .Name = "Sphere Terrain Lists",
                                                       .Size = GrownSize(listBytes, 16 * 1024),
                                                       .Usage = BufferUsage::Storage,
                                                       .HostMapped = true,
                                                   });
        }
        std::memcpy(slot.Records->GetMappedData(), records.data(), recordBytes);
        std::memcpy(slot.Lists->GetMappedData(), lists.data(), listBytes);

        // One set per source per frame slot, rewritten each time its slot comes round: the slot's
        // last frame has retired by then, and the pool's views move when its capacity does.
        std::unordered_map<u64, Ref<DescriptorSet>> sets;
        for (Terrain& terrain : m_Terrains)
        {
            if (terrain.Lists.empty())
            {
                continue;
            }
            const SphereTerrainSource& source = *terrain.Source;
            const u64 serial = source.GetSerial();
            Ref<DescriptorSet>& set = sets[serial];
            if (set == nullptr)
            {
                const auto kept = slot.Sets.find(serial);
                set = kept != slot.Sets.end()
                          ? kept->second
                          : DescriptorSet::Create(
                                m_Context, {.Name = "Sphere Terrain Set", .Layout = m_SetLayout});
                for (u32 channel = 0; channel < MaxSphereTerrainChannels; channel++)
                {
                    // An unused binding names the height channel, which every source carries.
                    set->Write(channel, source.GetChannelArrayView(
                                            channel < source.GetChannelCount() ? channel : 0));
                }
                set->Write(SphereTerrainRecordsBinding, slot.Records);
                set->Write(SphereTerrainListBinding, slot.Lists);
            }
            terrain.Set = set.get();
        }
        slot.Sets = std::move(sets);
    }

    void SphereTerrainSystem::RecordGBuffer(CommandBuffer& cmd, const SurfacePush& push,
                                            const Ref<DescriptorSet>& drawDataSet,
                                            SurfaceShadowPipelines& shadowReaders) const
    {
        if (!m_Prepared)
        {
            return;
        }
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        for (const Terrain& terrain : m_Terrains)
        {
            if (terrain.Set == nullptr)
            {
                continue;
            }
            for (u32 surface = 0; surface < SphereTerrainSurfaceCount; surface++)
            {
                const Surface& drawn = terrain.Surfaces[surface];
                const uvec2 list = terrain.Lists[0][surface];
                if (!drawn.Seated || list.y == 0)
                {
                    continue;
                }
                const bool readsShadowMaps =
                    shadowReaders.BindPipeline(cmd, *drawn.Material, SurfacePipelineKind::Static);
                registry.Bind(cmd);
                cmd.BindDescriptorSets({drawDataSet.get(), terrain.Set},
                                       BindlessRegistry::FirstUserSet);
                if (readsShadowMaps)
                {
                    shadowReaders.BindShadowSet(cmd);
                }
                SurfacePush terrainPush = push;
                terrainPush.InstanceBuffer = surface;
                terrainPush.InstanceDraw = drawn.DrawSlot;
                terrainPush.InstanceBase = 0;
                cmd.PushConstants(terrainPush);
                cmd.BindIndexBuffer(m_GridIndices, IndexType::U16);
                cmd.DrawIndexed(SphereTerrainGridIndexCount, list.y, 0, 0, list.x);
            }
        }
    }

    bool SphereTerrainSystem::HasShadowDraws(const u32 shadowView) const
    {
        if (!m_Prepared || shadowView >= m_ShadowViewCount)
        {
            return false;
        }
        return std::ranges::any_of(
            m_Terrains,
            [&](const Terrain& terrain)
            {
                if (terrain.Set == nullptr || shadowView + 1 >= terrain.Lists.size())
                {
                    return false;
                }
                return std::ranges::any_of(terrain.Lists[shadowView + 1],
                                           [](const uvec2 list) { return list.y > 0; });
            });
    }

    void SphereTerrainSystem::RecordShadowView(CommandBuffer& cmd, const u32 shadowView,
                                               const mat4& viewProj, const bool depthClamp) const
    {
        if (!HasShadowDraws(shadowView))
        {
            return;
        }
        cmd.BindPipeline(depthClamp ? m_ShadowClampPipeline : m_ShadowPipeline);
        for (const Terrain& terrain : m_Terrains)
        {
            if (terrain.Set == nullptr || shadowView + 1 >= terrain.Lists.size())
            {
                continue;
            }
            for (u32 surface = 0; surface < SphereTerrainSurfaceCount; surface++)
            {
                const uvec2 list = terrain.Lists[shadowView + 1][surface];
                if (list.y == 0)
                {
                    continue;
                }
                cmd.BindDescriptorSets({terrain.Set}, SphereTerrainSet);
                cmd.PushConstants(ShadowTerrainPush{
                    .ViewProj = viewProj,
                    .Camera = vec4(m_CameraPosition, 0.0f),
                    .Surface = surface,
                });
                cmd.BindIndexBuffer(m_GridIndices, IndexType::U16);
                cmd.DrawIndexed(SphereTerrainGridIndexCount, list.y, 0, 0, list.x);
            }
        }
    }

    const vector<SphereTerrainDrawnTile>*
    SphereTerrainSystem::FindSelection(const Entity entity) const
    {
        const auto it = m_Selections.find(EntityValue(entity));
        return it != m_Selections.end() ? &it->second.first : nullptr;
    }

    const SphereTerrainSelectionStats* SphereTerrainSystem::FindStats(const Entity entity) const
    {
        const auto it = m_Selections.find(EntityValue(entity));
        return it != m_Selections.end() ? &it->second.second : nullptr;
    }
}
