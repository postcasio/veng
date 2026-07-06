#include <Veng/Gui/UIDocument.h>

namespace Veng::Gui
{
    Ref<UIDocument> UIDocument::Create(vector<UIElementRecipe> elements,
                                       vector<AssetHandle<StyleSheet>> styleSheets,
                                       vector<Ref<Detail::AssetCacheEntry>> dependencies)
    {
        return Ref<UIDocument>(
            new UIDocument(std::move(elements), std::move(styleSheets), std::move(dependencies)));
    }

    UIDocument::UIDocument(vector<UIElementRecipe> elements,
                           vector<AssetHandle<StyleSheet>> styleSheets,
                           vector<Ref<Detail::AssetCacheEntry>> dependencies)
        : m_Elements(std::move(elements)), m_StyleSheets(std::move(styleSheets)),
          m_Dependencies(std::move(dependencies))
    {
    }
}
