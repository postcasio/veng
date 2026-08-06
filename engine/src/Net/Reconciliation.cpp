#include <Veng/Net/Reconciliation.h>

#include <Veng/Reflection/Serialize.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>
#include <new>

namespace Veng::Net
{
    namespace
    {
        // Aligned scratch storage for one type-erased component value, default-constructed through the
        // type's thunks — a value is decoded out of line so a malformed record never touches live
        // state (the Replication.cpp / PredictionHistory.cpp idiom).
        struct ScratchComponent
        {
            const TypeInfo& Info;
            void* Ptr;

            explicit ScratchComponent(const TypeInfo& info)
                : Info(info), Ptr(::operator new(info.Size, std::align_val_t{info.Align}))
            {
                Info.DefaultConstruct(Ptr);
            }

            ~ScratchComponent()
            {
                Info.Destruct(Ptr);
                ::operator delete(Ptr, std::align_val_t{Info.Align});
            }

            ScratchComponent(const ScratchComponent&) = delete;
            ScratchComponent& operator=(const ScratchComponent&) = delete;
        };

        bool BytesEqual(const void* a, const void* b, usize count)
        {
            return std::memcmp(a, b, count) == 0;
        }

        bool VectorMatch(const void* a, const void* b, usize floats, f32 epsilon)
        {
            const auto* pa = static_cast<const f32*>(a);
            const auto* pb = static_cast<const f32*>(b);
            for (usize i = 0; i < floats; ++i)
            {
                if (std::abs(pa[i] - pb[i]) > epsilon)
                {
                    return false;
                }
            }
            return true;
        }

        bool QuaternionMatch(const void* a, const void* b, f32 epsilon)
        {
            const auto& qa = *static_cast<const quat*>(a);
            const auto& qb = *static_cast<const quat*>(b);
            // The double-cover means q and -q are the same rotation, so compare on |dot|.
            return (1.0f - std::abs(glm::dot(qa, qb))) <= epsilon;
        }

        bool ValueMatch(const void* a, const void* b, const TypeInfo& info,
                        const TypeRegistry& registry, const ReconcileTolerances& tol);

        bool FieldMatch(const void* baseA, const void* baseB, const FieldDescriptor& field,
                        const TypeRegistry& registry, const ReconcileTolerances& tol)
        {
            const void* pa = static_cast<const u8*>(baseA) + field.Offset;
            const void* pb = static_cast<const u8*>(baseB) + field.Offset;
            switch (field.Class)
            {
            case FieldClass::Array:
            {
                const usize countA = field.ArraySize(pa);
                const usize countB = field.ArraySize(pb);
                if (countA != countB)
                {
                    return false;
                }
                const TypeInfo& element = registry.Info(field.ElementType);
                for (usize i = 0; i < countA; ++i)
                {
                    if (!ValueMatch(field.ArrayElementConst(pa, i), field.ArrayElementConst(pb, i),
                                    element, registry, tol))
                    {
                        return false;
                    }
                }
                return true;
            }
            // An asset handle serializes as its leading u64 id, an entity reference is a plain handle:
            // both are exact leaves whose field type may be synthetic/unregistered, so compare bytes
            // without a registry lookup.
            case FieldClass::AssetHandle:
                return BytesEqual(pa, pb, sizeof(u64));
            case FieldClass::Reference:
                return BytesEqual(pa, pb, sizeof(Entity));
            default:
                return ValueMatch(pa, pb, registry.Info(field.Type), registry, tol);
            }
        }

