#include "TextureImporter.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>
#include <stb_image.h>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng::Cook
{
    namespace
    {
        // The Parse* helpers below mirror Veng::Renderer::Filter / MipmapMode /
        // AddressMode ordinals (Renderer/Types.h) — kept in sync by hand per the
        // cycle-avoidance rule documented in assetpack's CookedBlobs.h.

        optional<u32> ParseFilter(const string& name)
        {
            if (name == "nearest")
                return 0u;
            if (name == "linear")
                return 1u;
            return std::nullopt;
        }

        optional<u32> ParseMipmapMode(const string& name)
        {
            if (name == "nearest")
                return 0u;
            if (name == "linear")
                return 1u;
            return std::nullopt;
        }

        optional<u32> ParseAddressMode(const string& name)
        {
            if (name == "repeat")
                return 0u;
            if (name == "mirrored_repeat")
                return 1u;
            if (name == "clamp_to_edge")
                return 2u;
            if (name == "clamp_to_border")
                return 3u;
            return std::nullopt;
        }
    }

    Result<vector<u8>> TextureImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
            return std::unexpected("texture importer: missing or invalid 'source'");

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        std::ifstream sourceFile(sourcePath, std::ios::binary);
        if (!sourceFile)
            return std::unexpected(fmt::format("texture importer: failed to open '{}'", sourcePath.string()));

        std::ostringstream contentStream;
        contentStream << sourceFile.rdbuf();
        const json texJson = json::parse(contentStream.str(), nullptr, false);
        if (texJson.is_discarded() || !texJson.is_object())
            return std::unexpected(fmt::format("texture importer: '{}': invalid JSON", sourcePath.string()));

        if (!texJson.contains("image") || !texJson["image"].is_string())
        {
            return std::unexpected(fmt::format(
                "texture importer: '{}': missing or invalid 'image'", sourcePath.string()));
        }

        if (texJson.contains("generate_mips") && texJson["generate_mips"].is_boolean() &&
            texJson["generate_mips"].get<bool>())
        {
            return std::unexpected(fmt::format(
                "texture importer: '{}': 'generate_mips' is not supported (single-mip output only)",
                sourcePath.string()));
        }

        const bool srgb = texJson.contains("srgb") && texJson["srgb"].is_boolean() && texJson["srgb"].get<bool>();

        const path imagePath = sourcePath.parent_path() / texJson["image"].get<string>();

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(imagePath.string().c_str(), &width, &height, &channels, 4);
        if (!pixels)
        {
            return std::unexpected(fmt::format("texture importer: '{}': failed to load image '{}': {}",
                sourcePath.string(), imagePath.string(), stbi_failure_reason()));
        }

        const usize pixelBytes = static_cast<usize>(width) * static_cast<usize>(height) * 4;
        vector<u8> pixelData(pixelBytes);
        std::memcpy(pixelData.data(), pixels, pixelBytes);
        stbi_image_free(pixels);

        CookedTextureHeader header{};
        header.Format = srgb ? 3u /* RGBA8Srgb */ : 2u /* RGBA8Unorm */;
        header.Width = static_cast<u32>(width);
        header.Height = static_cast<u32>(height);
        header.MipCount = 1;

        // Sampler defaults mirror Veng::Renderer::SamplerInfo's defaults
        // (Renderer/Sampler.h).
        header.MinFilter = 1;       // Linear
        header.MagFilter = 1;       // Linear
        header.MipmapMode = 1;      // Linear
        header.AddressModeU = 0;    // Repeat
        header.AddressModeV = 0;    // Repeat
        header.AddressModeW = 0;    // Repeat
        header.AnisotropyEnabled = 1;
        header.MaxAnisotropy = 8.0f;

        if (texJson.contains("sampler") && texJson["sampler"].is_object())
        {
            const json& sampler = texJson["sampler"];

            // Reads a string field through `parser`, returning `fallback` if the
            // field is absent.
            auto field = [&](const char* key, auto parser, u32 fallback) -> Result<u32>
            {
                if (!sampler.contains(key) || !sampler[key].is_string())
                    return fallback;

                const string value = sampler[key].get<string>();
                const optional<u32> parsed = parser(value);
                if (!parsed)
                {
                    return std::unexpected(fmt::format(
                        "texture importer: '{}': invalid sampler.{} '{}'", sourcePath.string(), key, value));
                }

                return *parsed;
            };

            const Result<u32> minFilter = field("min", ParseFilter, header.MinFilter);
            if (!minFilter)
                return std::unexpected(minFilter.error());
            header.MinFilter = *minFilter;

            const Result<u32> magFilter = field("mag", ParseFilter, header.MagFilter);
            if (!magFilter)
                return std::unexpected(magFilter.error());
            header.MagFilter = *magFilter;

            const Result<u32> mipmapMode = field("mipmap", ParseMipmapMode, header.MipmapMode);
            if (!mipmapMode)
                return std::unexpected(mipmapMode.error());
            header.MipmapMode = *mipmapMode;

            const Result<u32> addressModeU = field("wrap_u", ParseAddressMode, header.AddressModeU);
            if (!addressModeU)
                return std::unexpected(addressModeU.error());
            header.AddressModeU = *addressModeU;

            const Result<u32> addressModeV = field("wrap_v", ParseAddressMode, header.AddressModeV);
            if (!addressModeV)
                return std::unexpected(addressModeV.error());
            header.AddressModeV = *addressModeV;

            const Result<u32> addressModeW = field("wrap_w", ParseAddressMode, header.AddressModeW);
            if (!addressModeW)
                return std::unexpected(addressModeW.error());
            header.AddressModeW = *addressModeW;

            if (sampler.contains("anisotropy") && sampler["anisotropy"].is_number())
            {
                const f32 value = sampler["anisotropy"].get<f32>();
                header.AnisotropyEnabled = value > 0.0f ? 1u : 0u;
                header.MaxAnisotropy = value > 0.0f ? value : 1.0f;
            }
        }

        vector<u8> blob(sizeof(CookedTextureHeader) + pixelBytes);
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), pixelData.data(), pixelBytes);

        return blob;
    }
}
