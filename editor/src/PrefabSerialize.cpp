#include "PrefabSerialize.h"

#include "JsonUtil.h"

#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>
#include <fstream>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        using json = nlohmann::json;

        // Builds the JsonFieldHooks for one entity's component write: a live Entity maps back
        // to its prefab-local index (the inverse of the loader's spawn-time remap). AssetId
        // validation is unset — the editor writes whatever handle the entity already carries.
        JsonFieldHooks MakeWriteHooks(const function<i64(u32 liveIndex)>& referenceIndex)
        {
            JsonFieldHooks hooks;
            hooks.WriteReference = [&referenceIndex](Entity entity) -> json
            {
                if (entity.IsNull())
                {
                    return nullptr;
                }
                const i64 index = referenceIndex(entity.Index);
                // The reference points outside the saved entity set; emit null rather than a
                // dangling index the cooker would reject as out of range.
                return index < 0 ? json(nullptr) : json(index);
            };
            return hooks;
        }

        // Patches the entity's `components` object: each live component the writer understands is
        // merge-written over its existing source object (preserving any key the shared walker does
        // not own), reusing the source key's exact spelling when it already named that component
        // so a "::Veng::Name"-spelled source stays byte-stable. Any source key that does not
        // resolve to a registered component type — comments-as-keys, future fields, hand-authored
        // extras — is preserved in place. A registered-type key the live entity no longer holds is
        // dropped (the component was removed). Components are emitted by ascending TypeId, so a
        // re-save produces a stable diff regardless of pool iteration order.
        json WriteComponents(const Scene& scene, Entity entity, const TypeRegistry& registry,
                             const json& sourceComponents, const JsonFieldHooks& hooks)
        {
            vector<std::pair<TypeId, const void*>> components;
            const_cast<Scene&>(scene).ForEachComponent(entity, [&](TypeId id, void* component)
                                                       { components.emplace_back(id, component); });

            std::ranges::sort(components,
                              [](const auto& a, const auto& b) { return a.first < b.first; });

            // Preserve every source key that does not name a registered component type: an unknown
            // key (a comment, a future field) is kept verbatim; a key naming a registered type is
            // dropped here and re-emitted below only when the live entity still holds it.
            json out = json::object();
            if (sourceComponents.is_object())
            {
                for (auto it = sourceComponents.begin(); it != sourceComponents.end(); ++it)
                {
                    const bool namesRegisteredType =
                        std::ranges::any_of(registry.All(), [&](const auto& pair)
                                            { return TypeNameMatches(pair.second, it.key()); });
                    if (!namesRegisteredType)
                    {
                        out[it.key()] = it.value();
                    }
                }
            }

            for (const auto& [id, component] : components)
            {
                const TypeInfo& info = registry.Info(id);

                // Reuse the source's spelling of this component's key when present (so a leading
                // "::" or any qualified form authored by hand survives), else the canonical name.
                string key = info.QualifiedName;
                json existing = json::object();
                if (sourceComponents.is_object())
                {
                    for (auto it = sourceComponents.begin(); it != sourceComponents.end(); ++it)
                    {
                        if (TypeNameMatches(info, it.key()))
                        {
                            key = it.key();
                            existing = it.value();
                            break;
                        }
                    }
                }
                if (!existing.is_object())
                {
                    existing = json::object();
                }

                JsonWriteFields(existing, component, info, registry, hooks);
                out[key] = std::move(existing);
            }
            return out;
        }

        // Collects the scene's entities in a stable hierarchy order: each root (no parent) followed
        // by its Hierarchy subtree depth-first in ForEachChild (sibling) order, so a save→cook→
        // spawn round-trip reproduces the authored hierarchy and sibling order exactly.
        void GatherHierarchyOrder(const Scene& scene, Entity entity, vector<Entity>& out)
        {
            out.push_back(entity);
            scene.ForEachChild(entity,
                               [&](Entity child) { GatherHierarchyOrder(scene, child, out); });
        }

        vector<Entity> HierarchyOrderedEntities(const Scene& scene)
        {
            vector<Entity> roots;
            scene.ForEachEntity(
                [&](Entity entity)
                {
                    if (scene.GetParent(entity).IsNull())
                    {
                        roots.push_back(entity);
                    }
                });

            vector<Entity> ordered;
            for (const Entity root : roots)
            {
                GatherHierarchyOrder(scene, root, ordered);
            }
            return ordered;
        }

        // Writes `doc` to `target` atomically: serialize to a temp sibling, then rename over the
        // target (atomic on the same filesystem), so a failed or interrupted write never truncates
        // the only copy of hand-authored prefab source.
        VoidResult WriteAtomic(const json& doc, const path& target)
        {
            const path temp = path{target}.concat(".tmp");
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    return std::unexpected(
                        fmt::format("failed to open temp file '{}'", temp.string()));
                }
                out << doc.dump(2) << '\n';
                if (!out)
                {
                    return std::unexpected(fmt::format("failed to write '{}'", temp.string()));
                }
            }

            std::error_code ec;
            std::filesystem::rename(temp, target, ec);
            if (ec)
            {
                std::filesystem::remove(temp, ec);
                return std::unexpected(
                    fmt::format("failed to rename '{}' over '{}'", temp.string(), target.string()));
            }
            return {};
        }
    }

    VoidResult PrefabSerialize::Save(const Scene& scene, const TypeRegistry& registry,
                                     const path& sourcePath)
    {
        // Read the existing source so unknown keys (comments-as-keys, future fields, hand-authored
        // extras) survive — the prefab is patched, not regenerated. A missing or malformed file
        // starts from an empty document.
        json prefab = ReadJsonObject(sourcePath).value_or(json::object());

        const vector<Entity> ordered = HierarchyOrderedEntities(scene);

        // The live entity's slot index is its stable per-document round-trip id: it never changes
        // across an editing session (undo even restores the exact handle), so it aligns a live
        // entity to its source object across add / delete / reorder. A Reference field maps a live
        // slot index to that entity's position in the saved order — the prefab-local index the
        // cooker validates and the loader remaps on spawn.
        unordered_map<u32, usize> indexInOrder;
        for (usize i = 0; i < ordered.size(); ++i)
        {
            indexInOrder.emplace(ordered[i].Index, i);
        }
        const function<i64(u32)> referenceIndex = [&indexInOrder](u32 liveIndex) -> i64
        {
            const auto it = indexInOrder.find(liveIndex);
            return it == indexInOrder.end() ? -1 : static_cast<i64>(it->second);
        };
        const JsonFieldHooks hooks = MakeWriteHooks(referenceIndex);

        // Match each live entity to the source object carrying its id, so a patched entity keeps
        // every key the writer does not understand. A source object with no id falls back to
        // positional order (an id-less hand-authored source) consumed in declaration order.
        const json* existingEntities = prefab.contains("entities") && prefab["entities"].is_array()
                                           ? &prefab["entities"]
                                           : nullptr;
        unordered_map<u64, const json*> sourceById;
        vector<const json*> sourcePositional;
        if (existingEntities != nullptr)
        {
            for (const json& entity : *existingEntities)
            {
                if (!entity.is_object())
                {
                    continue;
                }
                const auto idIt = entity.find(string{EntityIdKey});
                if (idIt != entity.end() && idIt->is_number_unsigned())
                {
                    sourceById.emplace(idIt->get<u64>(), &entity);
                }
                else
                {
                    sourcePositional.push_back(&entity);
                }
            }
        }

        json entities = json::array();
        usize positionalCursor = 0;
        for (const Entity entity : ordered)
        {
            const u64 id = entity.Index;

            // Start from the matching source object (preserving its unknown keys), else the next
            // id-less positional source object, else a fresh object.
            json out = json::object();
            const auto matchById = sourceById.find(id);
            if (matchById != sourceById.end())
            {
                out = *matchById->second;
            }
            else if (positionalCursor < sourcePositional.size())
            {
                out = *sourcePositional[positionalCursor++];
            }

            // Patch the components object in place over whatever the matched source carried, so an
            // unknown component-level key survives alongside the rewritten component fields.
            const json sourceComponents =
                out.contains("components") ? out["components"] : json::object();

            out[string{EntityIdKey}] = id;
            out["components"] = WriteComponents(scene, entity, registry, sourceComponents, hooks);
            entities.push_back(std::move(out));
        }

        prefab["entities"] = std::move(entities);

        return WriteAtomic(prefab, sourcePath);
    }
}
