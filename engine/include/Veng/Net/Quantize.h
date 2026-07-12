#pragma once

#include <Veng/Net/BitStream.h>
#include <Veng/Veng.h>

// Veng/Net/Quantize.h — wire-only fixed-point/smallest-three codecs for the spatial leaves.
//
// A snapshot's dominant cost is the moving pawns' Transform; f32×N is far more precision than the
// wire needs when the client only displays the pose. Position rounds to a configurable fixed-point
// grid over a bounded world span; rotation encodes as the smallest-three of the unit quaternion.
// Both are decode-side dequantized — the sim state stays full-float on both ends, and the server
// never quantizes its own truth. The only place the rounding is observable is reconciliation, whose
// spatial epsilon is documented as >= the position quantum so quantization noise never reads as a
// misprediction. These are pure, device-free helpers over BitWriter/BitReader.

namespace Veng::Net
{
    /// @brief The spatial-leaf quantization resolution — the wire encoding's one tuning knob set.
    ///
    /// Position rounds to a fixed-point grid of step PositionQuantum spanning [-PositionExtent,
    /// +PositionExtent] per axis; rotation encodes each of its smallest-three components in
    /// RotationBits. The defaults (1 mm over a ±4 km world, 9-bit rotation) put a moving pawn's
    /// pose at a small fraction of its f32x7 cost with sub-millimeter and sub-0.2-degree error.
    struct QuantizationSettings
    {
        /// @brief Position grid step in meters; the max per-axis round error is half this.
        f32 PositionQuantum = 0.001f;
        /// @brief Half the encodable world span per axis in meters; a position past it clamps.
        f32 PositionExtent = 4096.0f;
        /// @brief Bits per smallest-three rotation component (the two index bits are separate).
        u32 RotationBits = 9;
    };

    /// @brief Bits needed to encode one position axis at the settings' quantum over its extent.
    /// @param settings  The quantization settings.
    /// @return The per-axis bit count (clamped to 1..32).
    [[nodiscard]] VE_API u32 PositionAxisBits(const QuantizationSettings& settings);

    /// @brief Writes a position as three fixed-point axes, clamped to the encodable span.
    /// @param out       The bit stream to append to.
    /// @param position  The world position; each axis clamps to [-PositionExtent, +PositionExtent].
    /// @param settings  The quantization settings.
    void VE_API EncodePosition(BitWriter& out, const vec3& position,
                               const QuantizationSettings& settings);

    /// @brief Reads a position written by EncodePosition, dequantized to the grid center.
    /// @param in        The bit stream to read from.
    /// @param settings  The quantization settings (must match the encoder's).
    /// @return The dequantized position.
    [[nodiscard]] VE_API vec3 DecodePosition(BitReader& in, const QuantizationSettings& settings);

    /// @brief Writes a rotation as smallest-three: a 2-bit largest-component index + three components.
    ///
    /// The quaternion is normalized and sign-canonicalized (the largest component made non-negative,
    /// which q and -q being the same rotation permits), so the dropped component reconstructs as the
    /// positive root of the remaining magnitude.
    /// @param out       The bit stream to append to.
    /// @param rotation  The rotation; normalized internally.
    /// @param settings  The quantization settings.
    void VE_API EncodeRotation(BitWriter& out, const quat& rotation,
                               const QuantizationSettings& settings);

    /// @brief Reads a rotation written by EncodeRotation, reconstructing the dropped component.
    /// @param in        The bit stream to read from.
    /// @param settings  The quantization settings (must match the encoder's).
    /// @return The dequantized unit quaternion.
    [[nodiscard]] VE_API quat DecodeRotation(BitReader& in, const QuantizationSettings& settings);
}
