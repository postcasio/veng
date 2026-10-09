#include <Veng/Renderer/ModelPortrait.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/ResidencyBatch.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/SceneView.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Visibility.h>

#include "CaptureDrive.h"
#include "PortraitRenderer.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace Veng::Renderer
{
    /// @brief The private scene, the renderer and the render state a ModelPortrait materializes.
    struct ModelPortraitRuntime
    {
        /// @brief Detaches the renderer from the private scene and hands it back to its pool.
        ~ModelPortraitRuntime();

        /// @brief The populate callback; survives a release (see ModelPortrait::Release).
        function<void(Scene&, Entity)> OnPopulate;
        /// @brief Instantiations of Model over the runtime's life.
        u32 InstantiationCount = 0;

        /// @brief The private scene; created with the first instantiation.
        Unique<Scene> Stage;
        /// @brief The model root: ModelPose's entity, every spawned root of Model under it.
        Entity Root = Entity::Null;
        /// @brief The key light entity.
        Entity KeyLight = Entity::Null;
        /// @brief The prefab instantiated under Root, held so a re-pointed Model is noticed.
        AssetHandle<Prefab> Instance;
        /// @brief What the spawn left streaming in; the portrait renders once it is resident.
        ResidencyBatch Pending;
        /// @brief Root and every entity the spawn put under it, sorted by EntityKey.
        vector<u64> ModelEntities;
        /// @brief Every entity a populate created, in creation order.
        vector<Entity> Populated;

        /// @brief The renderer; destroyed (or pooled) before Stage, which it may still reference.
        Unique<PortraitRenderer> Renderer;
        /// @brief Where Renderer goes on release; expired or empty drops it.
        std::weak_ptr<ModelPortraitPool> Pool;

        /// @brief Whether an OnDemand portrait owes a render.
        bool Owed = true;
        /// @brief Context frame serial of the last drive; 0 before the first.
        u64 LastDrivenSerial = 0;
        /// @brief The camera this frame renders through.
        CameraView Camera;
        /// @brief The model root's world matrix this frame.
        mat4 ModelTransform{1.0f};
        /// @brief World bounds of everything in the private scene this frame.
        AABB Bounds = AABB::Empty();
        /// @brief World bounds of the instantiated model alone this frame.
        AABB ModelBounds = AABB::Empty();
    };

    namespace
    {
        /// @brief A total-order key for an entity handle, for the sorted membership sets.
        u64 EntityKey(const Entity entity)
        {
            return (static_cast<u64>(entity.Index) << 32) | entity.Generation;
        }

        /// @brief Every live entity of @p scene, as sorted keys.
        vector<u64> LiveEntities(const Scene& scene)
        {
            vector<u64> keys;
            scene.ForEachEntity([&keys](const Entity entity)
                                { keys.push_back(EntityKey(entity)); });
            std::ranges::sort(keys);
            return keys;
        }

        /// @brief @p root and every entity below it, as sorted keys.
        vector<u64> Subtree(const Scene& scene, const Entity root)
        {
            vector<Entity> pending{root};
            vector<u64> keys;
            while (!pending.empty())
            {
                const Entity entity = pending.back();
                pending.pop_back();
                keys.push_back(EntityKey(entity));
                if (const auto* link = scene.TryGet<Hierarchy>(entity); link != nullptr)
                {
                    for (Entity child = link->FirstChild; !child.IsNull();)
                    {
                        pending.push_back(child);
                        const auto* childLink = scene.TryGet<Hierarchy>(child);
                        child = childLink != nullptr ? childLink->NextSibling : Entity::Null;
                    }
                }
            }
            std::ranges::sort(keys);
            return keys;
        }

        /// @brief Destroys every entity the last populate created that still stands.
        void ClearPopulated(ModelPortraitRuntime& runtime)
        {
            for (const Entity entity : runtime.Populated)
            {
                if (runtime.Stage->IsAlive(entity))
                {
                    runtime.Stage->DestroyEntity(entity);
                }
            }
            runtime.Populated.clear();
        }

        /// @brief Runs the populate callback, recording every entity it creates.
        void Populate(ModelPortraitRuntime& runtime)
        {
            if (!runtime.OnPopulate || runtime.Stage == nullptr || runtime.Root.IsNull())
            {
                return;
            }
            const vector<u64> before = LiveEntities(*runtime.Stage);
            runtime.OnPopulate(*runtime.Stage, runtime.Root);
            runtime.Stage->ForEachEntity(
                [&](const Entity entity)
                {
                    if (!std::ranges::binary_search(before, EntityKey(entity)))
                    {
                        runtime.Populated.push_back(entity);
                    }
                });
            runtime.Owed = true;
        }

        /// @brief Replaces the instance under Root with a fresh spawn of @p model.
        void Instantiate(ModelPortraitRuntime& runtime, const AssetHandle<Prefab>& model,
                         const Transform& pose, AssetManager& assets)
        {
            Scene& stage = *runtime.Stage;
            ClearPopulated(runtime);
            if (!runtime.Root.IsNull() && stage.IsAlive(runtime.Root))
            {
                stage.DestroyEntity(runtime.Root);
            }

            runtime.Root = stage.CreateEntity();
            stage.Add<Transform>(runtime.Root, pose);
            Prefab::SpawnResult spawned = model->SpawnInto(stage, assets);
            for (const Entity root : spawned.Roots)
            {
                stage.SetParent(root, runtime.Root);
            }
            runtime.Pending = std::move(spawned.Pending);
            runtime.Instance = model;
            runtime.ModelEntities = Subtree(stage, runtime.Root);
            ++runtime.InstantiationCount;
            runtime.Owed = true;
            Populate(runtime);
        }

        /// @brief Frames the camera from @p framing over this frame's bounds.
        void FrameCamera(ModelPortraitRuntime& runtime, const PortraitFraming& framing,
                         const uvec2 extent)
        {
            const f32 aspect = static_cast<f32>(extent.x) / static_cast<f32>(extent.y);
            const f32 fovY = glm::radians(std::clamp(framing.FieldOfView, 1.0f, 170.0f));

            CameraView& camera = runtime.Camera;
            if (framing.Mode == PortraitFramingMode::Explicit)
            {
                camera.SetViewFromWorld(glm::translate(mat4(1.0f), framing.CameraPosition) *
                                        glm::mat4_cast(glm::normalize(framing.CameraRotation)));
            }
            else
            {
                const AABB& fit =
                    runtime.ModelBounds.IsEmpty() ? runtime.Bounds : runtime.ModelBounds;
                const vec3 center = fit.IsEmpty() ? vec3(0.0f) : fit.Center();
                const f32 radius =
                    (fit.IsEmpty() ? 0.5f : std::max(glm::length(fit.Extents()), 1e-3f)) *
                    std::max(framing.Padding, 1e-3f);
                // The sphere fits the narrower of the two fields of view.
                const f32 fovX = 2.0f * std::atan(std::tan(fovY * 0.5f) * aspect);
                const f32 half = 0.5f * std::min(fovY, fovX);
                const f32 distance = radius / std::sin(half);
                const f32 yaw = glm::radians(framing.Yaw);
                const f32 pitch = glm::radians(std::clamp(framing.Pitch, -89.0f, 89.0f));
                const vec3 bearing{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                   std::cos(yaw) * std::cos(pitch)};
                camera.SetView(center + bearing * distance, center, vec3(0.0f, 1.0f, 0.0f));
            }

            // The clip planes enclose everything rendered, a populate's parts included.
            f32 nearPlane = 0.05f;
            f32 farPlane = 100.0f;
            if (!runtime.Bounds.IsEmpty())
            {
                const f32 reach = glm::length(runtime.Bounds.Extents());
                const f32 distance = glm::length(runtime.Bounds.Center() - camera.GetPosition());
                nearPlane = std::max(distance - reach, std::max(1e-3f, distance * 1e-3f));
                farPlane = std::max(distance + reach, nearPlane * 2.0f);
            }
            camera.SetPerspective(fovY, aspect, nearPlane, farPlane);
        }

        /// @brief Gathers this frame's bounds: everything, and the instantiated model alone.
        void GatherBounds(ModelPortraitRuntime& runtime)
        {
            vector<VisibleMesh> meshes;
            GatherMeshes(*runtime.Stage, meshes, runtime.Bounds);
            runtime.ModelBounds = AABB::Empty();
            for (const VisibleMesh& mesh : meshes)
            {
                if (std::ranges::binary_search(runtime.ModelEntities, EntityKey(mesh.Owner)))
                {
                    runtime.ModelBounds = Union(runtime.ModelBounds, mesh.WorldBounds);
                }
            }
        }
    }

    ModelPortraitRuntime::~ModelPortraitRuntime()
    {
        if (Renderer == nullptr)
        {
            return;
        }
        // The renderer retains the scene it last gathered, which dies with this runtime.
        Renderer->ResetForReuse();
        if (const Ref<ModelPortraitPool> pool = Pool.lock(); pool != nullptr)
        {
            pool->Return(std::move(Renderer));
        }
    }

    ModelPortrait::ModelPortrait() = default;
    ModelPortrait::~ModelPortrait() = default;
    ModelPortrait::ModelPortrait(ModelPortrait&&) noexcept = default;
    ModelPortrait& ModelPortrait::operator=(ModelPortrait&&) noexcept = default;

    void ModelPortrait::SetOnPopulate(function<void(Scene&, Entity)> callback)
    {
        if (!Runtime)
        {
            Runtime = CreateUnique<ModelPortraitRuntime>();
        }
        Runtime->OnPopulate = std::move(callback);
        Repopulate();
    }

    void ModelPortrait::Repopulate() const
    {
        if (!Runtime || Runtime->Stage == nullptr || Runtime->Root.IsNull())
        {
            return;
        }
        ClearPopulated(*Runtime);
        Populate(*Runtime);
    }

    void ModelPortrait::MarkDirty() const
    {
        if (!Runtime)
        {
            Runtime = CreateUnique<ModelPortraitRuntime>();
        }
        Runtime->Owed = true;
    }

    void ModelPortrait::Release() const
    {
        if (!Runtime || (Runtime->Renderer == nullptr && Runtime->Stage == nullptr))
        {
            return;
        }
        function<void(Scene&, Entity)> callback = std::move(Runtime->OnPopulate);
        const u32 instantiations = Runtime->InstantiationCount;
        Runtime = CreateUnique<ModelPortraitRuntime>();
        Runtime->OnPopulate = std::move(callback);
        Runtime->InstantiationCount = instantiations;
    }

    ModelPortraitOutput ModelPortrait::GetOutput() const
    {
        if (!Runtime || Runtime->Renderer == nullptr || !Runtime->Renderer->HasRendered())
        {
            return {};
        }
        const ModelPortraitRuntime& runtime = *Runtime;
        const PortraitRenderer& renderer = *runtime.Renderer;
        return ModelPortraitOutput{
            .Ready = true,
            .Color = renderer.GetColorHandle(),
            .Depth = renderer.GetDepthHandle(),
            .Normal = renderer.GetNormalHandle(),
            .Sampler = renderer.GetSampler(),
            .Extent = renderer.GetConfig().Extent,
            .Camera = runtime.Camera,
            .ModelTransform = runtime.ModelTransform,
            .Bounds = runtime.Bounds,
            .ModelBounds = runtime.ModelBounds,
        };
    }

    Scene* ModelPortrait::GetScene() const
    {
        return Runtime ? Runtime->Stage.get() : nullptr;
    }

    Entity ModelPortrait::GetModelRoot() const
    {
        return Runtime ? Runtime->Root : Entity::Null;
    }

    u32 ModelPortrait::GetInstantiationCount() const
    {
        return Runtime ? Runtime->InstantiationCount : 0;
    }

    bool ModelPortrait::HasRenderer() const
    {
        return Runtime && Runtime->Renderer != nullptr;
    }

    const PortraitRenderer* ModelPortrait::GetRenderer() const
    {
        return Runtime ? Runtime->Renderer.get() : nullptr;
    }

    void ModelPortrait::Materialize(Unique<PortraitRenderer> renderer,
                                    std::weak_ptr<ModelPortraitPool> pool) const
    {
        VE_ASSERT(renderer != nullptr, "ModelPortrait::Materialize: no renderer to install");
        if (!Runtime)
        {
            Runtime = CreateUnique<ModelPortraitRuntime>();
        }
        VE_ASSERT(Runtime->Renderer == nullptr,
                  "ModelPortrait::Materialize: the portrait already holds a renderer");
        Runtime->Renderer = std::move(renderer);
        Runtime->Pool = std::move(pool);
        Runtime->Owed = true;
    }

    bool ModelPortrait::Prepare(Context& context, AssetManager& assets) const
    {
        VE_ASSERT(HasRenderer(), "ModelPortrait::Prepare: no renderer is installed");
        ModelPortraitRuntime& runtime = *Runtime;

        // A portrait its driver skipped for a frame shows what it last saw, so it renders again.
        const u64 serial = context.GetFrameSerial();
        if (CaptureDriveSkippedFrame(runtime.LastDrivenSerial, serial))
        {
            runtime.Owed = true;
        }
        runtime.LastDrivenSerial = serial;

        if (!Model.IsLoaded())
        {
            // An emptied (or re-pointed, still loading) Model shows nothing rather than the old one.
            if (runtime.Instance.Get() != nullptr)
            {
                ClearPopulated(runtime);
                if (runtime.Stage->IsAlive(runtime.Root))
                {
                    runtime.Stage->DestroyEntity(runtime.Root);
                }
                runtime.Root = Entity::Null;
                runtime.Instance = {};
                runtime.ModelEntities.clear();
                runtime.Renderer->ResetForReuse();
            }
            return false;
        }
        if (runtime.Stage == nullptr)
        {
            runtime.Stage = Scene::Create(assets.GetTypeRegistry());
            runtime.KeyLight = runtime.Stage->CreateEntity();
            runtime.Stage->Add<Light>(runtime.KeyLight);
        }
        if (runtime.Instance.Get() != Model.Get())
        {
            Instantiate(runtime, Model, ModelPose, assets);
        }
        if (!runtime.Pending.IsResident())
        {
            return false;
        }

        Scene& stage = *runtime.Stage;
        // Written only on a change: a write moves the scene's spatial version and refits its tree.
        if (const Transform& pose = *std::as_const(stage).TryGet<Transform>(runtime.Root);
            pose.Position != ModelPose.Position || pose.Rotation != ModelPose.Rotation ||
            pose.Scale != ModelPose.Scale)
        {
            stage.Get<Transform>(runtime.Root) = ModelPose;
            runtime.Owed = true;
        }
        auto& key = stage.Get<Light>(runtime.KeyLight);
        key.Type = LightType::Directional;
        key.Direction = glm::length(Lighting.KeyDirection) > 0.0f
                            ? glm::normalize(Lighting.KeyDirection)
                            : vec3(0.0f, -1.0f, 0.0f);
        key.Color = Lighting.KeyColor;
        key.Intensity = Lighting.KeyIntensity;
        key.CastsShadows = false;

        GatherBounds(runtime);
        runtime.ModelTransform = WorldMatrix(stage, runtime.Root);
        FrameCamera(runtime, Framing, runtime.Renderer->GetConfig().Extent);

        return Refresh == CaptureRefresh::EveryFrame || runtime.Owed ||
               !runtime.Renderer->HasRendered();
    }

    void ModelPortrait::Render(CommandBuffer& cmd) const
    {
        ModelPortraitRuntime& runtime = *Runtime;
        const SceneView view{
            .World = *runtime.Stage,
            .Camera = runtime.Camera,
            .AmbientFloor = Lighting.Ambient,
        };
        runtime.Renderer->Render(cmd, view);
        runtime.Owed = false;
    }

    void ReleaseMismatchedRenderer(const ModelPortrait& portrait)
    {
        ModelPortraitRuntime* const runtime = portrait.Runtime.get();
        if (runtime == nullptr || runtime->Renderer == nullptr ||
            runtime->Renderer->GetConfig() ==
                PortraitRendererConfig{.Extent = portrait.Extent, .Output = portrait.Output})
        {
            return;
        }
        runtime->Renderer->ResetForReuse();
        if (const Ref<ModelPortraitPool> pool = runtime->Pool.lock(); pool != nullptr)
        {
            pool->Return(std::move(runtime->Renderer));
        }
        runtime->Renderer.reset();
    }

    ModelPortraitPool::ModelPortraitPool(const usize capacity) : m_Capacity(capacity) {}

    ModelPortraitPool::~ModelPortraitPool() = default;

    Unique<PortraitRenderer> ModelPortraitPool::Take(const PortraitRendererConfig& config)
    {
        // The most recently returned first: it has sat idle the shortest time.
        for (usize i = m_Held.size(); i-- > 0;)
        {
            if (m_Held[i]->GetConfig() == config)
            {
                Unique<PortraitRenderer> renderer = std::move(m_Held[i]);
                m_Held.erase(m_Held.begin() + static_cast<std::ptrdiff_t>(i));
                return renderer;
            }
        }
        return nullptr;
    }

    void ModelPortraitPool::Return(Unique<PortraitRenderer> renderer)
    {
        if (renderer == nullptr || m_Capacity == 0)
        {
            return;
        }
        if (m_Held.size() >= m_Capacity)
        {
            m_Held.erase(m_Held.begin());
        }
        renderer->ResetForReuse();
        m_Held.push_back(std::move(renderer));
    }

    void ModelPortraitPool::Clear()
    {
        m_Held.clear();
    }
}
