#include "NormalVariance.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <stb_image_resize2.h>

#include <Veng/Asset/NormalVariance.h>

namespace Veng::Cook
{
    namespace
    {
        // Writes the unit normal an unsigned-encoded X/Y pair decodes to, with zero spread.
        void StoreUnitNormal(f32* out, f32 encodedX, f32 encodedY)
        {
            const f32 x = (encodedX * 2.0f) - 1.0f;
            const f32 y = (encodedY * 2.0f) - 1.0f;
            const f32 z = std::sqrt(std::max(0.0f, 1.0f - (x * x) - (y * y)));
            // X and Y past the unit circle leave Z at zero and the vector longer than one.
            const f32 length = std::sqrt((x * x) + (y * y) + (z * z));
            out[0] = x / length;
            out[1] = y / length;
            out[2] = z / length;
            out[3] = 0.0f;
        }
    }

    NormalMoments NormalMomentsOfRgba8(const std::span<const u8> rgba, const u32 width,
                                       const u32 height)
    {
        NormalMoments moments{.Width = width, .Height = height};
        const usize texels = static_cast<usize>(width) * height;
        moments.Texels.resize(texels * 4);
        constexpr f32 Inv255 = 1.0f / 255.0f;
        for (usize i = 0; i < texels; ++i)
        {
            StoreUnitNormal(moments.Texels.data() + (i * 4), static_cast<f32>(rgba[i * 4]) * Inv255,
                            static_cast<f32>(rgba[(i * 4) + 1]) * Inv255);
        }
        return moments;
    }

    NormalMoments NormalMomentsOfFloat(const std::span<const f32> rgba, const u32 width,
                                       const u32 height)
    {
        NormalMoments moments{.Width = width, .Height = height};
        const usize texels = static_cast<usize>(width) * height;
        moments.Texels.resize(texels * 4);
        for (usize i = 0; i < texels; ++i)
        {
            StoreUnitNormal(moments.Texels.data() + (i * 4), rgba[i * 4], rgba[(i * 4) + 1]);
        }
        return moments;
    }

    NormalMoments ResampleNormalMoments(const NormalMoments& source, const u32 width,
                                        const u32 height)
    {
        // The second moment E|n|² = spread + |mean|² is what filters linearly; the spread is
        // recovered from it after the resample.
        const usize sourceTexels = static_cast<usize>(source.Width) * source.Height;
        vector<f32> raw(source.Texels);
        for (usize i = 0; i < sourceTexels; ++i)
        {
            f32* t = raw.data() + (i * 4);
            t[3] += (t[0] * t[0]) + (t[1] * t[1]) + (t[2] * t[2]);
        }

        NormalMoments out{.Width = width, .Height = height};
        out.Texels.resize(static_cast<usize>(width) * height * 4);
        // STBIR_4CHANNEL, not STBIR_RGBA: the fourth channel is data, not an alpha to premultiply by.
        stbir_resize_float_linear(raw.data(), static_cast<int>(source.Width),
                                  static_cast<int>(source.Height), 0, out.Texels.data(),
                                  static_cast<int>(width), static_cast<int>(height), 0,
                                  STBIR_4CHANNEL);

        const usize texels = static_cast<usize>(width) * height;
        for (usize i = 0; i < texels; ++i)
        {
            f32* t = out.Texels.data() + (i * 4);
            const f32 meanSquared = (t[0] * t[0]) + (t[1] * t[1]) + (t[2] * t[2]);
            t[3] = std::max(0.0f, t[3] - meanSquared);
        }
        return out;
    }

    NormalMoments HalveNormalMoments(const NormalMoments& level, const u32 outWidth,
                                     const u32 outHeight)
    {
        NormalMoments out{.Width = outWidth, .Height = outHeight};
        out.Texels.resize(static_cast<usize>(outWidth) * outHeight * 4);
        for (u32 y = 0; y < outHeight; ++y)
        {
            const u32 y0 = std::min(y * 2, level.Height - 1);
            const u32 y1 = std::min((y * 2) + 1, level.Height - 1);
            for (u32 x = 0; x < outWidth; ++x)
            {
                const u32 x0 = std::min(x * 2, level.Width - 1);
                const u32 x1 = std::min((x * 2) + 1, level.Width - 1);
                const std::array<const f32*, 4> parents{
                    level.Texels.data() + ((static_cast<usize>(y0) * level.Width + x0) * 4),
                    level.Texels.data() + ((static_cast<usize>(y0) * level.Width + x1) * 4),
                    level.Texels.data() + ((static_cast<usize>(y1) * level.Width + x0) * 4),
                    level.Texels.data() + ((static_cast<usize>(y1) * level.Width + x1) * 4),
                };

                // The mean as the first parent plus the mean offset of the rest, so four equal
                // parents reproduce the first exactly and leave no rounding to read as spread.
                f32* t = out.Texels.data() + ((static_cast<usize>(y) * outWidth + x) * 4);
                for (u32 c = 0; c < 3; ++c)
                {
                    const f32 first = parents[0][c];
                    t[c] = first + (0.25f * ((parents[1][c] - first) + (parents[2][c] - first) +
                                             (parents[3][c] - first)));
                }

                f32 spread = 0.0f;
                for (const f32* parent : parents)
                {
                    const f32 dx = parent[0] - t[0];
                    const f32 dy = parent[1] - t[1];
                    const f32 dz = parent[2] - t[2];
                    spread += parent[3] + (dx * dx) + (dy * dy) + (dz * dz);
                }
                t[3] = 0.25f * spread;
            }
        }
        return out;
    }

    vector<vector<f32>> NormalVarianceChain(NormalMoments base, const u32 mipCount)
    {
        vector<vector<f32>> chain;
        chain.reserve(mipCount);
        NormalMoments level = std::move(base);
        for (u32 index = 0; index < mipCount; ++index)
        {
            if (index > 0)
            {
                level = HalveNormalMoments(level, std::max(1u, level.Width >> 1),
                                           std::max(1u, level.Height >> 1));
            }
            const usize texels = static_cast<usize>(level.Width) * level.Height;
            vector<f32> variance(texels);
            for (usize i = 0; i < texels; ++i)
            {
                variance[i] = NormalVarianceOfSpread(level.Texels[(i * 4) + 3]);
            }
            chain.push_back(std::move(variance));
        }
        return chain;
    }
}
