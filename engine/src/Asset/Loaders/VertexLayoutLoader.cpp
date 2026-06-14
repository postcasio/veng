#include "VertexLayoutLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng
{
    namespace
    {
        // The Bridge* helpers below map the cooked blob's underlying-integer
        // enum fields to their Veng::Renderer enums — the engine side of the
        // cycle-avoidance rule documented in assetformat's CookedBlobs.h. An
        // unrecognized value means a stale/corrupt cooked archive, hence
        // AssetError::Corrupt (recoverable) rather than VE_ASSERT.

        // Only the plain float/vecN formats VertexBufferElement supports
        // (VertexBufferLayout.cpp's GetFormatSize/GetFormatComponentCount) are
        // valid vertex-input formats.
        optional<Renderer::Format> BridgeVertexInputFormat(u32 value)
        {
            switch (value)
            {
                case static_cast<u32>(Renderer::Format::R32Sfloat): return Renderer::Format::R32Sfloat;
                case static_cast<u32>(Renderer::Format::RG32Sfloat): return Renderer::Format::RG32Sfloat;
                case static_cast<u32>(Renderer::Format::RGB32Sfloat): return Renderer::Format::RGB32Sfloat;
                case static_cast<u32>(Renderer::Format::RGBA32Sfloat): return Renderer::Format::RGBA32Sfloat;
                default: return std::nullopt;
            }
        }

        // Cooked names are fixed-size, nul-terminated char arrays (CookedBlobs.h).
        template <usize N>
        string BridgeName(const char (&name)[N])
        {
            return string(name, strnlen(name, N));
        }

        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{.Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::RefAny> VertexLayoutLoader::Load(
        AssetManager& /*manager*/, Renderer::Context& /*context*/,
        AssetId id, std::span<const u8> cooked) const
    {
        if (cooked.size() < sizeof(CookedVertexLayoutHeader))
            return std::unexpected(Corrupt(id, "vertex_layout: cooked blob smaller than CookedVertexLayoutHeader"));

        CookedVertexLayoutHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        usize cursor = sizeof(CookedVertexLayoutHeader);

        const usize elementBytes = static_cast<usize>(header.ElementCount) * sizeof(CookedVertexLayoutElement);
        if (cooked.size() < cursor + elementBytes)
            return std::unexpected(Corrupt(id, "vertex_layout: cooked blob smaller than element table"));

        vector<Renderer::VertexBufferElement> elements;
        elements.reserve(header.ElementCount);

        for (u32 i = 0; i < header.ElementCount; ++i)
        {
            CookedVertexLayoutElement element;
            std::memcpy(&element, cooked.data() + cursor + i * sizeof(CookedVertexLayoutElement), sizeof(element));

            const optional<Renderer::Format> format = BridgeVertexInputFormat(element.Format);
            if (!format)
            {
                return std::unexpected(Corrupt(id, fmt::format(
                    "vertex_layout: element {} has unrecognized format {}", i, element.Format)));
            }

            elements.emplace_back(*format, BridgeName(element.Name));
        }

        const Ref<Renderer::VertexLayoutAsset> asset = CreateRef<Renderer::VertexLayoutAsset>(Renderer::VertexLayoutAsset{
            .Layout = Renderer::VertexBufferLayout(elements),
        });

        return Detail::RefAny(asset);
    }
}
