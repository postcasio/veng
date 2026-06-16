#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <VengEditor/EditorPanel.h>

// EditorRegistry — the editor's side of the module contract. The forward
// declaration lives in <Veng/Module/Module.h> (Veng::EditorRegistry); this
// header supplies its full definition, seen only by libveng_editor and the
// editor modules that register into it. libveng never sees the definition: a
// non-editor host passes Editor = nullptr.
//
// A loaded game editor module registers into this: per-AssetType editor
// factories (double-click an asset → open its editor panel), game-contributed
// custom panels, and per-type custom inspector widgets.

namespace Veng
{
    // Mints an EditorPanel for a given asset. Pure-virtual so a module supplies
    // its own concrete editor (e.g. a texture editor) without the registry
    // knowing the panel's type.
    class AssetEditorFactory
    {
    public:
        virtual ~AssetEditorFactory() = default;
        [[nodiscard]] virtual Unique<VengEditor::EditorPanel> OpenEditor(AssetId id) = 0;
    };

    // Draws one field's inspector widget. Overrides the inspector's built-in
    // widget for a given TypeId. Declared here and consumed by the inspector
    // panel.
    using FieldWidgetFn = function<void(void* fieldPtr, const FieldDescriptor& field)>;

    class EditorRegistry
    {
    public:
        // Register an asset editor for a type. Double-clicking an asset of this
        // type in the asset browser opens its editor through this factory.
        void RegisterAssetEditor(AssetType type, Unique<AssetEditorFactory> factory)
        {
            m_AssetEditors[type] = std::move(factory);
        }

        // Register a game-contributed custom panel. The host adopts it into its
        // panel set alongside the built-ins.
        void RegisterPanel(Unique<VengEditor::EditorPanel> panel)
        {
            m_Panels.push_back(std::move(panel));
        }

        // Register a custom inspector widget for a type, overriding the built-in
        // widget the inspector picks from the field's FieldClass.
        void RegisterFieldWidget(TypeId type, FieldWidgetFn widget)
        {
            m_FieldWidgets[type] = std::move(widget);
        }

        // The factory for an asset type, or nullptr when none is registered.
        [[nodiscard]] AssetEditorFactory* AssetEditorFor(AssetType type) const
        {
            const auto it = m_AssetEditors.find(type);
            return it == m_AssetEditors.end() ? nullptr : it->second.get();
        }

        // The custom widget for a type, or nullptr when none is registered.
        [[nodiscard]] const FieldWidgetFn* FieldWidgetFor(TypeId type) const
        {
            const auto it = m_FieldWidgets.find(type);
            return it == m_FieldWidgets.end() ? nullptr : &it->second;
        }

        // The game-contributed panels, transferred to the host at load time.
        [[nodiscard]] vector<Unique<VengEditor::EditorPanel>>& Panels() { return m_Panels; }

    private:
        unordered_map<AssetType, Unique<AssetEditorFactory>> m_AssetEditors;
        vector<Unique<VengEditor::EditorPanel>> m_Panels;
        unordered_map<TypeId, FieldWidgetFn> m_FieldWidgets;
    };
}
