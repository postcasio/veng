#pragma once

#include <Veng/Log.h>
#include <Veng/Persistence/Store.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Veng.h>

#include <algorithm>
#include <memory>
#include <span>
#include <utility>

namespace Veng
{
    /// @brief The key a singleton family's one record is stored under.
    inline constexpr StoreKey SingletonRecordKey{.Lo = 0, .Hi = 0};

    namespace Detail
    {
        /// @brief Appends T's reflected blob to a record when the entity carries T.
        /// @param scene   The scene holding the entity.
        /// @param entity  The entity to read T off.
        /// @param types   The registry T is reflected in.
        /// @param record  The record the blob is appended to.
        /// @return True when the entity carried T and a blob was appended.
        template <typename T>
        bool CaptureComponentBlob(Scene& scene, const Entity entity, const TypeRegistry& types,
                                  StoreRecord& record)
        {
            const T* const component = scene.TryGet<T>(entity);
            if (component == nullptr)
            {
                return false;
            }
            ComponentBlob blob{.Type = TypeIdOf<T>()};
            WriteFields(blob.Bytes, component, types.Info(TypeIdOf<T>()), types);
            record.Components.push_back(std::move(blob));
            return true;
        }

        /// @brief Decodes a blob onto the entity when the blob encodes T, adding T when absent.
        /// @param scene   The scene holding the entity.
        /// @param entity  The entity the component is applied to.
        /// @param types   The registry T is reflected in.
        /// @param blob    The stored blob.
        /// @return True when the blob encoded T and was claimed by this type, decoded or not.
        template <typename T>
        bool ApplyComponentBlob(Scene& scene, const Entity entity, const TypeRegistry& types,
                                const ComponentBlob& blob)
        {
            if (blob.Type != TypeIdOf<T>())
            {
                return false;
            }
            T* component = scene.TryGet<T>(entity);
            if (component == nullptr)
            {
                component = &scene.Add<T>(entity);
            }
            if (const VoidResult read =
                    ReadFields(std::span(blob.Bytes), component, types.Info(blob.Type), types);
                !read)
            {
                Log::Warn("store family '{}': a stored component did not decode: {}",
                          types.Info(blob.Type).Name, read.error());
            }
            return true;
        }

        /// @brief Warns about a stored blob no component type of a family claims, once per family.
        ///
        /// The latch is the family's own, so a record full of unmatched blobs — or a capture cycle
        /// repeating the same drift every rehydrate — costs one line, not a flood.
        /// @param reported  The family's one-shot latch; set by the first report.
        /// @param fileStem  The family's file stem, naming the family in the message.
        /// @param type      The unmatched blob's type id.
        VE_API void ReportUnmatchedBlob(bool& reported, string_view fileStem, TypeId type);
    }

    /// @brief Builds a family that persists a fixed component set off every marker-carrying entity.
    ///
    /// Capture walks every entity carrying MarkerT, derives the entity's record key from the marker
    /// through keyOf, and writes each ComponentTs the entity carries as a reflected blob.
    /// Rehydrate walks the same marker set and applies a stored record's blobs onto the entity whose
    /// key matches. Version and an optional Migrate are the caller's to set on the returned family
    /// before registering it.
    ///
    /// The edge behavior is the helper's contract, since it is where two hand-written spellings
    /// would otherwise diverge:
    /// - An entity whose key resolves to nullopt is skipped, never written under a sentinel key.
    /// - An entity from which zero components were captured contributes no record. A marker
    ///   carried alone would otherwise write an empty record on every capture, growing the family
    ///   forever.
    /// - Rehydrate adds a stored component the claimant does not already carry, rather than
    ///   only updating what is present: restoring onto a freshly built entity is the common case.
    /// - A component type absent from a stored record leaves the live component untouched.
    /// - A stored blob matching none of ComponentTs is skipped and logged, naming the type id,
    ///   once per family — an unrecognized blob is a diagnostic, never silently swallowed.
    /// - Where several entities claim one key, the first wins; the rest are left untouched.
    /// - Rehydrate is an identity restore: the elapsed wall seconds are forwarded to nothing. A
    ///   consumer needing catch-up math writes its own Rehydrate, which stays fully supported —
    ///   this is a convenience constructor for StoreFamily, not a new mechanism.
    ///
    /// @tparam MarkerT      The component identifying an entity this family persists.
    /// @tparam ComponentTs  The reflected component types captured off a marked entity.
    /// @param id        The family's minted id.
    /// @param fileStem  The family file's name stem within the slot directory.
    /// @param keyOf     Derives a marked entity's record key; nullopt skips the entity.
    /// @param types     The registry MarkerT's components are reflected in; must outlive the store.
    /// @return The family, ready to register.
    template <typename MarkerT, typename... ComponentTs>
    [[nodiscard]] StoreFamily ComponentSetFamily(const StoreFamilyId id, string fileStem,
                                                 function<optional<StoreKey>(const MarkerT&)> keyOf,
                                                 const TypeRegistry& types)
    {
        const string stem = fileStem;
        const Ref<bool> reported = std::make_shared<bool>(false);

        StoreFamily family{.Id = id, .FileStem = std::move(fileStem), .Version = 1};
        family.Capture =
            [keyOf, &types](Scene& scene, vector<std::pair<StoreKey, StoreRecord>>& out)
        {
            for (auto [entity, marker] : scene.View<MarkerT>())
            {
                const optional<StoreKey> key = keyOf(marker);
                if (!key.has_value())
                {
                    continue;
                }
                StoreRecord record;
                // A comma fold, not a disjunction: every component is offered the entity, so a
                // short-circuit cannot drop one behind an absent predecessor.
                (Detail::CaptureComponentBlob<ComponentTs>(scene, entity, types, record), ...);
                if (record.Components.empty())
                {
                    continue;
                }
                out.emplace_back(*key, std::move(record));
            }
        };
        family.RehydrateKeys = [keyOf](Scene& scene)
        {
            vector<StoreKey> keys;
            for (auto [entity, marker] : scene.View<MarkerT>())
            {
                if (const optional<StoreKey> key = keyOf(marker); key.has_value())
                {
                    keys.push_back(*key);
                }
            }
            return keys;
        };
        family.Rehydrate = [keyOf, &types, stem, reported](Scene& scene, const StoreKey key,
                                                           const StoreRecord& record, f64)
        {
            // The claimant is resolved before anything is applied: Rehydrate adds absent
            // components, and a scene mutated mid-view would iterate over a moving pool.
            Entity claimant = Entity::Null;
            for (auto [entity, marker] : scene.View<MarkerT>())
            {
                if (const optional<StoreKey> candidate = keyOf(marker);
                    candidate.has_value() && *candidate == key)
                {
                    claimant = entity;
                    break;
                }
            }
            if (claimant.IsNull())
            {
                return;
            }
            for (const ComponentBlob& blob : record.Components)
            {
                const bool applied =
                    (Detail::ApplyComponentBlob<ComponentTs>(scene, claimant, types, blob) || ...);
                if (!applied)
                {
                    Detail::ReportUnmatchedBlob(*reported, stem, blob.Type);
                }
            }
        };
        return family;
    }

