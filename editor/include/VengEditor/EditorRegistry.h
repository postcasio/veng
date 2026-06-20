#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <VengEditor/EditorPanel.h>

/// @brief EditorRegistry — the editor's side of the module contract.
///
/// The forward declaration lives in <Veng/Module/Module.h> (Veng::EditorRegistry);
/// this header supplies its full definition, seen only by libveng_editor and the
/// editor modules that register into it. A non-editor host passes Editor = nullptr.
///
/// A loaded game editor module registers: per-AssetType editor factories,
/// game-contributed custom panels, and per-TypeId custom inspector widgets.

namespace Veng
{
    /// @brief Factory that mints an EditorPanel for a given asset.
    ///
    /// Pure-virtual so a module supplies its own concrete editor without the
    /// registry knowing the panel's concrete type.
    class AssetEditorFactory
    {
    public:
        virtual ~AssetEditorFactory() = default;
        /// @brief Creates and returns the editor panel for the given asset id.
        [[nodiscard]] virtual Unique<VengEditor::EditorPanel> OpenEditor(AssetId id) = 0;
    };

    /// @brief Inspector widget function for a single field.
    ///
    /// Overrides the inspector's built-in widget for a given TypeId.
    using FieldWidgetFn = function<void(void* fieldPtr, const FieldDescriptor& field)>;

    /// @brief Holds the per-AssetType editor factories, game-contributed panels,
    /// and per-TypeId inspector widget overrides registered by a game editor module.
    class EditorRegistry
    {
    public:
        /// @brief Registers an asset editor for a type.
        ///
        /// Double-clicking an asset of this type opens its editor through this factory.
        /// First-write-wins: a game module's factory takes precedence over built-ins.
        /// @param type    The asset type to bind this factory to.
        /// @param factory The factory instance; ownership is transferred.
        void RegisterAssetEditor(AssetType type, Unique<AssetEditorFactory> factory)
        {
            m_AssetEditors.try_emplace(type, std::move(factory));
        }

        /// @brief Registers a game-contributed custom panel.
        ///
        /// The host adopts it into its panel set alongside the built-ins.
        /// @param panel The panel instance; ownership is transferred.
        void RegisterPanel(Unique<VengEditor::EditorPanel> panel)
        {
            m_Panels.push_back(std::move(panel));
        }

        /// @brief Registers a custom inspector widget for a type, overriding the
        /// built-in widget selected from the field's FieldClass.
        /// @param type   TypeId the widget handles.
        /// @param widget The widget function; replaces any previous registration.
        void RegisterFieldWidget(TypeId type, FieldWidgetFn widget)
        {
            m_FieldWidgets[type] = std::move(widget);
        }

        /// @brief Returns the factory for an asset type, or nullptr when none is registered.
        [[nodiscard]] AssetEditorFactory* AssetEditorFor(AssetType type) const
        {
            const auto it = m_AssetEditors.find(type);
            return it == m_AssetEditors.end() ? nullptr : it->second.get();
        }

        /// @brief Creates an editor panel for an asset, or nullptr when its type has
        /// no registered factory.
        [[nodiscard]] Unique<VengEditor::EditorPanel> CreateEditorFor(AssetType type,
                                                                      AssetId id) const
        {
            AssetEditorFactory* factory = AssetEditorFor(type);
            return factory == nullptr ? nullptr : factory->OpenEditor(id);
        }

        /// @brief Returns the custom widget for a type, or nullptr when none is registered.
        [[nodiscard]] const FieldWidgetFn* FieldWidgetFor(TypeId type) const
        {
            const auto it = m_FieldWidgets.find(type);
            return it == m_FieldWidgets.end() ? nullptr : &it->second;
        }

        /// @brief Returns the game-contributed panels; transferred to the host at load time.
        [[nodiscard]] vector<Unique<VengEditor::EditorPanel>>& Panels() { return m_Panels; }

    private:
        unordered_map<AssetType, Unique<AssetEditorFactory>> m_AssetEditors;
        vector<Unique<VengEditor::EditorPanel>> m_Panels;
        unordered_map<TypeId, FieldWidgetFn> m_FieldWidgets;
    };
}
