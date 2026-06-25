#include "EnvironmentImporter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>
#include <glm/gtc/packing.hpp>
#include <stb_image_resize2.h>
#include <tinyexr.h>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng::Cook
{
    Result<vector<u8>> EnvironmentImporter::Cook(const CookContext& context,
                                                 const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("environment importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const std::ifstream sourceFile(sourcePath, std::ios::binary);
        if (!sourceFile)
        {
            return std::unexpected(
                fmt::format("environment importer: failed to open '{}'", sourcePath.string()));
        }

        std::ostringstream contentStream;
        contentStream << sourceFile.rdbuf();
        const json envJson = json::parse(contentStream.str(), nullptr, false);
        if (envJson.is_discarded() || !envJson.is_object())
        {
            return std::unexpected(
                fmt::format("environment importer: '{}': invalid JSON", sourcePath.string()));
        }

        if (!envJson.contains("image") || !envJson["image"].is_string())
        {
            return std::unexpected(fmt::format(
                "environment importer: '{}': missing or invalid 'image'", sourcePath.string()));
        }

        const path imagePath = sourcePath.parent_path() / envJson["image"].get<string>();
        context.RecordDependency(imagePath);

        // Decode the equirectangular HDR panorama as 4-channel float; tinyexr allocates the
        // RGBA buffer, freed below.
        float* pixels = nullptr;
        int width = 0;
        int height = 0;
        const char* exrError = nullptr;
        if (LoadEXR(&pixels, &width, &height, imagePath.string().c_str(), &exrError) !=
            TINYEXR_SUCCESS)
        {
            const string message =
                exrError != nullptr ? string(exrError) : string("unknown tinyexr error");
            FreeEXRErrorMessage(exrError);
            return std::unexpected(
                fmt::format("environment importer: '{}': failed to load '{}': {}",
                            sourcePath.string(), imagePath.string(), message));
        }

        // Optional downscale: when the larger edge exceeds "max_size", shrink the panorama
        // (aspect-preserving, linear) before packing so a high-resolution HDRI does not bloat
        // the cooked blob.
        u32 maxSize = 0;
        if (envJson.contains("max_size") && envJson["max_size"].is_number_unsigned())
        {
            maxSize = envJson["max_size"].get<u32>();
        }

        int targetWidth = width;
        int targetHeight = height;
        if (maxSize > 0 &&
            (static_cast<u32>(width) > maxSize || static_cast<u32>(height) > maxSize))
        {
            const f32 scale = static_cast<f32>(maxSize) / static_cast<f32>(std::max(width, height));
            targetWidth = std::max(1, static_cast<int>(static_cast<f32>(width) * scale));
            targetHeight = std::max(1, static_cast<int>(static_cast<f32>(height) * scale));
        }

        const usize texelCount = static_cast<usize>(targetWidth) * static_cast<usize>(targetHeight);
        vector<f32> floatPixels(texelCount * 4);

        if (targetWidth != width || targetHeight != height)
        {
            const f32* resized =
                stbir_resize_float_linear(pixels, width, height, 0, floatPixels.data(), targetWidth,
                                          targetHeight, 0, STBIR_RGBA);
            if (resized == nullptr)
            {
                std::free(pixels);
                return std::unexpected(
                    fmt::format("environment importer: '{}': failed to resize '{}'",
                                sourcePath.string(), imagePath.string()));
            }
        }
        else
        {
            std::memcpy(floatPixels.data(), pixels, texelCount * 4 * sizeof(f32));
        }
        std::free(pixels);

        // Pack to RGBA16Sfloat: four half-floats per texel. Half halves the blob and matches the
        // HDR image format the engine creates the panorama texture in.
        const usize halfBytes = texelCount * 4 * sizeof(u16);
        vector<u8> halfPixels(halfBytes);
        auto* halfData = reinterpret_cast<u16*>(halfPixels.data());
        for (usize i = 0; i < texelCount * 4; ++i)
        {
            halfData[i] = glm::packHalf1x16(floatPixels[i]);
        }

        CookedEnvironmentHeader header{};
        header.Version = CookedEnvironmentVersion;
        header.Format = 6u; // RGBA16Sfloat (Renderer::Format ordinal)
        header.Width = static_cast<u32>(targetWidth);
        header.Height = static_cast<u32>(targetHeight);

        vector<u8> blob(sizeof(CookedEnvironmentHeader) + halfBytes);
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), halfPixels.data(), halfBytes);

        return blob;
    }
}
