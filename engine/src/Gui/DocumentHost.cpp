#include <Veng/Gui/DocumentHost.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Viewport.h>

namespace Veng::Gui
{
    DocumentHost::DocumentHost(AssetManager& assets, const TypeRegistry& types,
                               const AssetId documentId)
        : m_Assets(assets), m_Types(types), m_DocumentId(documentId)
    {
    }

    void DocumentHost::SetContext(BindingContext* context)
    {
        m_Context = context;
        if (m_Document != nullptr)
        {
            m_Document->BindContext(m_Context, m_Context != nullptr ? &m_Types : nullptr);
        }
    }

    void DocumentHost::SetInteractive(const bool interactive)
    {
        m_Interactive = interactive;
        if (m_Document != nullptr)
        {
            m_Document->SetInteractive(interactive);
        }
    }

    bool DocumentHost::EnsureDocument()
    {
        if (m_Document != nullptr)
        {
            return true;
        }
        if (m_LoadAttempted)
        {
            return false;
        }
        m_LoadAttempted = true;

        const AssetResult<AssetHandle<UIDocument>> recipe =
            m_Assets.LoadSync<UIDocument>(m_DocumentId);
        if (!recipe.has_value())
        {
            Log::Error("Gui::DocumentHost: document {:#018x} load failed: {}", m_DocumentId.Value,
                       recipe.error().Detail);
            return false;
        }
        m_Recipe = *recipe;

        m_Document = Document::Instantiate(*m_Recipe.Get(), m_Assets);
        if (m_Context != nullptr)
        {
            m_Document->BindContext(m_Context, &m_Types);
        }
        return true;
    }

    Document* DocumentHost::Attach(Renderer::Viewport& viewport, const i32 layer)
    {
        if (!EnsureDocument())
        {
            return nullptr;
        }

        if (m_Document->GetHostViewport() != &viewport)
        {
            // A destroyed viewport already cleared the back-reference; an attach elsewhere must
            // release the old host first (a document attaches to at most one viewport).
            if (m_Document->GetHostViewport() != nullptr)
            {
                m_Document->GetHostViewport()->DetachDocument(*m_Document);
            }
            viewport.AttachDocument(*m_Document, layer);
            m_Document->SetInteractive(m_Interactive);
        }
        return m_Document.get();
    }
}
