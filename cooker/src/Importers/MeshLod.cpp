#include "MeshLod.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <fmt/format.h>

#include <meshoptimizer.h>

namespace Veng::Cook
{
    namespace
    {
        // The default levels' target ratios; a level past the list halves the last.
        constexpr f32 DefaultLevelRatios[] = {0.5f, 0.25f, 0.125f};

        // The default target ratio of the level at `index` (level 1 is index 0).
        f32 DefaultRatio(const usize index)
        {
            const usize last = std::size(DefaultLevelRatios) - 1;
            if (index <= last)
            {
                return DefaultLevelRatios[index];
            }
            return DefaultLevelRatios[last] / static_cast<f32>(1u << (index - last));
        }

        // A threshold derived from an error is capped here: a mesh at least as tall as the view
        // draws level 0 however small the simplification's error.
        constexpr f32 MaxDerivedScreenSize = 1.0f;

        // The smallest error a derived threshold divides by, so a lossless simplification derives
        // the cap rather than a division by zero.
        constexpr f32 MinDerivedError = 1e-6f;

        Result<f32> ReadPositive(const json& object, const char* key, const string& where)
        {
            if (!object[key].is_number())
            {
                return std::unexpected(fmt::format("{}: '{}' must be a number", where, key));
            }
            const f32 value = object[key].get<f32>();
            if (!(value > 0.0f))
            {
                return std::unexpected(fmt::format("{}: '{}' must be greater than 0", where, key));
            }
            return value;
        }

        Result<MeshLodLevelSource> ParseLevel(const json& level, const usize index)
        {
            const string where = fmt::format("'lod.levels[{}]'", index);
            if (!level.is_object())
            {
                return std::unexpected(fmt::format("{} must be an object", where));
            }

            MeshLodLevelSource out;
            out.Ratio = DefaultRatio(index);

            for (const auto& [key, value] : level.items())
            {
                if (key == "ratio")
                {
                    const Result<f32> ratio = ReadPositive(level, "ratio", where);
                    if (!ratio)
                    {
                        return std::unexpected(ratio.error());
                    }
                    if (*ratio >= 1.0f)
                    {
                        return std::unexpected(fmt::format("{}: 'ratio' must be below 1", where));
                    }
                    out.Ratio = *ratio;
                }
                else if (key == "max_error")
                {
                    const Result<f32> error = ReadPositive(level, "max_error", where);
                    if (!error)
                    {
                        return std::unexpected(error.error());
                    }
                    out.MaxError = *error;
                }
                else if (key == "screen_size")
                {
                    const Result<f32> size = ReadPositive(level, "screen_size", where);
                    if (!size)
                    {
                        return std::unexpected(size.error());
                    }
                    out.ScreenSize = *size;
                }
                else if (key == "model")
                {
                    if (!value.is_string() || value.get<string>().empty())
                    {
                        return std::unexpected(
                            fmt::format("{}: 'model' must be a non-empty path string", where));
                    }
                    out.Model = value.get<string>();
                }
                else if (key == "submeshes")
                {
                    if (!value.is_array())
                    {
                        return std::unexpected(fmt::format(
                            "{}: 'submeshes' must be an array of submesh indices", where));
                    }
                    vector<u32> mapping;
                    for (const json& entry : value)
                    {
                        if (!entry.is_number_unsigned())
                        {
                            return std::unexpected(fmt::format(
                                "{}: 'submeshes' entries must be non-negative integers", where));
                        }
                        mapping.push_back(entry.get<u32>());
                    }
                    out.SubMeshes = std::move(mapping);
                }
                else
                {
                    return std::unexpected(fmt::format("{}: unknown key '{}'", where, key));
                }
            }

            if (out.IsAuthored() && (level.contains("ratio") || level.contains("max_error")))
            {
                return std::unexpected(fmt::format(
                    "{}: an authored level ('model') takes no 'ratio' or 'max_error'", where));
            }
            if (!out.IsAuthored() && out.SubMeshes.has_value())
            {
                return std::unexpected(fmt::format(
                    "{}: 'submeshes' maps an authored level's meshes and needs 'model'", where));
            }
            return out;
        }

        void AppendDefaultLevels(MeshLodSource& source, const usize count)
        {
            for (usize i = 0; i < count; ++i)
            {
                MeshLodLevelSource level;
                level.Ratio = DefaultRatio(i);
                source.Levels.push_back(level);
            }
        }
    }

