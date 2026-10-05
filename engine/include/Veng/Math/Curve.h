#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief How a curve moves from one key to the next.
    ///
    /// A key's interpolation governs the segment that leaves it, so the last key's value has no
    /// segment to shape. Values are append-only: an authored curve stores the name, but a cooked
    /// blob stores the index.
    enum class CurveInterp : u32
    {
        /// @brief A straight line to the next key.
        Linear,
        /// @brief Holds this key's value until the next key, then jumps.
        Step,
        /// @brief A Hermite ease with flat tangents at both keys (smoothstep).
        Smooth,
    };

    /// @brief One key of a Curve1D: a value at a time, and how the curve leaves it.
    struct CurveKey
    {
        /// @brief The key's time, in the curve's own units (seconds for a clip channel).
        f32 Time = 0.0f;
        /// @brief The curve's value at Time.
        f32 Value = 0.0f;
        /// @brief How the curve moves from this key to the next.
        CurveInterp Interp = CurveInterp::Linear;

        /// @brief Compares every member.
        bool operator==(const CurveKey&) const = default;
    };

    /// @brief A keyframed scalar curve: a sorted list of keys evaluated at any time.
    ///
    /// General on purpose: any keyframed scalar (a motor envelope, a particle size over life, an
    /// audio fade) is one of these rather than a bespoke curve per feature. Keys are expected sorted
    /// by Time, non-decreasing; two keys at one time author an instant jump, and the later key's value
    /// holds from that time on. An importer authoring curves validates the order.
    struct Curve1D
    {
        /// @brief The keys, sorted by Time.
        vector<CurveKey> Keys;

        /// @brief Evaluates the curve at a time.
        ///
        /// Before the first key the curve holds the first key's value, and after the last key the
        /// last key's value. Between two keys it follows the earlier key's CurveInterp.
        /// @param time  The time to evaluate at.
        /// @return The curve's value, or zero for a curve with no keys.
        [[nodiscard]] f32 Evaluate(f32 time) const;

        /// @brief Whether the curve has no keys, so it evaluates to zero everywhere.
        [[nodiscard]] bool IsEmpty() const { return Keys.empty(); }

        /// @brief Compares every key.
        bool operator==(const Curve1D&) const = default;
    };
}

VE_ENUM(::Veng::CurveInterp, 0x4187DC7C827699BCULL)
VE_ENUMERATOR(Linear)
VE_ENUMERATOR(Step)
VE_ENUMERATOR(Smooth)
VE_ENUM_END();

VE_REFLECT(::Veng::CurveKey, 0x699205CA5A45C103ULL)
VE_FIELD(Time, .DisplayName = "Time")
VE_FIELD(Value, .DisplayName = "Value")
VE_FIELD(Interp, .DisplayName = "Interpolation",
         .Tooltip = "How the curve moves from this key to the next.")
VE_REFLECT_END();

VE_REFLECT(::Veng::Curve1D, 0xCAB05F6AFF77201CULL)
VE_ARRAY_FIELD(Keys, .DisplayName = "Keys")
VE_REFLECT_END();
