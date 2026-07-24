#include <Veng/Net/Quantize.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace Veng::Net
{
    namespace
    {
        // The smallest-three encoding maps a component in [-1/sqrt2, +1/sqrt2] (the widest a
        // non-largest unit-quaternion component can be) onto the integer grid.
        constexpr f32 SmallestThreeRange = 0.70710678f; // 1/sqrt(2)

        // The level count for a field width. Computed in u64 because a 32-bit field's shift
        // overflows a u32 before the subtraction that would have brought it back in range.
        u32 LevelsForBits(u32 bits)
        {
            return static_cast<u32>((1ull << bits) - 1ull);
        }

        // The fixed-point mapping runs in f64, so the achieved grid is the declared quantum at any
        // extent. An f32 intermediate represents integers exactly only to 2^24, so a field wider
        // than 24 bits would round the code to a multiple of the f32 ULP at that magnitude —
        // coarsening the grid by that factor while every declared setting still reads correct.
        u32 QuantizeUnitSigned(f32 value, f32 range, u32 bits)
        {
            const u32 levels = LevelsForBits(bits);
            const f64 span = 2.0 * static_cast<f64>(range);
            const f64 normalized =
                std::clamp((static_cast<f64>(value) + static_cast<f64>(range)) / span, 0.0, 1.0);
            return static_cast<u32>(std::llround(normalized * static_cast<f64>(levels)));
        }

        f32 DequantizeUnitSigned(u32 code, f32 range, u32 bits)
        {
            const u32 levels = LevelsForBits(bits);
            const f64 normalized = static_cast<f64>(code) / static_cast<f64>(levels);
            return static_cast<f32>(normalized * 2.0 * static_cast<f64>(range) -
                                    static_cast<f64>(range));
        }
    }

    u32 PositionAxisBits(const QuantizationSettings& settings)
    {
        const f32 quantum = settings.PositionQuantum > 0.0f ? settings.PositionQuantum : 0.001f;
        const f32 extent = settings.PositionExtent > 0.0f ? settings.PositionExtent : 1.0f;
        const f64 levels = std::ceil((2.0 * static_cast<f64>(extent)) / static_cast<f64>(quantum));
        u32 bits = 1;
        while (bits < 32 && (static_cast<f64>((1ull << bits) - 1ull)) < levels)
        {
            ++bits;
        }
        return bits;
    }

    void EncodePosition(BitWriter& out, const vec3& position, const QuantizationSettings& settings)
    {
        const u32 bits = PositionAxisBits(settings);
        const f32 extent = settings.PositionExtent > 0.0f ? settings.PositionExtent : 1.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            out.WriteBits(QuantizeUnitSigned(position[axis], extent, bits), bits);
        }
    }

    vec3 DecodePosition(BitReader& in, const QuantizationSettings& settings)
    {
        const u32 bits = PositionAxisBits(settings);
        const f32 extent = settings.PositionExtent > 0.0f ? settings.PositionExtent : 1.0f;
        vec3 out{0.0f};
        for (int axis = 0; axis < 3; ++axis)
        {
            out[axis] = DequantizeUnitSigned(in.ReadBits(bits), extent, bits);
        }
        return out;
    }

    void EncodeRotation(BitWriter& out, const quat& rotation, const QuantizationSettings& settings)
    {
        const quat q = glm::normalize(rotation);

        // Index of the largest-magnitude component; it is the one dropped and reconstructed.
        const f32 comps[4] = {q.x, q.y, q.z, q.w};
        int largest = 0;
        for (int i = 1; i < 4; ++i)
        {
            if (std::abs(comps[i]) > std::abs(comps[largest]))
            {
                largest = i;
            }
        }

        // Canonicalize the sign so the dropped component is non-negative (q and -q are the same
        // rotation), so the decoder takes the positive root with no sign bit.
        const f32 sign = comps[largest] < 0.0f ? -1.0f : 1.0f;

        out.WriteBits(static_cast<u32>(largest), 2);
        for (int i = 0; i < 4; ++i)
        {
            if (i == largest)
            {
                continue;
            }
            out.WriteBits(
                QuantizeUnitSigned(sign * comps[i], SmallestThreeRange, settings.RotationBits),
                settings.RotationBits);
        }
    }

    quat DecodeRotation(BitReader& in, const QuantizationSettings& settings)
    {
        const int largest = static_cast<int>(in.ReadBits(2));
        f32 comps[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        f32 sumSquares = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            if (i == largest)
            {
                continue;
            }
            const f32 value = DequantizeUnitSigned(in.ReadBits(settings.RotationBits),
                                                   SmallestThreeRange, settings.RotationBits);
            comps[i] = value;
            sumSquares += value * value;
        }
        comps[largest] = std::sqrt(std::max(0.0f, 1.0f - sumSquares));

        return glm::normalize(quat(comps[3], comps[0], comps[1], comps[2])); // (w, x, y, z)
    }
}