    Result<MeshLodSource> ParseMeshLodSource(const json& meshJson)
    {
        MeshLodSource source;
        if (!meshJson.contains("lod"))
        {
            return source;
        }

        const json& lod = meshJson["lod"];
        if (lod.is_boolean())
        {
            source.Enabled = lod.get<bool>();
            if (source.Enabled)
            {
                AppendDefaultLevels(source, std::size(DefaultLevelRatios));
            }
            return source;
        }
        if (!lod.is_object())
        {
            return std::unexpected(string("'lod' must be a boolean or an object"));
        }

        source.Enabled = true;
        bool levelsGiven = false;
        for (const auto& [key, value] : lod.items())
        {
            if (key == "enabled")
            {
                if (!value.is_boolean())
                {
                    return std::unexpected(string("'lod.enabled' must be a boolean"));
                }
                source.Enabled = value.get<bool>();
            }
            else if (key == "levels")
            {
                levelsGiven = true;
                if (value.is_number_unsigned())
                {
                    const usize count = value.get<usize>();
                    if (count == 0 || count >= MaxMeshLodLevels)
                    {
                        return std::unexpected(
                            fmt::format("'lod.levels' must count between 1 and {} levels",
                                        MaxMeshLodLevels - 1));
                    }
                    AppendDefaultLevels(source, count);
                }
                else if (value.is_array())
                {
                    if (value.empty() || value.size() >= MaxMeshLodLevels)
                    {
                        return std::unexpected(
                            fmt::format("'lod.levels' must list between 1 and {} levels",
                                        MaxMeshLodLevels - 1));
                    }
                    for (usize i = 0; i < value.size(); ++i)
                    {
                        Result<MeshLodLevelSource> level = ParseLevel(value[i], i);
                        if (!level)
                        {
                            return std::unexpected(level.error());
                        }
                        source.Levels.push_back(std::move(*level));
                    }
                }
                else
                {
                    return std::unexpected(
                        string("'lod.levels' must be a level count or an array of levels"));
                }
            }
            else if (key == "screen_error")
            {
                const Result<f32> error = ReadPositive(lod, "screen_error", "'lod'");
                if (!error)
                {
                    return std::unexpected(error.error());
                }
                source.ScreenError = *error;
            }
            else if (key == "normal_weight")
            {
                if (!value.is_number() || value.get<f32>() < 0.0f)
                {
                    return std::unexpected(
                        string("'lod.normal_weight' must be a non-negative number"));
                }
                source.NormalWeight = value.get<f32>();
            }
            else
            {
                return std::unexpected(fmt::format("'lod': unknown key '{}'", key));
            }
        }

        if (!levelsGiven)
        {
            AppendDefaultLevels(source, std::size(DefaultLevelRatios));
        }
        if (!source.Enabled)
        {
            source.Levels.clear();
        }
        return source;
    }

    BuiltMeshLod SimplifyMeshLod(const MeshLodGeometry& geometry, const MeshLodLevelSource& level,
                                 const f32 normalWeight)
    {
        BuiltMeshLod built;
        built.ScreenSize = level.ScreenSize;
        built.SubMeshIndices.resize(geometry.SubMeshes.size());

        const f32 weights[3] = {normalWeight, normalWeight, normalWeight};
        // Sparse: each call reads only the submesh's own vertices out of the shared buffer.
        // Absolute: the error is in model units, normalized once against the whole mesh below, so
        // every submesh measures against the same diameter. LockBorder keeps each submesh's open
        // edges — those it shares with a neighbouring submesh among them — exactly where they are.
        const unsigned int options =
            meshopt_SimplifyLockBorder | meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;

        f32 worst = 0.0f;
        for (usize s = 0; s < geometry.SubMeshes.size(); ++s)
        {
            const MeshLodSubMeshRange& range = geometry.SubMeshes[s];
            const std::span<const u32> source =
                geometry.Indices.subspan(range.IndexOffset, range.IndexCount);
            vector<u32>& out = built.SubMeshIndices[s];
            if (source.size() < 3)
            {
                out.assign(source.begin(), source.end());
                continue;
            }

            const usize target = std::max<usize>(
                3, static_cast<usize>(static_cast<f32>(source.size() / 3) * level.Ratio) * 3);
            out.resize(source.size());
            f32 error = 0.0f;
            const usize written = meshopt_simplifyWithAttributes(
                out.data(), source.data(), source.size(), geometry.Positions, geometry.VertexCount,
                geometry.Stride, geometry.Normals, geometry.Stride, weights, 3, nullptr, target,
                level.MaxError * geometry.Diameter, options, &error);
            out.resize(written);
            meshopt_optimizeVertexCache(out.data(), out.data(), out.size(), geometry.VertexCount);
            worst = std::max(worst, error);
        }

        built.Error = geometry.Diameter > 0.0f ? worst / geometry.Diameter : 0.0f;
        return built;
    }

    u32 CountTriangles(const BuiltMeshLod& level)
    {
        usize indices = 0;
        for (const vector<u32>& subMesh : level.SubMeshIndices)
        {
            indices += subMesh.size();
        }
        return static_cast<u32>(indices / 3);
    }

    Result<vector<f32>> ResolveMeshLodScreenSizes(const std::span<const BuiltMeshLod> levels,
                                                  const f32 screenError)
    {
        vector<f32> sizes;
        sizes.reserve(levels.size());
        f32 previous = std::numeric_limits<f32>::infinity();
        for (usize i = 0; i < levels.size(); ++i)
        {
            const BuiltMeshLod& level = levels[i];
            f32 size = 0.0f;
            if (level.ScreenSize.has_value())
            {
                size = *level.ScreenSize;
                if (size >= previous)
                {
                    return std::unexpected(fmt::format(
                        "level {} 'screen_size' {} does not descend below level {}'s {}", i + 1,
                        size, i, previous));
                }
            }
            else
            {
                if (level.Authored)
                {
                    size = std::isinf(previous) ? 0.5f : previous * 0.5f;
                }
                else
                {
                    size = std::min(screenError / std::max(level.Error, MinDerivedError),
                                    MaxDerivedScreenSize);
                }
                size = std::min(size, previous);
            }
            sizes.push_back(size);
            previous = size;
        }
        return sizes;
    }
}
