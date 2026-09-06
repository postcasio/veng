#include <Veng/Render/GraphicsSchema.h>

namespace Veng
{
    Ref<GraphicsSchema> GraphicsSchema::Create(GraphicsSchemaData data)
    {
        return Ref<GraphicsSchema>(new GraphicsSchema(std::move(data)));
    }

    GraphicsSchema::GraphicsSchema(GraphicsSchemaData data) : m_Data(std::move(data)) {}

    const GraphicsCategory* GraphicsSchema::FindCategory(const std::string_view id) const
    {
        for (const GraphicsCategory& category : m_Data.Categories)
        {
            if (category.Id == id)
            {
                return &category;
            }
        }
        return nullptr;
    }

    const GraphicsSetting* GraphicsSchema::FindSetting(const std::string_view id) const
    {
        for (const GraphicsCategory& category : m_Data.Categories)
        {
            for (const GraphicsSetting& setting : category.Settings)
            {
                if (setting.Id == id)
                {
                    return &setting;
                }
            }
        }
        return nullptr;
    }

    const GraphicsPreset* GraphicsSchema::FindPreset(const std::string_view id) const
    {
        for (const GraphicsPreset& preset : m_Data.Presets)
        {
            if (preset.Id == id)
            {
                return &preset;
            }
        }
        return nullptr;
    }
}
