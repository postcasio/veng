#include <Veng/Gui/DocumentHost.h>

#include <utility>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Log.h>

namespace Veng::Gui
{
    DocumentHost::DocumentHost(AssetManager& assets, const TypeRegistry& types,
                               const AssetId documentId)
        : m_Assets(assets), m_Types(types), m_DocumentId(documentId)
    {
    }

    DocumentHost::DocumentHost(AssetManager& assets, const TypeRegistry& types)
        : m_Assets(assets), m_Types(types)
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

    void DocumentHost::SetOnInstantiate(function<void(Document&)> callback)
    {
        m_OnInstantiate = std::move(callback);
        if (m_Document != nullptr && m_OnInstantiate)
        {
            m_OnInstantiate(*m_Document);
        }
    }

    void DocumentHost::SetDocument(Unique<Document> document)
    {
        if (document == nullptr)
        {
            m_Document.reset();
            return;
        }
        AdoptDocument(std::move(document));
    }

    void DocumentHost::Recreate()
    {
        m_Document.reset();
        m_LoadAttempted = false;
    }

    void DocumentHost::AdoptDocument(Unique<Document> document)
    {
        m_Document = std::move(document);
        if (m_Context != nullptr)
        {
            m_Document->BindContext(m_Context, &m_Types);
        }
        if (m_OnInstantiate)
        {
            m_OnInstantiate(*m_Document);
        }
    }

    bool DocumentHost::EnsureDocument()
    {
        if (m_Document != nullptr)
        {
            return true;
        }
        // An injection-only host has no recipe id to load — it stays empty until SetDocument feeds it.
        if (!m_DocumentId.IsValid() || m_LoadAttempted)
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

        AdoptDocument(Document::Instantiate(*m_Recipe.Get(), m_Assets));
        return true;
    }

    Document* DocumentHost::Drive()
    {
        if (!EnsureDocument())
        {
            return nullptr;
        }
        m_Document->UpdateBindings();
        return m_Document.get();
    }
}
