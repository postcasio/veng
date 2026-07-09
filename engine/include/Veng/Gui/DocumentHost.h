#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Gui/Document.h>

namespace Veng
{
    class AssetManager;
    class TypeRegistry;
}

namespace Veng::Gui
{
    class UIDocument;

    /// @brief A cooked UI document made live: lazy load, instantiate, bind, and dirty-gate.
    ///
    /// The document lifecycle every consumer otherwise hand-rolls, owned once and stripped of
    /// presentation: the host holds the cooked UIDocument recipe id and the instantiated live tree,
    /// loads lazily on the first Drive (a failed load is latched and logged once, never retried per
    /// frame), instantiates with fonts resolved through the asset manager, binds the game's
    /// BindingContext through the borrowed TypeRegistry, and refreshes those bindings each Drive.
    /// It owns no viewport and no GPU target — where the live document *goes* is a presenter's
    /// concern (Gui::DocumentLayer for a viewport's screen-space layer stack, Gui::DocumentTexture
    /// for an HDR render target). A live document can also be supplied directly through SetDocument,
    /// for an imperatively-built tree or a recipe a consumer instantiates itself.
    ///
    /// The host borrows the asset manager, the type registry, and the bound context; all three must
    /// outlive it. It owns the document — dropping the host destroys the tree.
    class DocumentHost
    {
    public:
        /// @brief Constructs an id-driven host; the document loads and instantiates on first Drive.
        /// @param assets      The asset manager the recipe and its fonts load through; borrowed.
        /// @param types       The registry the binding context's field paths resolve through; borrowed.
        /// @param documentId  The cooked UIDocument asset to instantiate.
        DocumentHost(AssetManager& assets, const TypeRegistry& types, AssetId documentId);

        /// @brief Constructs an injection-only host; the live document is supplied by SetDocument.
        ///
        /// No recipe id is held, so Drive never loads or instantiates on its own — the document must
        /// be handed in through SetDocument. For a consumer that owns its recipe→instance decision
        /// (an imperatively-built tree, or a recipe instantiated only once its async handle loads).
        /// @param assets  The asset manager an injected document resolves its assets through; borrowed.
        /// @param types   The registry the binding context's field paths resolve through; borrowed.
        DocumentHost(AssetManager& assets, const TypeRegistry& types);

        DocumentHost(const DocumentHost&) = delete;
        DocumentHost& operator=(const DocumentHost&) = delete;

        /// @brief Sets the binding context the document's `{path}` bindings resolve against.
        ///
        /// Bound immediately when the document is live, else on instantiate. Passing nullptr clears
        /// the binding. The context is borrowed and must outlive the host (or be cleared first).
        /// @param context  The game-owned binding context, or nullptr to clear.
        void SetContext(BindingContext* context);

        /// @brief Sets a callback run after the document is instantiated and bound.
        ///
        /// Invoked once inside the lazy load — after instantiate and bind — and invoked immediately
        /// if the document is already live when set, so the ordering of this call against the first
        /// Drive is a non-issue. This is where a consumer resolves element pointers and does one-time
        /// setup; it re-runs on any future re-instantiate (SetDocument or Recreate), so cached
        /// pointers stay correct by construction. Passing an empty function clears the callback.
        /// @param callback  The callback receiving the live document, or an empty function to clear.
        void SetOnInstantiate(function<void(Document&)> callback);

        /// @brief Supplies a live document directly, binding it and running the on-instantiate hook.
        ///
        /// Replaces any current tree with the given one, binds the current context to it, and runs
        /// the SetOnInstantiate callback — the same adopt path the id-driven lazy load takes. Passing
        /// null drops the current document without adopting a replacement. Used by a consumer that
        /// instantiates its own recipe or builds a tree imperatively.
        /// @param document  The live document to adopt, or null to clear.
        void SetDocument(Unique<Document> document);

        /// @brief Drops the live document so the next Drive re-instantiates it from the recipe.
        ///
        /// Clears the tree and resets the load latch; an id-driven host re-loads, re-instantiates,
        /// re-binds, and re-runs the on-instantiate hook on the next Drive. An injection-only host is
        /// simply left empty until SetDocument supplies a new tree. A presenter observing the changed
        /// document pointer re-presents it.
        void Recreate();

        /// @brief Ensures the document is live and bound, refreshes its bindings, and returns it.
        ///
        /// Loads and instantiates on the first call for an id-driven host (nullptr thereafter when
        /// the load failed), then refreshes the bound context's `{path}` bindings (a no-op when the
        /// context version is unchanged). Call once per frame; a presenter drives layout and draw
        /// from the returned document.
        /// @return The live document, or nullptr while unavailable.
        Document* Drive();

        /// @brief Returns the live document, or nullptr before the first Drive (or after a failed load).
        [[nodiscard]] Document* Get() const { return m_Document.get(); }

    private:
        /// @brief Loads, instantiates, and binds the document once; false until it succeeds.
        [[nodiscard]] bool EnsureDocument();

        /// @brief Adopts a live tree: stores it, binds the context, and runs the on-instantiate hook.
        void AdoptDocument(Unique<Document> document);

        /// @brief The borrowed asset manager the recipe and its fonts load through.
        AssetManager& m_Assets;
        /// @brief The borrowed registry the binding context's field paths resolve through.
        const TypeRegistry& m_Types;
        /// @brief The cooked document asset to instantiate; invalid for an injection-only host.
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
    };
}
