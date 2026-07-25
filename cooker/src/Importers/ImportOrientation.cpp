#include "ImportOrientation.h"

#include <array>

#include <fmt/format.h>

namespace Veng::Cook
{
    namespace
    {
        // The engine's model-space convention: the frame every declared convention is rotated onto.
        constexpr vec3 EngineForward{0.0f, 0.0f, -1.0f};
        constexpr vec3 EngineUp{0.0f, 1.0f, 0.0f};

        // The axis names an orientation may declare. A bare axis reads as the positive direction,
        // which is why each letter appears three times rather than only signed.
        struct NamedAxis
        {
            const char* Name;
            vec3 Axis;
        };

        constexpr std::array<NamedAxis, 9> AxisNames{{
            {.Name = "+X", .Axis = vec3{1.0f, 0.0f, 0.0f}},
            {.Name = "-X", .Axis = vec3{-1.0f, 0.0f, 0.0f}},
            {.Name = "X", .Axis = vec3{1.0f, 0.0f, 0.0f}},
            {.Name = "+Y", .Axis = vec3{0.0f, 1.0f, 0.0f}},
            {.Name = "-Y", .Axis = vec3{0.0f, -1.0f, 0.0f}},
            {.Name = "Y", .Axis = vec3{0.0f, 1.0f, 0.0f}},
            {.Name = "+Z", .Axis = vec3{0.0f, 0.0f, 1.0f}},
            {.Name = "-Z", .Axis = vec3{0.0f, 0.0f, -1.0f}},
            {.Name = "Z", .Axis = vec3{0.0f, 0.0f, 1.0f}},
        }};

        // The list of accepted spellings, for a rejection's message.
        constexpr const char* AxisNameList = "+X, -X, +Y, -Y, +Z or -Z";

        optional<vec3> ParseAxis(const string& name)
        {
            for (const NamedAxis& candidate : AxisNames)
            {
                if (name == candidate.Name)
                {
                    return candidate.Axis;
                }
            }
            return std::nullopt;
        }
    }

    Result<ImportOrientation> ParseImportOrientation(const json& import)
    {
        if (!import.contains("orientation"))
        {
            return ImportOrientation{};
        }

        const json& declared = import["orientation"];
        if (!declared.is_object())
        {
            return std::unexpected(
                string("'import.orientation' must be an object declaring 'forward' and 'up' axis "
                       "names"));
        }

        // Each key defaults to the engine's own axis, so a partial declaration states only what
        // differs and an empty object is the identity.
        string forwardName = "-Z";
        string upName = "+Y";
        const auto readName = [&declared](const char* key, string& out) -> VoidResult
        {
            if (!declared.contains(key))
            {
                return {};
            }
            if (!declared[key].is_string())
            {
                return std::unexpected(fmt::format(
                    "'import.orientation.{}' must be an axis name string ({})", key, AxisNameList));
            }
            out = declared[key].get<string>();
            return {};
        };
        if (const VoidResult read = readName("forward", forwardName); !read)
        {
            return std::unexpected(read.error());
        }
        if (const VoidResult read = readName("up", upName); !read)
        {
            return std::unexpected(read.error());
        }

        const optional<vec3> forward = ParseAxis(forwardName);
        if (!forward)
        {
            return std::unexpected(
                fmt::format("'import.orientation.forward' is not an axis name '{}' (expected {})",
                            forwardName, AxisNameList));
        }
        const optional<vec3> up = ParseAxis(upName);
        if (!up)
        {
            return std::unexpected(
                fmt::format("'import.orientation.up' is not an axis name '{}' (expected {})",
                            upName, AxisNameList));
        }

        // Two axis-aligned unit vectors are either perpendicular or the same axis, so the dot
        // product is exactly 0 or ±1 and the test needs no tolerance reasoning.
        if (glm::dot(*forward, *up) != 0.0f)
        {
            return std::unexpected(fmt::format(
                "'import.orientation' declares forward '{}' and up '{}' on one axis; the two must "
                "be perpendicular, since a single axis does not determine a rotation",
                forwardName, upName));
        }

        // Each frame's third column is the cross product of the first two, so both bases are
        // right-handed with determinant +1 and the rotation between them is proper.
        const mat3 sourceBasis(*forward, *up, glm::cross(*forward, *up));
        const mat3 engineBasis(EngineForward, EngineUp, glm::cross(EngineForward, EngineUp));

        ImportOrientation orientation;
        orientation.Rotation = engineBasis * glm::transpose(sourceBasis);
        // Every entry is exactly 0 or ±1, so the comparison against the identity is exact.
        orientation.IsIdentity = orientation.Rotation == mat3(1.0f);
        return orientation;
    }
}
