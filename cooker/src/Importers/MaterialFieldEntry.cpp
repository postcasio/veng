#include "MaterialFieldEntry.h"

#include <algorithm>
#include <array>

#include <fmt/format.h>

namespace Veng::Cook
{
    namespace
    {
        // The float param spellings, indexed by component count.
        constexpr std::array<const char*, 5> FloatTypeNames = {"", "float", "vec2", "vec3", "vec4"};

        // The authoring spelling of a reflected member's element, so a mismatch error names what
        // the shader actually declares. A uint vector has no spelling — the schema holds scalar
        // uints only.
        string ReflectedElementTypeName(const ReflectedStructField& reflected)
        {
            if (reflected.ComponentCount < 1 || reflected.ComponentCount > 4)
            {
                return fmt::format("{} of {} components", reflected.IsFloat ? "float" : "uint",
                                   reflected.ComponentCount);
            }
            if (reflected.IsFloat)
            {
                return FloatTypeNames[reflected.ComponentCount];
            }
            if (reflected.ComponentCount == 1)
            {
                return "uint";
            }
            return fmt::format("uint{}", reflected.ComponentCount);
        }

        // The element spelling with the member's arity, so an error over an array says so.
        string ReflectedTypeName(const ReflectedStructField& reflected)
        {
            const string element = ReflectedElementTypeName(reflected);
            if (reflected.ElementCount > 1)
            {
                return fmt::format("{}[{}]", element, reflected.ElementCount);
            }
            return element;
        }
    }

    Result<MaterialFieldEntry>
    ResolveMaterialFieldEntry(const json& fieldJson,
                              const std::map<string, const ReflectedStructField*>& reflectedByName,
                              std::string_view errorPrefix)
    {
        MaterialFieldEntry entry;
        bool typeStated = false;

        if (fieldJson.is_string())
        {
            entry.Name = fieldJson.get<string>();
        }
        else if (fieldJson.is_object())
        {
            if (!fieldJson.contains("name") || !fieldJson["name"].is_string())
            {
                return std::unexpected(
                    fmt::format("{}: field entry missing or invalid 'name'", errorPrefix));
            }
            entry.Name = fieldJson["name"].get<string>();

            if (fieldJson.contains("type"))
            {
                if (!fieldJson["type"].is_string())
                {
                    return std::unexpected(fmt::format("{}: field '{}' has a non-string 'type'",
                                                       errorPrefix, entry.Name));
                }
                entry.Type = fieldJson["type"].get<string>();
                typeStated = true;
            }
        }
        else
        {
            return std::unexpected(fmt::format(
                "{}: a field entry must be a member-name string or an object", errorPrefix));
        }

        if (typeStated)
        {
            static const vector<string> AllowedTypes = {
                "texture", "volume", "sampler", "storagebuffer", "param",
                "uint",    "float",  "vec2",    "vec3",          "vec4"};
            if (std::ranges::find(AllowedTypes, entry.Type) == AllowedTypes.end())
            {
                return std::unexpected(fmt::format("{}: field '{}' has unknown type '{}'",
                                                   errorPrefix, entry.Name, entry.Type));
            }
        }

        const auto reflectedIt = reflectedByName.find(entry.Name);
        if (reflectedIt == reflectedByName.end())
        {
            if (!typeStated)
            {
                return std::unexpected(
                    fmt::format("{}: field '{}' does not match any field in MaterialParams, so its "
                                "type cannot be inferred",
                                errorPrefix, entry.Name));
            }
            return entry;
        }
        const ReflectedStructField& reflected = *reflectedIt->second;
        entry.Reflected = &reflected;

        const bool scalarUint = !reflected.IsFloat && reflected.ComponentCount == 1;

        // A handle kind names a bindless slot the loader patches from a single cooked id, and an
        // array of handles wants an array-of-handles idiom with a resident asset per element —
        // its own design, not this schema's.
        if (reflected.ElementCount > 1 && (entry.Type == "texture" || entry.Type == "sampler" ||
                                           entry.Type == "volume" || entry.Type == "storagebuffer"))
        {
            return std::unexpected(fmt::format(
                "{}: field '{}' is declared '{}' but reflects as an array of {} elements; a "
                "bindless handle is a single slot, so an array of handles is not expressible",
                errorPrefix, entry.Name, entry.Type, reflected.ElementCount));
        }

        if (!typeStated || entry.Type == "param")
        {
            if (scalarUint)
            {
                if (!typeStated)
                {
                    return std::unexpected(fmt::format(
                        "{}: field '{}' reflects as a scalar uint, which may be a plain param or a "
                        "texture/sampler/volume/storagebuffer handle — reflection cannot tell them "
                        "apart, so the entry must be an object stating a 'type'",
                        errorPrefix, entry.Name));
                }
                entry.Type = "uint";
                return entry;
            }
            if (!reflected.IsFloat || reflected.ComponentCount > 4)
            {
                return std::unexpected(
                    fmt::format("{}: field '{}' reflects as {}, which a material source cannot "
                                "express",
                                errorPrefix, entry.Name, ReflectedTypeName(reflected)));
            }
            entry.Type = FloatTypeNames[reflected.ComponentCount];
            return entry;
        }

        // A stated type is checked against reflection: the four handle kinds and 'uint' name a
        // scalar uint member, and a float spelling must match the scalar type and component count.
        const bool expectsScalarUint = entry.Type == "uint" || entry.Type == "texture" ||
                                       entry.Type == "sampler" || entry.Type == "volume" ||
                                       entry.Type == "storagebuffer";
        const bool matches = expectsScalarUint
                                 ? scalarUint
                                 : (reflected.IsFloat && reflected.ComponentCount <= 4 &&
                                    entry.Type == FloatTypeNames[reflected.ComponentCount]);
        if (!matches)
        {
            return std::unexpected(
                fmt::format("{}: field '{}' is declared '{}' but the shader reflects it as '{}'",
                            errorPrefix, entry.Name, entry.Type, ReflectedTypeName(reflected)));
        }

        return entry;
    }
}
