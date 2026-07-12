// Spatial quantization codec: position fixed-point and smallest-three rotation round-trip within
// their documented error bounds, and the bit stream packs/reads sub-byte fields exactly. Pure and
// device-free — the wire-only compression of a Transform's spatial leaves, decode-side dequantized.

#include <doctest/doctest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Net/BitStream.h>
#include <Veng/Net/Quantize.h>

using namespace Veng;
using namespace Veng::Net;

TEST_CASE("BitWriter/BitReader round-trip arbitrary-width fields")
{
    BitWriter bw;
    bw.WriteBits(0b101, 3);
    bw.WriteBits(0xABCD, 16);
    bw.WriteBit(true);
    bw.WriteBits(0, 5);
    bw.WriteBits(0xFFFFFFFF, 32);

    BitReader br(bw.Bytes());
    CHECK(br.ReadBits(3) == 0b101u);
    CHECK(br.ReadBits(16) == 0xABCDu);
    CHECK(br.ReadBit() == true);
    CHECK(br.ReadBits(5) == 0u);
    CHECK(br.ReadBits(32) == 0xFFFFFFFFu);
}

TEST_CASE("BitReader past the end reads zero bits, never faults")
{
    BitWriter bw;
    bw.WriteBits(0b11, 2);
    BitReader br(bw.Bytes());
    CHECK(br.ReadBits(2) == 0b11u);
    CHECK(br.ReadBits(16) == 0u); // past the packed bits
}

TEST_CASE("Position quantizes to within half a quantum per axis")
{
    const QuantizationSettings settings{.PositionQuantum = 0.001f, .PositionExtent = 4096.0f};
    const f32 halfQuantum = settings.PositionQuantum * 0.5f;

    for (const vec3 p :
         {vec3(0.0f), vec3(1.2345f, -67.89f, 1000.5f), vec3(-4095.9f, 4095.9f, 0.25f)})
    {
        BitWriter bw;
        EncodePosition(bw, p, settings);
        BitReader br(bw.Bytes());
        const vec3 decoded = DecodePosition(br, settings);
        for (int axis = 0; axis < 3; ++axis)
        {
            CHECK(std::abs(decoded[axis] - p[axis]) <= halfQuantum + 1e-4f);
        }
    }
}

TEST_CASE("Position past the extent clamps rather than wrapping")
{
    const QuantizationSettings settings{.PositionQuantum = 0.01f, .PositionExtent = 100.0f};
    BitWriter bw;
    EncodePosition(bw, vec3(1000.0f, -1000.0f, 50.0f), settings);
    BitReader br(bw.Bytes());
    const vec3 decoded = DecodePosition(br, settings);
    CHECK(decoded.x == doctest::Approx(100.0f).epsilon(0.001f));
    CHECK(decoded.y == doctest::Approx(-100.0f).epsilon(0.001f));
    CHECK(decoded.z == doctest::Approx(50.0f).epsilon(0.02f));
}

TEST_CASE("Rotation smallest-three round-trips within its angular bound")
{
    const QuantizationSettings settings{.RotationBits = 9};

    const quat rotations[] = {
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        glm::normalize(glm::quat(0.3f, 0.5f, -0.7f, 0.2f)),
        glm::angleAxis(1.2f, glm::normalize(vec3(1.0f, 2.0f, 3.0f))),
        glm::angleAxis(-2.9f, glm::normalize(vec3(-1.0f, 0.5f, 0.2f))),
    };

    for (const quat& q : rotations)
    {
        BitWriter bw;
        EncodeRotation(bw, q, settings);
        BitReader br(bw.Bytes());
        const quat decoded = DecodeRotation(br, settings);

        // q and -q are the same rotation, so compare on |dot| (1 = identical orientation).
        const f32 alignment = std::abs(glm::dot(glm::normalize(q), decoded));
        CHECK(alignment > 0.9995f);
    }
}

TEST_CASE("PositionAxisBits scales with the quantum and extent")
{
    CHECK(PositionAxisBits(
              QuantizationSettings{.PositionQuantum = 0.001f, .PositionExtent = 4096.0f}) >= 23u);
    CHECK(PositionAxisBits(
              QuantizationSettings{.PositionQuantum = 1.0f, .PositionExtent = 100.0f}) <= 8u);
}
