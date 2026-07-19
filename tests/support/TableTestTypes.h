#pragma once

// Reflected types the table suites author columns against: an enum, a nested struct, a struct
// carrying an array, and a row struct for the typed row bridge. They live here rather than in
// either suite so the cook-side and runtime-side tests describe the same schema.

#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Veng.h>

namespace VengTest
{
    /// @brief An enum column's type: a small named vocabulary bound to a reflected enum.
    enum class Cadence : Veng::u32
    {
        /// @brief No motion.
        Idle = 0,
        /// @brief Constant motion.
        Steady = 1,
        /// @brief Intermittent motion.
        Burst = 2,
    };

    /// @brief A nested-struct column's type: fixed-size leaves that still encode as a record.
    struct Extent
    {
        /// @brief Width and height.
        Veng::vec2 Size;
        /// @brief Uniform margin around the extent.
        Veng::f32 Margin = 0.0f;
    };

    /// @brief An array column's type: a reflected struct whose single field is a dynamic list.
    struct WeightCurve
    {
        /// @brief The curve's samples, in order.
        Veng::vector<Veng::f32> Samples;
    };

    /// @brief The row struct the typed row bridge binds a whole row into.
    struct TuningRow
    {
        /// @brief The row's integer key.
        Veng::i64 Id = 0;
        /// @brief The row's display label.
        Veng::string Label;
        /// @brief The row's scalar weight.
        Veng::f32 Weight = 0.0f;
        /// @brief The row's cadence.
        Cadence Motion = Cadence::Idle;
    };

    /// @brief Registers the engine builtins plus every type the table suites author columns against.
    /// @param registry  The registry to fill; must be empty.
    inline void RegisterTableTestTypes(Veng::TypeRegistry& registry)
    {
        Veng::RegisterBuiltinTypes(registry);
        registry.Register<Cadence>();
        registry.Register<Extent>();
        registry.Register<WeightCurve>();
        registry.Register<TuningRow>();
    }
}

VE_ENUM(::VengTest::Cadence, 0x03F9311EA0FB6D6DULL)
VE_ENUMERATOR(Idle)
VE_ENUMERATOR(Steady)
VE_ENUMERATOR(Burst)
VE_ENUM_END();

VE_REFLECT(::VengTest::Extent, 0x29BF301FBE0B0FB7ULL)
VE_FIELD(Size)
VE_FIELD(Margin)
VE_REFLECT_END();

VE_REFLECT(::VengTest::WeightCurve, 0xFCF4B7D4642496FEULL)
VE_ARRAY_FIELD(Samples)
VE_REFLECT_END();

VE_REFLECT(::VengTest::TuningRow, 0x4AA4E84F390D2AD4ULL)
VE_FIELD(Id)
VE_FIELD(Label)
VE_FIELD(Weight)
VE_FIELD(Motion)
VE_REFLECT_END();
