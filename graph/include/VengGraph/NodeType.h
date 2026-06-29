#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <VengGraph/NodeGraph.h>

#include <span>

namespace VengGraph
{
    /// @brief Descriptor for one pin on a node type.
    ///
    /// The name is the stable identity links serialize against (by name, not index)
    /// for forward tolerance. PinType reuses the topology core's builtin-leaf TypeId space.
    struct PinDesc
    {
        /// @brief Stable display and serialization name for this pin.
        Veng::string Name;
        /// @brief Data type of this pin.
        PinType Type;
    };

    /// @brief Descriptor for a node type: pins plus a reflected property struct.
    ///
    /// A node type is data, not a subclass. The property struct is described by
    /// hand-authored FieldDescriptors over a POD the node instance stores as opaque
    /// bytes — walked by the reflection serializer and drawn by the inspector's
    /// per-FieldClass widgets. Properties are restricted to builtin leaf FieldClasses
    /// (Scalar/Vector/Quaternion/Enum/AssetHandle), so a property walk never requires
    /// a TypeRegistry lookup.
    struct NodeType
    {
        /// @brief Editor-local id minted by NodeCatalog::Register.
        NodeTypeId Id;
        /// @brief Catalog display name; also the stable serialized key.
        Veng::string Name;
        /// @brief Ordered input pin descriptors.
        Veng::vector<PinDesc> Inputs;
        /// @brief Ordered output pin descriptors.
        Veng::vector<PinDesc> Outputs;
        /// @brief FieldDescriptors over the node's property POD.
        Veng::vector<Veng::FieldDescriptor> Properties;
        /// @brief sizeof the property struct; the graph allocates this many bytes per node.
        Veng::usize PropertySize = 0;
    };

    /// @brief Owns and indexes node type descriptors; mints their editor-local NodeTypeIds.
    ///
    /// Supplies NodeGraph's PinShapeFn and the node-instance property layout.
    /// @pre The catalog must outlive any graph that references its types.
    class NodeCatalog
    {
    public:
        /// @brief Mints a fresh NodeTypeId, stores the descriptor, and returns the id.
        ///
        /// The descriptor's incoming Id field is overwritten with the minted value.
        /// @param type Node type descriptor; Id is ignored on input.
        /// @return The minted NodeTypeId.
        NodeTypeId Register(NodeType type);

        /// @brief Finds a type by its editor-local id, or nullptr when not found.
        [[nodiscard]] const NodeType* Find(NodeTypeId id) const;

        /// @brief Finds a type by its stable catalog name, or nullptr when not found.
        ///
        /// Used by deserialization so ids survive a catalog reorder.
        [[nodiscard]] const NodeType* Find(Veng::string_view name) const;

        /// @brief Returns every registered type in registration order.
        [[nodiscard]] std::span<const NodeType> Types() const;

        /// @brief Returns the pin shape for a type, derived from its pin descriptors.
        ///
        /// Bind this as the NodeGraph's PinShapeFn.
        [[nodiscard]] NodePinShape ShapeOf(NodeTypeId id) const;

    private:
        Veng::vector<NodeType> m_Types;
    };
}
