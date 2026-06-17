#include <VengEditor/NodeGraph/NodeGraph.h>

#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>

#include <algorithm>
#include <cstring>

namespace VengEditor
{
    NodeGraph::NodeGraph(CanConnectFn canConnect, PinShapeFn pinShape,
                         PropertySizeFn propertySize)
        : m_CanConnect(std::move(canConnect))
        , m_PinShape(std::move(pinShape))
        , m_PropertySize(std::move(propertySize))
    {
        VE_ASSERT(static_cast<bool>(m_CanConnect), "NodeGraph requires a CanConnect predicate");
        VE_ASSERT(static_cast<bool>(m_PinShape), "NodeGraph requires a PinShape callback");
        VE_ASSERT(static_cast<bool>(m_PropertySize), "NodeGraph requires a PropertySize callback");
    }

    NodeId NodeGraph::AddNode(NodeTypeId type)
    {
        Veng::u32 index;
        if (!m_FreeList.empty())
        {
            index = m_FreeList.back();
            m_FreeList.pop_back();
        }
        else
        {
            index = static_cast<Veng::u32>(m_Nodes.size());
            m_Nodes.emplace_back();
        }

        Node& node = m_Nodes[index];
        node.Type = type;
        node.Alive = true;
        node.Position = Veng::vec2{0.0f, 0.0f};
        node.Properties.assign(m_PropertySize(type), std::byte{0});

        NodeId id{index, node.Generation};
        m_Live.push_back(id);
        return id;
    }

    void NodeGraph::RemoveNode(NodeId node)
    {
        const Node* found = Lookup(node);
        if (found == nullptr)
            return; // stale / non-existent — a no-op

        // Drop every link touching this node from either endpoint.
        std::erase_if(m_Links, [&](const Link& link) {
            return link.From.Node == node || link.To.Node == node;
        });

        std::erase(m_Live, node);

        Node& slot = m_Nodes[node.Index];
        slot.Alive = false;
        ++slot.Generation; // recycle: a stale NodeId can never alias the next node
        m_FreeList.push_back(node.Index);
    }

    Veng::VoidResult NodeGraph::Connect(PinRef from, PinRef to)
    {
        if (!IsValid(from.Node) || !IsValid(to.Node))
            return std::unexpected("Connect: a referenced node does not exist");

        const NodePinShape fromShape = ShapeOf(from.Node);
        const NodePinShape toShape = ShapeOf(to.Node);

        // Direction: From must be a valid output, To a valid input.
        if (from.Pin >= fromShape.Outputs.size())
            return std::unexpected("Connect: From is not an output pin");
        if (to.Pin >= toShape.Inputs.size())
            return std::unexpected("Connect: To is not an input pin");

        // Arity: an input pin holds at most one link. Output pins fan out freely.
        for (const Link& link : m_Links)
        {
            if (link.To == to)
                return std::unexpected("Connect: input pin already has a link");
        }

        // Type: Wildcard is compatible with anything; otherwise the domain decides.
        const PinType& fromType = fromShape.Outputs[from.Pin];
        const PinType& toType = toShape.Inputs[to.Pin];
        const bool wildcard = fromType.Kind == PinType::Kind::Wildcard
            || toType.Kind == PinType::Kind::Wildcard;
        if (!wildcard && !m_CanConnect(fromType, toType))
            return std::unexpected("Connect: incompatible pin types");

        // Acyclicity: reject an edge that would close a cycle. The new edge runs
        // From's node -> To's node; if From is already reachable from To, it closes.
        if (Reaches(to.Node, from.Node))
            return std::unexpected("Connect: would introduce a cycle");

        m_Links.push_back(Link{from, to});
        return {};
    }

    void NodeGraph::Disconnect(const Link& link)
    {
        std::erase(m_Links, link);
    }

    void NodeGraph::MoveNode(NodeId node, Veng::vec2 canvasPos)
    {
        if (!IsValid(node))
            return;
        m_Nodes[node.Index].Position = canvasPos;
    }

    void NodeGraph::SetProperty(NodeId node, const Veng::FieldDescriptor& field,
                                std::span<const std::byte> bytes)
    {
        if (!IsValid(node))
            return;

        Veng::vector<std::byte>& buffer = m_Nodes[node.Index].Properties;
        VE_ASSERT(field.Offset + bytes.size() <= buffer.size(),
                  "SetProperty: field '{}' writes past the node's property buffer", field.Name);
        std::memcpy(buffer.data() + field.Offset, bytes.data(), bytes.size());
    }

    bool NodeGraph::IsValid(NodeId node) const
    {
        return Lookup(node) != nullptr;
    }

    NodeTypeId NodeGraph::GetTypeOf(NodeId node) const
    {
        const Node* found = Lookup(node);
        VE_ASSERT(found != nullptr, "GetTypeOf: stale or non-existent node");
        return found->Type;
    }

