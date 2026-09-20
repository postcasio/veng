#pragma once

#include <map>
#include <string_view>

#include <Veng/Cook/Types.h>

#include "SlangReflect.h"

// Reads one entry of a material source's "fields" array — a bare member-name string or an
// object — and resolves its authoring type against the fragment shader's reflected
// MaterialParams. Shared by MaterialImporter, which packs the entry, and
// MaterialInstanceImporter, which reads a parent's entries to build the exposed override
// surface, so the two agree on what a field entry means.

namespace Veng::Cook
{
    /// @brief One resolved "fields" entry: the member it names, its canonical authoring type, and
    /// the reflected member itself.
    struct MaterialFieldEntry
    {
        /// @brief The MaterialParams member name the entry declares.
        string Name;
        /// @brief The canonical authoring type: one of texture, volume, sampler, storagebuffer,
        /// uint, float, vec2, vec3, vec4. An omitted type and the "param" spelling both resolve to
        /// one of these.
        string Type;
        /// @brief The reflected member of the same name, or null when the struct declares no such
        /// member — which only the caller can judge, and only reachable for an entry that states
        /// its own type.
        const ReflectedStructField* Reflected = nullptr;
    };

    /// @brief Resolves one "fields" entry against the reflected MaterialParams members.
    ///
    /// A bare string names a member and takes a zero default; an object carries "name" and,
    /// optionally, "type" beside the keys its kind needs. An omitted type comes from reflection,
    /// which is legal only for a float or float-vector member: every scalar-uint member must state
    /// whether it is a plain param or one of the handle kinds, because reflection cannot tell those
    /// apart and a handle silently cooked as a param would sample bindless slot 0. A stated type
    /// that contradicts reflection is an error naming both.
    /// @param fieldJson       The entry, a string or an object.
    /// @param reflectedByName The fragment's MaterialParams members by name.
    /// @param errorPrefix     Importer label the located errors are prefixed with.
    [[nodiscard]] Result<MaterialFieldEntry>
    ResolveMaterialFieldEntry(const json& fieldJson,
                              const std::map<string, const ReflectedStructField*>& reflectedByName,
                              std::string_view errorPrefix);
}
