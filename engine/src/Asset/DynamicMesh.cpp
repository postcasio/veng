#include <Veng/Asset/DynamicMesh.h>

#include <Veng/Asset/AssetManager.h>

namespace Veng
{
    DynamicMesh::DynamicMesh(AssetManager& assets, string name)
        : m_Assets(assets), m_Name(std::move(name))
    {
    }

    void DynamicMesh::Rebuild(MeshData data)
    {
        VE_ASSERT(!data.Vertices.empty() && !data.Indices.empty(),
                  "DynamicMesh::Rebuild: empty geometry for '{}' — hide the mesh at its "
                  "consumer or call Reset() instead",
                  m_Name);
        m_Queued = std::move(data);
        if (!m_InFlight)
        {
            StartQueuedBuild();
        }
    }

    bool DynamicMesh::Update()
    {
        bool swapped = false;
        if (m_InFlight && m_Back.IsLoaded())
        {
            m_Front = std::move(m_Back);
            m_Back = {};
            m_InFlight = false;
            swapped = true;
        }
        if (!m_InFlight && m_Queued.has_value())
        {
            StartQueuedBuild();
        }
        return swapped;
    }

    void DynamicMesh::Reset()
    {
        m_Front = {};
        m_Back = {};
        m_Queued.reset();
        m_InFlight = false;
    }

    void DynamicMesh::StartQueuedBuild()
    {
        m_Back = m_Assets.Build<Mesh>(std::move(*m_Queued), m_Name);
        m_Queued.reset();
        m_InFlight = true;
    }
}
