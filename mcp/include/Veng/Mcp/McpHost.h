#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class TypeRegistry;
    class AssetManager;
    class Scene;

    namespace Renderer
    {
        class Viewport;
    }
}

namespace Veng::Mcp
{
    /// @brief The provider seam the app fills so the built-in tools reach live state.
    ///
    /// Mirrors VengModuleHost: the app hands the server the systems it wants reachable —
    /// references it owns and provider closures resolving per-frame state. The built-in
    /// tools capture the host by reference, so the host must outlive the server (the app
    /// owns both). Every accessor runs on the render thread during McpServer::Pump(), so a
    /// closure may freely touch engine state.
    ///
    /// A game fills CurrentWorld with its managed world and Viewport with the primary
    /// viewport; the editor fills them from the active document's scene and its panel
    /// viewports. A null CurrentWorld() (no world loaded, a closed document) makes the world
    /// tools return an empty result, never a null deref.
    struct McpHost
    {
        /// @brief Resolves a component's TypeId to its reflected fields.
        TypeRegistry& Types;

        /// @brief The asset system, for asset queries and id → name lookups.
        AssetManager& Assets;

        /// @brief Returns the scene an agent inspects, or null when no world is loaded.
        ///
        /// Called on the render thread at each world tool. A null return means "no world";
        /// every world tool handles it by returning an empty/so-stated result.
        function<Scene*()> CurrentWorld;

        /// @brief Resolves a viewport by name, or null when unknown or unset.
        ///
        /// Left unset here; the render tools that consume it arrive later.
        function<Renderer::Viewport*(string_view name)> Viewport;
    };
}