    std::span<const NodeId> NodeGraph::Nodes() const
    {
        return std::span<const NodeId>(m_Live);
    }

    std::span<const Link> NodeGraph::Links() const
    {
        return std::span<const Link>(m_Links);
    }

    Veng::vec2 NodeGraph::PositionOf(NodeId node) const
    {
        const Node* found = Lookup(node);
        VE_ASSERT(found != nullptr, "PositionOf: stale or non-existent node");
        return found->Position;
    }

    std::span<const std::byte> NodeGraph::PropertyBytes(NodeId node) const
    {
        const Node* found = Lookup(node);
        VE_ASSERT(found != nullptr, "PropertyBytes: stale or non-existent node");
        return std::span<const std::byte>(found->Properties);
    }

    Veng::vector<NodeId> NodeGraph::TopoOrder() const
    {
        // Kahn's algorithm over node-level edges, seeding and draining in
        // node-creation order (m_Live is creation-ordered) so the result is stable.
        Veng::vector<NodeId> order;
        order.reserve(m_Live.size());

        // In-degree per live node, counting distinct node-level predecessors only
        // once (two links between the same node pair must not double-count).
        Veng::vector<Veng::u32> indegree(m_Live.size(), 0);

        const auto liveIndexOf = [&](NodeId node) -> Veng::usize {
            for (Veng::usize i = 0; i < m_Live.size(); ++i)
                if (m_Live[i] == node)
                    return i;
            return m_Live.size();
        };

        for (Veng::usize i = 0; i < m_Live.size(); ++i)
        {
            const NodeId target = m_Live[i];
            // Distinct source nodes feeding target.
            Veng::vector<NodeId> sources;
            for (const Link& link : m_Links)
            {
                if (!(link.To.Node == target))
                    continue;
                if (std::find(sources.begin(), sources.end(), link.From.Node) == sources.end())
                    sources.push_back(link.From.Node);
            }
            indegree[i] = static_cast<Veng::u32>(sources.size());
        }

        // Ready queue as a creation-ordered scan, re-swept after each emission so
        // ties always resolve toward the earliest-created node.
        Veng::vector<bool> emitted(m_Live.size(), false);
        for (Veng::usize emittedCount = 0; emittedCount < m_Live.size(); ++emittedCount)
        {
            Veng::usize pick = m_Live.size();
            for (Veng::usize i = 0; i < m_Live.size(); ++i)
            {
                if (!emitted[i] && indegree[i] == 0)
                {
                    pick = i;
                    break;
                }
            }

            // A remaining cycle would leave no zero-indegree node; the graph is a
            // DAG by construction, so this never fires.
            VE_ASSERT(pick != m_Live.size(), "TopoOrder: graph is not acyclic");

            const NodeId picked = m_Live[pick];
            order.push_back(picked);
            emitted[pick] = true;

            // Decrement successors' in-degree, once per distinct successor node.
            Veng::vector<NodeId> successors;
            for (const Link& link : m_Links)
            {
                if (!(link.From.Node == picked))
                    continue;
                if (std::find(successors.begin(), successors.end(), link.To.Node) == successors.end())
                    successors.push_back(link.To.Node);
            }
            for (NodeId successor : successors)
            {
                const Veng::usize si = liveIndexOf(successor);
                if (si < m_Live.size() && indegree[si] > 0)
                    --indegree[si];
            }
        }

        return order;
    }

    const NodeGraph::Node* NodeGraph::Lookup(NodeId node) const
    {
        if (node.Index >= m_Nodes.size())
            return nullptr;
        const Node& slot = m_Nodes[node.Index];
        if (!slot.Alive || slot.Generation != node.Generation)
            return nullptr;
        return &slot;
    }

    NodePinShape NodeGraph::ShapeOf(NodeId node) const
    {
        const Node* found = Lookup(node);
        VE_ASSERT(found != nullptr, "ShapeOf: stale or non-existent node");
        return m_PinShape(found->Type);
    }

    bool NodeGraph::Reaches(NodeId origin, NodeId target) const
    {
        if (origin == target)
            return true;

        Veng::vector<NodeId> stack;
        stack.push_back(origin);
        Veng::vector<NodeId> seen;
        seen.push_back(origin);

        while (!stack.empty())
        {
            const NodeId current = stack.back();
            stack.pop_back();

            for (const Link& link : m_Links)
            {
                if (!(link.From.Node == current))
                    continue;
                const NodeId next = link.To.Node;
                if (next == target)
                    return true;
                if (std::find(seen.begin(), seen.end(), next) == seen.end())
                {
                    seen.push_back(next);
                    stack.push_back(next);
                }
            }
        }

        return false;
    }
}
