#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <VengEditor/NodeGraph/NodeGraph.h>

#include <span>

namespace VengEditor
{
    // One pin on a node type: its display/serialization name and its data type.
    // The name is the stable identity links serialize against (pins by name, not
    // index, for forward tolerance); the PinType reuses the topology core's
    // builtin-leaf TypeId space.
    struct PinDesc
    {
        Veng::string Name;
        PinType Type;
    };

    // A node type is data, not a subclass: pins plus a reflected property struct.
    // The property struct is described by hand-authored FieldDescriptors over a
    // POD the node instance stores as opaque bytes — walked by the reflection
    // serializer and drawn by the inspector's per-FieldClass widgets, exactly the
    // ECS-component machinery, reused. Node properties are restricted to builtin
    // leaf FieldClasses (Scalar/Vector/Quaternion/Enum/AssetHandle); a node-local
    // nested struct is out of scope, so a property walk never needs a
    // TypeRegistry::Info lookup for an unregistered type.
    struct NodeType
    {
        NodeTypeId Id;
        Veng::string Name; // catalog display name; the serialized stable key
        Veng::vector<PinDesc> Inputs;
        Veng::vector<PinDesc> Outputs;
        Veng::vector<Veng::FieldDescriptor> Properties;
        Veng::usize PropertySize = 0; // sizeof the property struct
    };

    // The catalog a domain (the material specialization) fills. It owns the type
    // descriptors, mints their editor-local NodeTypeIds, and supplies NodeGraph's
    // PinShapeFn / the node-instance property layout. The catalog must outlive any
    // graph that references its types.
    class NodeCatalog
    {
    public:
        // Mints a fresh NodeTypeId, stores the descriptor, and returns the id. The
        // descriptor's incoming Id is overwritten with the minted value.
        NodeTypeId Register(NodeType type);

        [[nodiscard]] const NodeType* Find(NodeTypeId id) const;

        // Resolves a type by its stable catalog name — the form serialization
        // uses, so ids survive a catalog reorder. nullptr when no type matches.
        [[nodiscard]] const NodeType* Find(Veng::string_view name) const;

        // Every registered type, in registration order — the "add node" menu.
        [[nodiscard]] std::span<const NodeType> Types() const;

        // The pin shape of a type, derived from its pin descs — bind this as the
        // NodeGraph's PinShapeFn.
        [[nodiscard]] NodePinShape ShapeOf(NodeTypeId id) const;

    private:
        Veng::vector<NodeType> m_Types;
    };
}
