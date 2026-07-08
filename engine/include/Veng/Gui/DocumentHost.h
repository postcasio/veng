#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Gui/Document.h>

namespace Veng
{
    class AssetManager;
    class TypeRegistry;

    namespace Renderer
    {
        class Viewport;
    }
}

namespace Veng::Gui
{
    class UIDocument;

    /// @brief A cooked UI document made live: lazy load, instantiate, bind, and viewport attach.
    ///
    /// The lifecycle every document consumer otherwise hand-rolls, owned once: the host holds the
    /// cooked UIDocument recipe handle and the instantiated live tree, loads lazily on the first
    /// Attach (a failed load is latched and logged once, never retried per frame), instantiates
    /// with fonts resolved through the asset manager, binds the game's BindingContext through the
    /// borrowed TypeRegistry, and re-attaches whenever the target viewport changes or is recreated
    /// (a viewport's destruction clears the document's host back-reference, so an open/close cycle
    /// re-attaches transparently). The interactive flag is reapplied on every attach.
    ///
    /// The host borrows the asset manager, the type registry, and the bound context; all three
    /// must outlive it. It owns the document — dropping the host detaches and destroys the tree.
    class DocumentHost
    {
    public:
        /// @brief Constructs the host without loading anything; the document loads on first Attach.
        /// @param assets      The asset manager the recipe and its fonts load through; borrowed.
        /// @param types       The registry the binding context's field paths resolve through; borrowed.
        /// @param documentId  The cooked UIDocument asset to instantiate.
        DocumentHost(AssetManager& assets, const TypeRegistry& types, AssetId documentId);

        DocumentHost(const DocumentHost&) = delete;
        DocumentHost& operator=(const DocumentHost&) = delete;

        /// @brief Sets the binding context the document's `{path}` bindings resolve against.
        ///
        /// Bound immediately when the document is live, else on instantiate. Passing nullptr
        /// clears the binding. The context is borrowed and must outlive the host (or be cleared
        /// first).
        /// @param context  The game-owned binding context, or nullptr to clear.
        void SetContext(BindingContext* context);

        /// @brief Sets whether the document routes input (see Document::SetInteractive).
        ///
        /// Applied immediately when the document is live and reapplied on every attach, so the
        /// flag survives the viewport being destroyed and recreated across an open/close cycle.
        /// @param interactive  True to route input into the document, false for display-only.
        void SetInteractive(bool interactive);

        /// @brief Sets a callback run after the document is instantiated and bound.
        ///
        /// Invoked once inside the lazy load — after instantiate and bind, before the first attach —
        /// and invoked immediately if the document is already live when set, so the ordering of this
        /// call against the first Attach is a non-issue. This is where a consumer resolves element
        /// pointers and does one-time setup; it re-runs on any future re-instantiate, so cached
        /// pointers stay correct by construction. Passing an empty function clears the callback.
        /// @param callback  The callback receiving the live document, or an empty function to clear.
        void SetOnInstantiate(function<void(Document&)> callback);

        /// @brief Ensures the document is loaded, bound, and attached to the given viewport.
        ///
        /// Loads and instantiates on first call (nullptr thereafter when the load failed), moves
        /// the document onto the viewport when it is detached or attached elsewhere, and reapplies
        /// the interactive flag on a fresh attach. Call once per frame with the live viewport; the
        /// engine drives the attached document's layout and draw from there.
        /// @param viewport  The viewport to host the document.
        /// @param layer     The document's layer in the viewport's stack (bottom to top).
        /// @return The live document, or nullptr while unavailable.
        Document* Attach(Renderer::Viewport& viewport, i32 layer = 0);

        /// @brief Returns the live document, or nullptr before the first Attach (or after a
        /// failed load).
        [[nodiscard]] Document* Get() const { return m_Document.get(); }

    private:
        /// @brief Loads, instantiates, and binds the document once; false until it succeeds.
        [[nodiscard]] bool EnsureDocument();

        /// @brief The borrowed asset manager the recipe and its fonts load through.
        AssetManager& m_Assets;
        /// @brief The borrowed registry the binding context's field paths resolve through.
        const TypeRegistry& m_Types;
        /// @brief The cooked document asset to instantiate.
        AssetId m_DocumentId;
        /// @brief The cooked recipe, kept resident for the live tree's dependencies.
        AssetHandle<UIDocument> m_Recipe;
        /// @brief The live document tree; owned, self-detaching on destruction.
        Unique<Document> m_Document;
        /// @brief The bound context, or nullptr; borrowed.
        BindingContext* m_Context = nullptr;
        /// @brief The callback run after (re)instantiate, or empty; the resolve-elements-once hook.
        function<void(Document&)> m_OnInstantiate;
        /// @brief Whether the document load was attempted (latched so a miss is not retried).
        bool m_LoadAttempted = false;
        /// @brief The interactive flag applied on attach (and live changes).
        bool m_Interactive = false;
    };
}
