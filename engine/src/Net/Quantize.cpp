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

        u32 QuantizeUnitSigned(f32 value, f32 range, u32 bits)
        {
            const u32 levels = (1u << bits) - 1u;
            const f32 normalized = std::clamp((value + range) / (2.0f * range), 0.0f, 1.0f);
            return static_cast<u32>(std::lround(normalized * static_cast<f32>(levels)));
        }

        f32 DequantizeUnitSigned(u32 code, f32 range, u32 bits)
        {
            const u32 levels = (1u << bits) - 1u;
            const f32 normalized = static_cast<f32>(code) / static_cast<f32>(levels);
            return normalized * 2.0f * range - range;
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
        quat q = glm::normalize(rotation);

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
