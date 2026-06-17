#include <VengEditor/NodeGraph/NodeType.h>

#include <Veng/Assert.h>

namespace VengEditor
{
    NodeTypeId NodeCatalog::Register(NodeType type)
    {
        // Ids are 1-based: 0 is the default-constructed "unset" NodeTypeId.
        const NodeTypeId id{static_cast<Veng::u32>(m_Types.size()) + 1};
        type.Id = id;
        m_Types.push_back(std::move(type));
        return id;
    }

    const NodeType* NodeCatalog::Find(NodeTypeId id) const
    {
        if (id.Value == 0 || id.Value > m_Types.size())
            return nullptr;
        return &m_Types[id.Value - 1];
    }

    const NodeType* NodeCatalog::Find(Veng::string_view name) const
    {
        for (const NodeType& type : m_Types)
            if (type.Name == name)
                return &type;
        return nullptr;
    }

    std::span<const NodeType> NodeCatalog::Types() const
    {
        return std::span<const NodeType>(m_Types);
    }

    NodePinShape NodeCatalog::ShapeOf(NodeTypeId id) const
    {
        const NodeType* type = Find(id);
        VE_ASSERT(type != nullptr, "NodeCatalog::ShapeOf: unknown NodeTypeId {}", id.Value);

        NodePinShape shape;
        shape.Inputs.reserve(type->Inputs.size());
        for (const PinDesc& pin : type->Inputs)
            shape.Inputs.push_back(pin.Type);
        shape.Outputs.reserve(type->Outputs.size());
        for (const PinDesc& pin : type->Outputs)
            shape.Outputs.push_back(pin.Type);
        return shape;
    }
}
