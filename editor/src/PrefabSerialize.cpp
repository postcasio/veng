#include "PrefabSerialize.h"

#include <Veng/Asset/Mesh.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        using json = nlohmann::json;

        // Writes one reflected field at obj+field.Offset into its JSON value. Each arm is the
        // exact inverse of PrefabImporter::BindField's matching FieldClass arm — read that switch
        // alongside this one. The remap maps a live entity's slot index to its prefab-local index
        // for Reference fields (the inverse of the loader's spawn-time remap).
        json WriteField(const void* obj, const FieldDescriptor& field, const TypeRegistry& registry,
                        const function<i64(u32 liveIndex)>& referenceIndex);

        // Emits each field of `type` at `obj` as a name-keyed JSON object — the per-component /
        // per-struct / per-variant-value body shared by the component walk and the Struct/Variant
        // recursion.
        json WriteFieldsObject(const void* obj, const TypeInfo& type, const TypeRegistry& registry,
                               const function<i64(u32 liveIndex)>& referenceIndex)
        {
            json out = json::object();
            for (const FieldDescriptor& field : type.Fields)
            {
                // An empty variant (no active alternative) is omitted: the read arm leaves an
                // absent variant empty, so emitting null — which BindField rejects as "expected an
                // object with a string 'type' key" — would break the round-trip. A null Reference
                // is distinct and is written as JSON null, which the reader does accept.
                if (field.Class == FieldClass::Variant)
                {
                    const TypeInfo& variant = registry.Info(field.Type);
                    const void* fieldPtr = static_cast<const u8*>(obj) + field.Offset;
                    if (variant.VariantActiveType(fieldPtr) == InvalidTypeId)
                    {
                        continue;
                    }
                }

                out[field.Name] =
                    WriteField(static_cast<const u8*>(obj), field, registry, referenceIndex);
            }
            return out;
        }

        json WriteField(const void* obj, const FieldDescriptor& field, const TypeRegistry& registry,
                        const function<i64(u32 liveIndex)>& referenceIndex)
        {
            const auto* fieldPtr = static_cast<const u8*>(obj) + field.Offset;

            switch (field.Class)
            {
            case FieldClass::Scalar:
            {
                // Scalars are bool/f32/i32/u32/u64, read at the field's exact byte width by its
                // leaf TypeId — the inverse of BindField's scalar coercion.
                const TypeId t = field.Type;
                if (t == TypeIdOf<bool>())
                {
                    bool v = false;
                    std::memcpy(&v, fieldPtr, sizeof(v));
                    return v;
                }
                if (t == TypeIdOf<f32>())
                {
                    f32 v = 0.0f;
                    std::memcpy(&v, fieldPtr, sizeof(v));
                    return v;
                }
                if (t == TypeIdOf<i32>())
                {
                    i32 v = 0;
                    std::memcpy(&v, fieldPtr, sizeof(v));
                    return v;
                }
                if (t == TypeIdOf<u32>())
                {
                    u32 v = 0;
                    std::memcpy(&v, fieldPtr, sizeof(v));
                    return v;
                }
                if (t == TypeIdOf<u64>())
                {
                    u64 v = 0;
                    std::memcpy(&v, fieldPtr, sizeof(v));
                    return v;
                }
                return nullptr;
            }

            case FieldClass::Vector:
            case FieldClass::Quaternion:
            case FieldClass::Matrix:
            {
                // A glm vector/quat/matrix is an array of its components in storage type — f32, or
                // u32 for an unsigned-integer vector — arity = byte size / component size. A quat
                // is [x,y,z,w] (glm memory layout), matching the read arm exactly.
                const bool unsignedVector = field.Type == TypeIdOf<uvec2>();
                const usize componentSize = unsignedVector ? sizeof(u32) : sizeof(f32);
                const usize size = registry.Info(field.Type).Size;
                const usize arity = size / componentSize;

                json out = json::array();
                if (unsignedVector)
                {
                    for (usize i = 0; i < arity; ++i)
                    {
                        u32 v = 0;
                        std::memcpy(&v, fieldPtr + i * sizeof(u32), sizeof(u32));
                        out.push_back(v);
                    }
                }
                else
                {
                    for (usize i = 0; i < arity; ++i)
                    {
                        f32 v = 0.0f;
                        std::memcpy(&v, fieldPtr + i * sizeof(f32), sizeof(f32));
                        out.push_back(v);
                    }
                }
                return out;
            }

            case FieldClass::String:
            {
                return *reinterpret_cast<const string*>(fieldPtr);
            }

            case FieldClass::AssetHandle:
            {
                // The handle stores the AssetId at offset 0 (pinned in AssetHandle.h); emit it as
                // a decimal unsigned integer, the JSON convention BindField reads back.
                u64 id = 0;
                std::memcpy(&id, fieldPtr, sizeof(id));
                return id;
            }

            case FieldClass::Enum:
            {
                // The low `size` bytes of the backing integer, written as an integer — the inverse
                // of BindField's low-byte enum write.
                const usize size = registry.Info(field.Type).Size;
                u64 bits = 0;
                std::memcpy(&bits, fieldPtr, size);
                return static_cast<i64>(bits);
            }

            case FieldClass::Reference:
            {
                // A live Entity reference maps back to a prefab-local index (the inverse of the
                // spawn-time remap); a null reference is JSON null.
                const auto& entity = *reinterpret_cast<const Entity*>(fieldPtr);
                if (entity.IsNull())
                {
                    return nullptr;
                }
                const i64 index = referenceIndex(entity.Index);
                if (index < 0)
                {
                    // The reference points outside the saved entity set; emit null rather than a
                    // dangling index the cooker would reject as out of range.
                    return nullptr;
                }
                return index;
            }

            case FieldClass::Struct:
            {
                return WriteFieldsObject(fieldPtr, registry.Info(field.Type), registry,
                                         referenceIndex);
            }

            case FieldClass::Variant:
            {
                // A variant is authored as { "type": <qualified name>, "value": {…} }. An empty
                // variant (no active alternative) is omitted entirely — the read arm leaves an
                // absent or empty-"type" variant empty, so omission round-trips byte-identically.
                const TypeInfo& info = registry.Info(field.Type);
                const TypeId active = info.VariantActiveType(fieldPtr);
                if (active == InvalidTypeId)
                {
                    return nullptr;
                }

                const TypeInfo& alt = registry.Info(active);
                const void* member = info.VariantActivePtrConst(fieldPtr);

                json out = json::object();
                out["type"] = alt.QualifiedName;
                out["value"] = WriteFieldsObject(member, alt, registry, referenceIndex);
                return out;
            }

            case FieldClass::Array:
            {
                // A dynamic array is a JSON array of its elements, walked through the descriptor's
                // type-erased shims (the inverse of an array read). Prefab sources do not yet cook
                // array fields (BindField rejects them), but the writer emits the natural inverse
                // so the arm is complete and a future cooker array arm round-trips.
                const usize count = field.ArraySize != nullptr ? field.ArraySize(fieldPtr) : 0;
                const TypeInfo& elem = registry.Info(field.ElementType);

                json out = json::array();
                for (usize i = 0; i < count; ++i)
                {
                    const void* element = field.ArrayElementConst(fieldPtr, i);
                    FieldDescriptor elementField;
                    elementField.Name = field.Name;
                    elementField.Type = field.ElementType;
                    elementField.Class = elem.Class;
                    elementField.Offset = 0;
                    out.push_back(WriteField(element, elementField, registry, referenceIndex));
                }
                return out;
            }
            }

            return nullptr;
        }

        // Patches the entity's `components` object: each live component the writer understands is
        // rewritten (reusing the source key's exact spelling when it already named that component,
        // so a "::Veng::Name"-spelled source stays byte-stable), and any source key that does not
        // resolve to a registered component type — comments-as-keys, future fields, hand-authored
        // extras — is preserved in place. A registered-type key the live entity no longer holds is
        // dropped (the component was removed). Components are emitted by ascending TypeId, so a
        // re-save produces a stable diff regardless of pool iteration order.
        json WriteComponents(const Scene& scene, Entity entity, const TypeRegistry& registry,
                             const json& sourceComponents,
                             const function<i64(u32 liveIndex)>& referenceIndex)
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
                if (sourceComponents.is_object())
                {
                    for (auto it = sourceComponents.begin(); it != sourceComponents.end(); ++it)
                    {
                        if (TypeNameMatches(info, it.key()))
                        {
                            key = it.key();
                            break;
                        }
                    }
                }

                out[key] = WriteFieldsObject(component, info, registry, referenceIndex);
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
        json prefab = json::object();
        {
            const std::ifstream file(sourcePath, std::ios::binary);
            if (file)
            {
                std::ostringstream contents;
                contents << file.rdbuf();
                const json parsed = json::parse(contents.str(), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                {
                    prefab = parsed;
                }
            }
        }

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
            out["components"] =
                WriteComponents(scene, entity, registry, sourceComponents, referenceIndex);
            entities.push_back(std::move(out));
        }

        prefab["entities"] = std::move(entities);

        return WriteAtomic(prefab, sourcePath);
    }
}