        bool ValueMatch(const void* a, const void* b, const TypeInfo& info,
                        const TypeRegistry& registry, const ReconcileTolerances& tol)
        {
            switch (info.Class)
            {
            case FieldClass::Vector:
                return VectorMatch(a, b, info.Size / sizeof(f32), tol.Position);
            case FieldClass::Quaternion:
                return QuaternionMatch(a, b, tol.Rotation);
            case FieldClass::String:
                return *static_cast<const string*>(a) == *static_cast<const string*>(b);
            case FieldClass::Struct:
                for (const FieldDescriptor& field : info.Fields)
                {
                    if (!FieldMatch(a, b, field, registry, tol))
                    {
                        return false;
                    }
                }
                return true;
            case FieldClass::Variant:
            {
                const TypeId activeA = info.VariantActiveType(a);
                const TypeId activeB = info.VariantActiveType(b);
                if (activeA != activeB)
                {
                    return false;
                }
                if (activeA == InvalidTypeId)
                {
                    return true;
                }
                return ValueMatch(info.VariantActivePtrConst(a), info.VariantActivePtrConst(b),
                                  registry.Info(activeA), registry, tol);
            }
            case FieldClass::AssetHandle:
                return BytesEqual(a, b, sizeof(u64));
            case FieldClass::Reference:
                return BytesEqual(a, b, sizeof(Entity));
            // Scalar / Enum / Matrix — discrete leaves that must match exactly.
            default:
                return BytesEqual(a, b, info.Size);
            }
        }

        // Restores the predicted set's live components to the authoritative snapshot records — the
        // "server is right" rewind, from the record rather than the recorded prediction.
        void ApplyAuthoritative(Scene& scene, std::span<const PredictedRecord> records,
                                const TypeRegistry& registry)
        {
            for (const PredictedRecord& record : records)
            {
                if (record.Entity.IsNull() || !scene.IsAlive(record.Entity))
                {
                    continue;
                }
                for (const PredictedRecord::Component& component : record.Components)
                {
                    if (!registry.IsRegistered(component.Type))
                    {
                        continue;
                    }
                    const TypeInfo& info = registry.Info(component.Type);
                    ScratchComponent scratch(info);
                    if (VoidResult read = ReadFields(component.Bytes, scratch.Ptr, info, registry);
                        !read)
                    {
                        continue;
                    }
                    void* dest = scene.TryGetComponent(record.Entity, component.Type);
                    if (dest == nullptr)
                    {
                        dest = scene.AddComponent(record.Entity, component.Type);
                    }
                    info.Destruct(dest);
                    info.MoveConstruct(dest, scratch.Ptr);
                }
            }
        }

