#include "YogaTree.h"

namespace Veng::Gui
{
    Document::YogaTree::YogaTree()
    {
        Config = YGConfigNew();
        // Snap layout to whole framebuffer pixels so quads and text land on integer edges.
        YGConfigSetPointScaleFactor(Config, 1.0f);
        // Match the web flexbox defaults (row main axis, shrink 1) the style vocabulary maps onto.
        YGConfigSetErrata(Config, YGErrataNone);
        YGConfigSetUseWebDefaults(Config, false);
    }

    Document::YogaTree::~YogaTree()
    {
        for (const auto& [element, node] : Nodes)
        {
            YGNodeFree(node);
        }
        Nodes.clear();
        if (Config != nullptr)
        {
            YGConfigFree(Config);
            Config = nullptr;
        }
    }

    YGNodeRef Document::YogaTree::Create(Element& element)
    {
        const YGNodeRef node = YGNodeNewWithConfig(Config);
        YGNodeSetContext(node, &element);
        Nodes.emplace(&element, node);
        return node;
    }

    void Document::YogaTree::Destroy(Element& element)
    {
        const auto it = Nodes.find(&element);
        if (it == Nodes.end())
        {
            return;
        }
        YGNodeFree(it->second);
        Nodes.erase(it);
    }

    YGNodeRef Document::YogaTree::Get(const Element& element) const
    {
        const auto it = Nodes.find(&element);
        return it == Nodes.end() ? nullptr : it->second;
    }
}
