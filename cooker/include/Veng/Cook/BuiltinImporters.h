#pragma once

#include <Veng/Cook/Cooker.h>

namespace Veng::Cook
{
    /// @brief Registers the veng-free core importers (raw, texture, mesh, shader, vertex layout, material).
    ///
    /// These link nothing from libveng, so the veng-free bootstrap cooker (which cooks
    /// veng's own embedded core pack and must not form a build cycle through libveng) uses
    /// this set directly. A libveng-linked cooker registers the full set with
    /// RegisterBuiltinImporters instead.
    /// @param cooker  The cooker to register into.
    void RegisterCoreImporters(Cooker& cooker);

    /// @brief Registers the full built-in importer set: the core importers plus prefab and level.
    ///
    /// The one call a libveng-linked cooker (the full vengc, the editor's cook session) makes
    /// to register every built-in importer. It adds the prefab and level importers — which
    /// reuse libveng's reflection serializer (WriteFields) — on top of RegisterCoreImporters,
    /// so it is **absent from the veng-free bootstrap cooker**: referencing it would drag an
    /// unresolved libveng symbol into that link. The bootstrap cooks no prefabs or levels and
    /// registers RegisterCoreImporters alone.
    /// @param cooker  The cooker to register into.
    void RegisterBuiltinImporters(Cooker& cooker);

    /// @brief Registers the prefab importer.
    ///
    /// Links libveng's reflection serializer (WriteFields) and is therefore absent from the
    /// veng-free bootstrap cooker. Folded into RegisterBuiltinImporters; exposed individually
    /// for a cooker that wants the core set plus only this importer.
    /// @param cooker  The cooker to register into.
    void RegisterPrefabImporter(Cooker& cooker);

    /// @brief Registers the level importer.
    ///
    /// Links libveng's reflection serializer (WriteFields) and validates against the module's
    /// reflected TypeRegistry + SystemRegistry, so it is absent from the veng-free bootstrap
    /// cooker. Folded into RegisterBuiltinImporters; exposed individually for a cooker that
    /// wants the core set plus only this importer.
    /// @param cooker  The cooker to register into.
    void RegisterLevelImporter(Cooker& cooker);

    /// @brief Registers the input-map importer.
    ///
    /// Links libveng's reflection serializer (WriteFields) and is therefore absent from the
    /// veng-free bootstrap cooker. Needs no game module — an input map references only engine
    /// builtins (InputAction / Binding). Folded into RegisterBuiltinImporters; exposed
    /// individually for a cooker that wants the core set plus only this importer.
    /// @param cooker  The cooker to register into.
    void RegisterInputMapImporter(Cooker& cooker);

    /// @brief Registers the stylesheet importer.
    ///
    /// References libveng's Gui/ style vocabulary (StyleProperty and the flex enums) and links a
    /// cooker-only CSS tokenizer, so it is absent from the veng-free bootstrap cooker. Needs no game
    /// module — a stylesheet references only the engine's style properties. Folded into
    /// RegisterBuiltinImporters; exposed individually for a cooker that wants the core set plus only
    /// this importer.
    /// @param cooker  The cooker to register into.
    void RegisterStyleSheetImporter(Cooker& cooker);

    /// @brief Registers the UI-document importer.
    ///
    /// References libveng's Gui/ vocabulary (ElementKind/StyleProperty) and links cooker-only XML
    /// and CSS parsers, so it is absent from the veng-free bootstrap cooker. Needs no game module —
    /// a UI document references only the engine's element kinds and its font/texture/stylesheet
    /// assets by id. Folded into RegisterBuiltinImporters; exposed individually for a cooker that
    /// wants the core set plus only this importer.
    /// @param cooker  The cooker to register into.
    void RegisterUIDocumentImporter(Cooker& cooker);

    /// @brief Registers the table-schema and data-table importers.
    ///
    /// References libveng's DataTable column vocabulary (the cell-size and alignment rules the
    /// row layout is computed from), so it is absent from the veng-free bootstrap cooker — which
    /// needs it for nothing, the embedded core pack carrying no tables. Folded into
    /// RegisterBuiltinImporters; exposed individually for a cooker that wants the core set plus
    /// only these importers.
    /// @param cooker  The cooker to register into.
    void RegisterTableImporters(Cooker& cooker);
}
