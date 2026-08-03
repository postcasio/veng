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

        // Bytes the header occupies: the magic, the version, the shape count, and seven u32 fields
        // per shape.
        constexpr usize HeaderBytes(const usize shapeCount)
        {
            return BlobMagic.size() + (2 * sizeof(u32)) + (shapeCount * 7 * sizeof(u32));
        }

        static_assert(HeaderBytes(MaxShapes) == MaxGeneratedTextureBlobHeaderBytes,
                      "the published header bound must be the largest header this codec writes");

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

    vector<u8> BeginGeneratedTextureBlob(const vector<GeneratedTextureBlobShape>& shapes,
                                         const usize texelBytes)
    {
        usize expected = 0;
        for (const GeneratedTextureBlobShape& shape : shapes)
        {
            const usize bytes = GeneratedTextureShapeBytes(shape);
            if (bytes == 0)
            {
                return {};
            }
            expected += bytes;
        }
        if (shapes.empty() || shapes.size() > MaxShapes || expected != texelBytes)
        {
            return {};
        }

        vector<u8> out;
        // The texels are appended onto this buffer, so reserving for them here is what keeps the
        // payload one allocation and the texels one copy.
        out.reserve(texelBytes + HeaderBytes(shapes.size()));
        out.insert(out.end(), BlobMagic.begin(), BlobMagic.end());
        Put<u32>(out, FormatVersion);
        Put<u32>(out, static_cast<u32>(shapes.size()));
        for (const GeneratedTextureBlobShape& shape : shapes)
        {
            Put<u32>(out, static_cast<u32>(shape.TexelFormat));
            Put<u32>(out, static_cast<u32>(shape.Type));
            Put<u32>(out, shape.Extent.x);
            Put<u32>(out, shape.Extent.y);
            Put<u32>(out, shape.Extent.z);
            Put<u32>(out, shape.Layers);
            Put<u32>(out, shape.MipLevels);
        }
        return out;
    }

    vector<u8> EncodeGeneratedTextureBlob(const GeneratedTextureBlob& blob)
    {
        vector<u8> out = BeginGeneratedTextureBlob(blob.Shapes, blob.Texels.size());
        if (out.empty())
        {
            return {};
        }
        out.insert(out.end(), blob.Texels.begin(), blob.Texels.end());
        return out;
    }

    optional<GeneratedTextureBlobLayout>
    ReadGeneratedTextureBlobPrefix(const std::span<const u8> prefix)
    {
        Reader reader{.Bytes = prefix};
        u32 version = 0;
        u32 shapeCount = 0;
        if (!reader.TakeMagic(BlobMagic) || !reader.Take(version) || version != FormatVersion ||
            !reader.Take(shapeCount) || shapeCount == 0 || shapeCount > MaxShapes)
        {
            return std::nullopt;
        }

        GeneratedTextureBlobLayout layout;
        layout.Shapes.reserve(shapeCount);
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
            layout.Shapes.push_back(shape);
        }

        layout.TexelOffset = reader.Offset;
        layout.TexelBytes = expected;
        return layout;
    }

    optional<GeneratedTextureBlobLayout>
    ReadGeneratedTextureBlobHeader(const std::span<const u8> payload)
    {
        const optional<GeneratedTextureBlobLayout> layout = ReadGeneratedTextureBlobPrefix(payload);
        // The one check a prefix cannot make: the texels behind the header are exactly as many as
        // the shapes describe, so a truncated or padded payload is not decoded into the wrong texels.
        if (!layout.has_value() || payload.size() - layout->TexelOffset != layout->TexelBytes)
        {
            return std::nullopt;
        }
        return layout;
    }

    optional<GeneratedTextureBlob> DecodeGeneratedTextureBlob(const std::span<const u8> payload)
    {
        const optional<GeneratedTextureBlobLayout> layout = ReadGeneratedTextureBlobHeader(payload);
        if (!layout.has_value())
        {
            return std::nullopt;
        }
        return GeneratedTextureBlob{
            .Shapes = layout->Shapes,
            .Texels = {payload.begin() + static_cast<isize>(layout->TexelOffset), payload.end()},
        };
    }
}
