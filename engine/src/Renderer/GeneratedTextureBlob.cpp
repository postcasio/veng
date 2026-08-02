#include "GeneratedTextureBlob.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <Veng/Renderer/FormatInfo.h>

namespace Veng::Renderer
{
    namespace
    {
        // The payload's own identity, checked ahead of the shapes it prefixes. The cache validates
        // that a blob is intact; this validates that it is a texture blob of this layout.
        constexpr std::array<u8, 8> BlobMagic = {'V', 'N', 'G', '.', 'G', 'T', 'X', '1'};
        constexpr u32 FormatVersion = 1;

        // A shape count past this is a corrupt header, not a job: a job's targets are a handful.
        constexpr u32 MaxShapes = 64;

        template <typename T>
        void Put(vector<u8>& out, const T value)
        {
            for (usize i = 0; i < sizeof(T); i++)
            {
                out.push_back(static_cast<u8>((static_cast<u64>(value) >> (i * 8)) & 0xFFu));
            }
        }

        struct Reader
        {
            std::span<const u8> Bytes;
            usize Offset = 0;

            template <typename T>
            [[nodiscard]] bool Take(T& value)
            {
                if (Bytes.size() - Offset < sizeof(T))
                {
                    return false;
                }
                u64 raw = 0;
                for (usize i = 0; i < sizeof(T); i++)
                {
                    raw |= static_cast<u64>(Bytes[Offset + i]) << (i * 8);
                }
                Offset += sizeof(T);
                value = static_cast<T>(raw);
                return true;
            }

            [[nodiscard]] bool TakeMagic(const std::array<u8, 8>& magic)
            {
                if (Bytes.size() - Offset < magic.size())
                {
                    return false;
                }
                const bool match =
                    std::memcmp(Bytes.data() + Offset, magic.data(), magic.size()) == 0;
                Offset += magic.size();
                return match;
            }
        };
    }

    usize GeneratedTextureLayerBytes(const GeneratedTextureBlobShape& shape, const u32 mipLevel)
    {
        const u32 width = std::max(1u, shape.Extent.x >> mipLevel);
        const u32 height = std::max(1u, shape.Extent.y >> mipLevel);
        const u32 depth = std::max(1u, shape.Extent.z >> mipLevel);
        return BytesForLevel(shape.TexelFormat, width, height) * depth;
    }

    usize GeneratedTextureShapeBytes(const GeneratedTextureBlobShape& shape)
    {
        usize bytes = 0;
        for (u32 mip = 0; mip < shape.MipLevels; mip++)
        {
            bytes += GeneratedTextureLayerBytes(shape, mip) * shape.Layers;
        }
        return bytes;
    }

    usize GeneratedTextureMipOffset(const GeneratedTextureBlobShape& shape, const u32 mipLevel)
    {
        usize offset = 0;
        for (u32 mip = 0; mip < mipLevel && mip < shape.MipLevels; mip++)
        {
            offset += GeneratedTextureLayerBytes(shape, mip) * shape.Layers;
        }
        return offset;
    }

    vector<u8> EncodeGeneratedTextureBlob(const GeneratedTextureBlob& blob)
    {
        usize expected = 0;
        for (const GeneratedTextureBlobShape& shape : blob.Shapes)
        {
            const usize bytes = GeneratedTextureShapeBytes(shape);
            if (bytes == 0)
            {
                return {};
            }
            expected += bytes;
        }
        if (blob.Shapes.empty() || blob.Shapes.size() > MaxShapes || expected != blob.Texels.size())
        {
            return {};
        }

        vector<u8> out;
        out.reserve(blob.Texels.size() + 64);
        out.insert(out.end(), BlobMagic.begin(), BlobMagic.end());
        Put<u32>(out, FormatVersion);
        Put<u32>(out, static_cast<u32>(blob.Shapes.size()));
        for (const GeneratedTextureBlobShape& shape : blob.Shapes)
        {
            Put<u32>(out, static_cast<u32>(shape.TexelFormat));
            Put<u32>(out, static_cast<u32>(shape.Type));
            Put<u32>(out, shape.Extent.x);
            Put<u32>(out, shape.Extent.y);
            Put<u32>(out, shape.Extent.z);
            Put<u32>(out, shape.Layers);
            Put<u32>(out, shape.MipLevels);
        }
        out.insert(out.end(), blob.Texels.begin(), blob.Texels.end());
        return out;
    }

    optional<GeneratedTextureBlob> DecodeGeneratedTextureBlob(const std::span<const u8> payload)
    {
        Reader reader{.Bytes = payload};
        u32 version = 0;
        u32 shapeCount = 0;
        if (!reader.TakeMagic(BlobMagic) || !reader.Take(version) || version != FormatVersion ||
            !reader.Take(shapeCount) || shapeCount == 0 || shapeCount > MaxShapes)
        {
            return std::nullopt;
        }

        GeneratedTextureBlob blob;
        blob.Shapes.reserve(shapeCount);
        usize expected = 0;
        for (u32 i = 0; i < shapeCount; i++)
        {
            u32 texelFormat = 0;
            u32 type = 0;
            GeneratedTextureBlobShape shape;
            if (!reader.Take(texelFormat) || !reader.Take(type) || !reader.Take(shape.Extent.x) ||
                !reader.Take(shape.Extent.y) || !reader.Take(shape.Extent.z) ||
                !reader.Take(shape.Layers) || !reader.Take(shape.MipLevels))
            {
                return std::nullopt;
            }
            // Both enums are u8-backed, so a stored value past that range is not merely unknown —
            // casting it would not round-trip. Reject before the cast, not after.
            if (texelFormat > 0xFFu || type > 0xFFu || shape.Layers == 0 || shape.MipLevels == 0)
            {
                return std::nullopt;
            }
            shape.TexelFormat = static_cast<Format>(texelFormat);
            shape.Type = static_cast<ImageType>(type);
            const usize bytes = GeneratedTextureShapeBytes(shape);
            if (bytes == 0)
            {
                return std::nullopt;
            }
            expected += bytes;
            blob.Shapes.push_back(shape);
        }

        if (payload.size() - reader.Offset != expected)
        {
            return std::nullopt;
        }
        blob.Texels.assign(payload.data() + reader.Offset, payload.data() + payload.size());
        return blob;
    }
}
