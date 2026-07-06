#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/Document.h>

#include <yoga/Yoga.h>

namespace Veng::Gui
{
    /// @brief The layout mirror: one Yoga node per element, keyed by the element's address.
    ///
    /// Owns a Yoga config and every YGNodeRef, and maps each Element to its node so the Document
    /// applies styles, runs the solve, and reads results back through element identity. A node's
    /// context is the owning Element pointer, which the measure callback reads to size a text leaf.
    struct Document::YogaTree
    {
        /// @brief Constructs the config the tree's nodes are created against.
        YogaTree();

        /// @brief Frees every node and the config.
        ~YogaTree();

        YogaTree(const YogaTree&) = delete;
        YogaTree& operator=(const YogaTree&) = delete;

        /// @brief Creates a node for an element and records the mapping.
        /// @param element  The element the node mirrors.
        /// @return The new node.
        YGNodeRef Create(Element& element);

        /// @brief Frees an element's node and drops its mapping.
        /// @param element  The element whose node to free.
        void Destroy(Element& element);

        /// @brief Returns the node mirroring an element, or nullptr when none.
        /// @param element  The element to look up.
        /// @return The mirrored node, or nullptr.
        [[nodiscard]] YGNodeRef Get(const Element& element) const;

        /// @brief The shared config every node is created against.
        YGConfigRef Config = nullptr;

        /// @brief Element address to its mirrored node.
        map<const Element*, YGNodeRef> Nodes;
    };
}
