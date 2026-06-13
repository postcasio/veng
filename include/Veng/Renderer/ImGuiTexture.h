#pragma once
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    class ImGuiTexture
    {
    public:
        ~ImGuiTexture();

        [[nodiscard]] u64 GetTextureId() const;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        explicit ImGuiTexture(Unique<Native> native);

        Unique<Native> m_Native;

        friend class Context;
    };
}