        // True when the recorded prediction at C matches the authoritative records field-wise.
        bool PredictionMatches(const PredictionHistory& history,
                               std::span<const PredictedRecord> authoritative, u64 consumedTick,
                               const TypeRegistry& registry, const ReconcileTolerances& tol)
        {
            for (const PredictedRecord& record : authoritative)
            {
                for (const PredictedRecord::Component& component : record.Components)
                {
                    if (!registry.IsRegistered(component.Type))
                    {
                        continue;
                    }
                    const std::span<const u8> predicted =
                        history.Captured(consumedTick, record.Entity, component.Type);
                    if (predicted.empty())
                    {
                        // The server sent a component the prediction never captured at C — a mismatch.
                        return false;
                    }
                    const TypeInfo& info = registry.Info(component.Type);
                    ScratchComponent authoritativeValue(info);
                    ScratchComponent predictedValue(info);
                    if (VoidResult read =
                            ReadFields(component.Bytes, authoritativeValue.Ptr, info, registry);
                        !read)
                    {
                        continue; // undecodable authoritative record: leave the prediction be
                    }
                    if (VoidResult read = ReadFields(predicted, predictedValue.Ptr, info, registry);
                        !read)
                    {
                        return false;
                    }
                    if (!ValueMatch(authoritativeValue.Ptr, predictedValue.Ptr, info, registry,
                                    tol))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    bool ValuesMatch(const void* a, const void* b, const TypeInfo& info,
                     const TypeRegistry& registry, const ReconcileTolerances& tol)
    {
        return ValueMatch(a, b, info, registry, tol);
    }

    ReconcileResult Reconcile(Scene& scene, PredictionHistory& history,
                              std::span<const PredictedRecord> authoritative,
                              const u64 consumedTick, const ReplayTick& replay,
                              const ReconcileTolerances& tol)
    {
        ReconcileResult result;
        if (authoritative.empty() || consumedTick == 0)
        {
            // No predicted entity in this snapshot, or the server has confirmed no input yet
            // (client ticks start at 1) — nothing to confirm against.
            return result;
        }

        const TypeRegistry& registry = scene.GetTypeRegistry();
        result.Compared = true;

        // A confirmation for a tick older than the retained history — the prediction started after the
        // input the server is confirming (join/possession warm-up), or an extreme spike aged it out of
        // the ring. There is nothing to compare or roll back to: leave the prediction to stand and the
        // history to grow, so reconciliation resumes once the history covers the confirmed tick. Never
        // a crash; the ring's own capacity bounds the growth (planset-54's interpolation-only stance
        // until the window catches up).
        if (!history.Contains(consumedTick))
        {
            return result; // Compared, not corrected
        }

        if (PredictionMatches(history, authoritative, consumedTick, registry, tol))
        {
            // The prediction stands; drop the history the server has now confirmed.
            history.TrimThrough(consumedTick);
            return result;
        }

        result.Corrected = true;
        const bool canReplay = static_cast<bool>(replay);

        // Capture the pre-correction *visible* pose (sim Transform plus any decaying render offset)
        // of each tracked entity, so the correction can be hidden behind a fresh render offset.
        struct VisiblePose
        {
            Entity Entity;
            vec3 Position;
            quat Rotation;
        };
        vector<VisiblePose> before;
        const Scene& readScene = scene;
        for (const Entity entity : history.Tracked())
        {
            if (entity.IsNull() || !readScene.IsAlive(entity))
            {
                continue;
            }
            const auto* transform = readScene.TryGet<Transform>(entity);
            if (transform == nullptr)
            {
                continue;
            }
            vec3 position = transform->Position;
            quat rotation = transform->Rotation;
            if (const auto* error = readScene.TryGet<PredictionError>(entity))
            {
                position += error->Position;
                rotation = glm::normalize(error->Rotation * rotation);
            }
            before.push_back(
                VisiblePose{.Entity = entity, .Position = position, .Rotation = rotation});
        }

        if (canReplay)
        {
            // Rollback: snapshot the input tape C+1..now, restore to the authoritative record at C,
            // then replay the recorded inputs forward through the real Sim systems, re-recording.
            const std::span<const StoredInput> tape = history.InputsAfter(consumedTick);
            const vector<StoredInput> inputs(tape.begin(), tape.end());
            ApplyAuthoritative(scene, authoritative, registry);
            history.Clear();
            for (const StoredInput& input : inputs)
            {
                replay(scene, input.Tick, input.Input);
                history.Record(input.Tick, input.Input, scene);
                ++result.ReplayedTicks;
            }
        }
        else
        {
            // Rollback disabled (no replay driver): snap to the authoritative state, clear history,
            // and re-predict forward from live input — the planset-54 hard-snap fallback.
            ApplyAuthoritative(scene, authoritative, registry);
            history.Clear();
            result.Snapped = true;
        }

        // Error smoothing: hold each tracked entity's pre-correction visible pose as a decaying render
        // offset from its corrected pose, unless the residual is teleport-scale (then it snaps).
        for (const VisiblePose& pose : before)
        {
            if (!readScene.IsAlive(pose.Entity))
            {
                continue;
            }
            const auto* corrected = readScene.TryGet<Transform>(pose.Entity);
            if (corrected == nullptr)
            {
                continue;
            }
            const vec3 positionResidual = pose.Position - corrected->Position;
            const quat rotationResidual =
                glm::normalize(pose.Rotation * glm::inverse(corrected->Rotation));

            const bool snap = result.Snapped || glm::length(positionResidual) > tol.SnapDistance;
            if (snap)
            {
                if (scene.Has<PredictionError>(pose.Entity))
                {
                    (void)scene.Remove<PredictionError>(pose.Entity);
                }
                result.Snapped = true;
                continue;
            }

            PredictionError& error = scene.Has<PredictionError>(pose.Entity)
                                         ? scene.Get<PredictionError>(pose.Entity)
                                         : scene.Add<PredictionError>(pose.Entity);
            error.Position = positionResidual;
            error.Rotation = rotationResidual;
        }

        return result;
    }
}
