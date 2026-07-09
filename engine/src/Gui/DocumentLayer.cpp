#include <Veng/Gui/DocumentLayer.h>

#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Renderer/Viewport.h>

namespace Veng::Gui
{
    DocumentLayer::DocumentLayer(DocumentHost& host, const i32 layer) : m_Host(host), m_Layer(layer)
    {
    }

    DocumentLayer::~DocumentLayer()
    {
        // Detach the document from its host viewport without destroying it (the host owns the tree).
        // A destroyed viewport already cleared the back-reference, so skip when there is none.
        Document* const document = m_Host.Get();
        if (document != nullptr && document->GetHostViewport() != nullptr)
        {
            document->GetHostViewport()->DetachDocument(*document);
        }
    }

    void DocumentLayer::SetInteractive(const bool interactive)
    {
        m_Interactive = interactive;
        if (Document* const document = m_Host.Get(); document != nullptr)
        {
            document->SetInteractive(interactive);
        }
    }

    Document* DocumentLayer::Present(Renderer::Viewport& viewport)
    {
        Document* const document = m_Host.Drive();
        if (document == nullptr)
        {
            return nullptr;
        }

        if (document->GetHostViewport() != &viewport)
        {
            // A destroyed viewport already cleared the back-reference; an attach elsewhere (or a
            // re-instantiated tree) must release the old host first — a document attaches to at most
            // one viewport.
            if (document->GetHostViewport() != nullptr)
            {
                document->GetHostViewport()->DetachDocument(*document);
            }
            viewport.AttachDocument(*document, m_Layer);
            document->SetInteractive(m_Interactive);
        }
        return document;
    }

    Document* DocumentLayer::Get() const
    {
        return m_Host.Get();
    }
}