    /// @brief Builds a family holding exactly one record, at SingletonRecordKey.
    ///
    /// The whole-slot settings shape: no scene hooks, since the record is written directly through
    /// WriteSingleton rather than captured off entities. Version and an optional Migrate are the
    /// caller's to set before registering it.
    /// @param id        The family's minted id.
    /// @param fileStem  The family file's name stem within the slot directory.
    /// @return The family, ready to register.
    [[nodiscard]] VE_API StoreFamily SingletonFamily(StoreFamilyId id, string fileStem);

    /// @brief Reads T out of a singleton family's record.
    /// @tparam T  The reflected type to read.
    /// @param store   The store holding the family.
    /// @param family  The singleton family's id.
    /// @param types   The registry T is reflected in.
    /// @return The decoded value, or nullopt when the record or T's blob is absent, or the blob
    /// failed to decode (which is logged).
    template <typename T>
    [[nodiscard]] optional<T> ReadSingleton(Store& store, const StoreFamilyId family,
                                            const TypeRegistry& types)
    {
        const optional<StoreRecord> record = store.Read(family, SingletonRecordKey);
        if (!record.has_value())
        {
            return std::nullopt;
        }
        for (const ComponentBlob& blob : record->Components)
        {
            if (blob.Type != TypeIdOf<T>())
            {
                continue;
            }
            T value;
            if (const VoidResult read =
                    ReadFields(std::span(blob.Bytes), &value, types.Info(blob.Type), types);
                !read)
            {
                Log::Warn("singleton record: a stored '{}' did not decode: {}",
                          types.Info(blob.Type).Name, read.error());
                return std::nullopt;
            }
            return value;
        }
        return std::nullopt;
    }

    /// @brief Writes T into a singleton family's record, preserving the record's other blobs.
    ///
    /// Read-modify-write at the blob level: the stored record is read back, T's blob replaced
    /// or inserted, and every other component blob kept, so independent types sharing the record
    /// never clobber each other. It is not field-level — writing a T replaces the whole T — so
    /// a consumer holding several independently-updated fields inside one reflected type still
    /// reads that type back and modifies it before calling.
    /// @tparam T  The reflected type to write.
    /// @param store   The store holding the family.
    /// @param family  The singleton family's id.
    /// @param value   The value to write.
    /// @param types   The registry T is reflected in.
    template <typename T>
    void WriteSingleton(Store& store, const StoreFamilyId family, const T& value,
                        const TypeRegistry& types)
    {
        StoreRecord record = store.Read(family, SingletonRecordKey).value_or(StoreRecord{});
        record.CapturedAtWall = Store::WallClockSeconds();

        ComponentBlob blob{.Type = TypeIdOf<T>()};
        WriteFields(blob.Bytes, &value, types.Info(TypeIdOf<T>()), types);

        const auto existing =
            std::ranges::find_if(record.Components, [](const ComponentBlob& component)
                                 { return component.Type == TypeIdOf<T>(); });
        if (existing != record.Components.end())
        {
            *existing = std::move(blob);
        }
        else
        {
            record.Components.push_back(std::move(blob));
        }
        store.Write(family, SingletonRecordKey, std::move(record));
    }
}
