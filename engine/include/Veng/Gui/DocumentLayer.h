#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    namespace Gui
    {
        class Document;
        class DocumentHost;
    }

    namespace Renderer
    {
        class Viewport;
    }
}

namespace Veng::Gui
{
    /// @brief Presents a DocumentHost's live document on a Viewport's screen-space layer stack.
    ///
    /// The screen-space presenter of a Gui::DocumentHost: it registers the host's live document into
    /// a viewport's ordered layer stack (Viewport::AttachDocument), where the viewport's compositor
    /// draws it over the scene after tonemap (LDR, un-bloomed). It re-attaches whenever the target
    /// viewport changes or is recreated (a viewport's destruction clears the document's host
    /// back-reference, so an open/close cycle re-attaches transparently) and whenever the host
    /// re-instantiates its document (SetDocument or Recreate replaces the tree). It carries the
    /// interactivity flag — whether the layer takes input, a layer-stack concern — reapplied on every
    /// attach. Dropping the layer detaches the document without destroying it.
    ///
    /// The layer borrows the host and must not outlive it. Thin by design: a host reference, a layer
    /// index, an interactivity flag, and the attach/detach bookkeeping.
    class DocumentLayer
    {
    public:
        /// @brief Constructs a layer presenting the host's document at the given stack index.
        /// @param host   The document host whose live document this layer presents; borrowed.
        /// @param layer  The document's index in the viewport's stack (bottom to top).
        explicit DocumentLayer(DocumentHost& host, i32 layer = 0);

        /// @brief Detaches the presented document from its host viewport, without destroying it.
        ~DocumentLayer();

        DocumentLayer(const DocumentLayer&) = delete;
        DocumentLayer& operator=(const DocumentLayer&) = delete;

        /// @brief Sets whether the presented document routes input (see Document::SetInteractive).
        ///
        /// Applied immediately when the document is live and reapplied on every attach, so the flag
        /// survives the viewport being destroyed and recreated across an open/close cycle.
        /// @param interactive  True to route input into the document, false for display-only.
        void SetInteractive(bool interactive);

        /// @brief Drives the host and ensures its document is attached to the given viewport.
        ///
        /// Drives the host (lazy load, instantiate, bind, refresh bindings), then moves the live
        /// document onto @p viewport when it is detached, attached elsewhere, or was re-instantiated,
        /// reapplying the interactive flag on a fresh attach. Call once per frame with the live
        /// viewport; the engine drives the attached document's layout and draw from there.
        /// @param viewport  The viewport to present the document on.
        /// @return The live document, or nullptr while unavailable.
        Document* Present(Renderer::Viewport& viewport);

        /// @brief Returns the host's live document, or nullptr before the first Present.
        [[nodiscard]] Document* Get() const;

    private:
        /// @brief The borrowed document host whose live document this layer presents.
        DocumentHost& m_Host;
        /// @brief The document's index in the viewport's layer stack.
        i32 m_Layer;
        /// @brief The interactive flag applied on attach.
        bool m_Interactive = false;
    };
}
