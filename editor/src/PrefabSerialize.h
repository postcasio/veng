#pragma once

#include <Veng/Result.h>
#include <Veng/Veng.h>

namespace Veng
{
    class Scene;
    class TypeRegistry;
    struct Entity;
}

namespace VengEditor
{
    /// @brief Writes a live Scene back to its .prefab.json source — the inverse of the cooker's read.
    ///
    /// A prefab editor edits a spawned Scene; saving persists it as .prefab.json source the cooker
    /// re-reads. The writer walks the scene's entities (roots and their Hierarchy subtrees, in a
    /// stable hierarchy order) and, per entity, emits its components through the name-keyed
    /// reflection walk — each FieldClass arm the exact inverse of PrefabImporter::BindField. It
    /// round-trips the preserve-unknown-keys way (the texture/material/level editor idiom), keyed
    /// to a stable per-entity id so add/delete/reorder re-aligns to the right source object rather
    /// than clobbering hand-authored content. The writer lives in the editor exe so libveng /
    /// libveng_editor stay JSON-free.
    namespace PrefabSerialize
    {
        /// @brief The JSON key carrying an entity's stable, optional round-trip id.
        ///
        /// Added to the .prefab.json schema so the writer can match a live entity to the source
        /// object it came from after entities are added, deleted, or reordered. Additive: the
        /// cooker reads and preserves it, and an absent id falls back to positional order, so
        /// existing sources still cook.
        inline constexpr Veng::string_view EntityIdKey = "id";

        /// @brief Serializes @p scene back over its existing source JSON, preserving unknown keys.
        ///
        /// Reads the file at @p sourcePath (an empty/missing/malformed file is treated as no prior
        /// source), patches the "entities" array in place — matching each live entity to a source
        /// object by its EntityIdKey (else positionally for an id-less object), rewriting the
        /// component keys the reflection walk understands and preserving every other key — then
        /// writes the result back atomically (a temp sibling + rename, so a failed write never
        /// truncates the only copy of hand-authored work). A live entity with no source match is
        /// appended with a freshly minted id; a source object with no live match is dropped.
        ///
        /// @param scene      The live scene to persist; its entities (roots + Hierarchy subtrees)
        ///                    are emitted in a stable hierarchy order.
        /// @param registry   Type registry whose descriptors drive the per-component field walk.
        /// @param sourcePath  The .prefab.json file to round-trip.
        /// @return Empty on success; an error string on parse or I/O failure.
        Veng::VoidResult Save(const Veng::Scene& scene, const Veng::TypeRegistry& registry,
                              const Veng::path& sourcePath);
    }
}
