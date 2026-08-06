#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Path.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <VengEditor/CookRequest.h>
#include <VengEditor/EditorPanel.h>

/// @brief EditorRegistry — the editor's side of the module contract.
///
/// The forward declaration lives in <Veng/Module/Module.h> (Veng::EditorRegistry);
/// this header supplies its full definition, seen only by libveng_editor and the
/// editor modules that register into it. A non-editor host passes Editor = nullptr.
///
/// A loaded game editor module registers: per-AssetTypeId editor factories,
/// game-contributed custom panels, and per-TypeId custom inspector widgets.

namespace Veng
{
    class AssetManager;
    class TaskSystem;
    namespace Renderer
    {
        class Context;
    }
    namespace Audio
    {
        class AudioEngine;
    }

    /// @brief Engine services an asset-editor factory receives when it opens a panel.
    ///
    /// A game editor module registers its factories while the module loads, before the engine's
    /// AssetManager and render Context exist, so a factory cannot capture them at registration. This
    /// context is passed to OpenEditor instead — built by the host at open time, when the services
    /// are live — so a game panel can load, inspect, audition, and save the asset it edits with the
    /// same host seams the built-in editors get.
    struct AssetEditorContext
    {
        /// @brief The asset manager the panel loads and hot-reloads its asset through.
        AssetManager& Assets;
        /// @brief The render context, for a panel that builds GPU resources (a preview target).
        Renderer::Context& Context;
        /// @brief The host audio engine, for a panel that auditions sound through PlayGenerator.
        Audio::AudioEngine& Audio;
        /// @brief The host task system, for a panel that offloads heavy work (a bake, an offline
        ///        render, an export) off the UI thread, the same one the cook-on-demand runs on.
        TaskSystem& Tasks;
        /// @brief The asset's authoring source file, the panel's save target; empty when unresolved.
        path SourcePath;
        /// @brief The recook seam, bound over the host's cook-on-demand; empty when no project is
        ///        configured. A panel that saves triggers its in-process recook and hot-reload here.
        VengEditor::CookDriver Cook;
    };

    /// @brief Factory that mints an EditorPanel for a given asset.
    ///
    /// Pure-virtual so a module supplies its own concrete editor without the
    /// registry knowing the panel's concrete type.
    class AssetEditorFactory
    {
    public:
        virtual ~AssetEditorFactory() = default;
        /// @brief Creates and returns the editor panel for the given asset id.
        /// @param id   The asset to open.
        /// @param ctx  Live engine services the panel may capture (asset manager, render context).
        [[nodiscard]] virtual Unique<VengEditor::EditorPanel>
        OpenEditor(AssetId id, const AssetEditorContext& ctx) = 0;
    };

    /// @brief Inspector widget function for a single field.
    ///
    /// Overrides the inspector's built-in widget for a given TypeId.
    using FieldWidgetFn = function<void(void* fieldPtr, const FieldDescriptor& field)>;

    /// @brief Holds the per-AssetTypeId editor factories, game-contributed panels,
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
        void RegisterAssetEditor(AssetTypeId type, Unique<AssetEditorFactory> factory)
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
        [[nodiscard]] AssetEditorFactory* AssetEditorFor(AssetTypeId type) const
        {
            const auto it = m_AssetEditors.find(type);
            return it == m_AssetEditors.end() ? nullptr : it->second.get();
        }

        /// @brief Creates an editor panel for an asset, or nullptr when its type has
        /// no registered factory.
        /// @param type The asset type whose factory to use.
        /// @param id   The asset to open.
        /// @param ctx  Live engine services passed through to the factory's OpenEditor.
        [[nodiscard]] Unique<VengEditor::EditorPanel>
        CreateEditorFor(AssetTypeId type, AssetId id, const AssetEditorContext& ctx) const
        {
            AssetEditorFactory* factory = AssetEditorFor(type);
            return factory == nullptr ? nullptr : factory->OpenEditor(id, ctx);
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
        unordered_map<AssetTypeId, Unique<AssetEditorFactory>> m_AssetEditors;
        vector<Unique<VengEditor::EditorPanel>> m_Panels;
        unordered_map<TypeId, FieldWidgetFn> m_FieldWidgets;
    };
}
