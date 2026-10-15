#include <Veng/Renderer/DeviceCapabilities.h>

#include <array>
#include <utility>

#include <fmt/format.h>

namespace Veng::Renderer
{
    namespace
    {
        string JoinStages(const ShaderStage stages)
        {
            string out;
            for (const auto& [stage, name] : std::array{
                     std::pair{ShaderStage::Vertex, "vertex"},
                     std::pair{ShaderStage::Fragment, "fragment"},
                     std::pair{ShaderStage::Compute, "compute"},
                 })
            {
                if (HasFlag(stages, stage))
                {
                    out += out.empty() ? name : fmt::format("|{}", name);
                }
            }
            return out.empty() ? "none" : out;
        }

        string JoinOperations(const SubgroupOperation operations)
        {
            string out;
            for (const auto& [operation, name] : std::array{
                     std::pair{SubgroupOperation::Basic, "basic"},
                     std::pair{SubgroupOperation::Vote, "vote"},
                     std::pair{SubgroupOperation::Arithmetic, "arithmetic"},
                     std::pair{SubgroupOperation::Ballot, "ballot"},
                     std::pair{SubgroupOperation::Shuffle, "shuffle"},
                     std::pair{SubgroupOperation::ShuffleRelative, "shuffle-relative"},
                     std::pair{SubgroupOperation::Clustered, "clustered"},
                     std::pair{SubgroupOperation::Quad, "quad"},
                 })
            {
                if (HasFlag(operations, operation))
                {
                    out += out.empty() ? name : fmt::format("|{}", name);
                }
            }
            return out.empty() ? "none" : out;
        }

        const char* YesNo(const bool value)
        {
            return value ? "yes" : "no";
        }
    }

    string DescribeDeviceCapabilities(const DeviceCapabilities& capabilities)
    {
        const string sizeControl =
            capabilities.SubgroupSizeControl
                ? fmt::format("{}..{} (full subgroups {}, {} per workgroup)",
                              capabilities.MinSubgroupSize, capabilities.MaxSubgroupSize,
                              YesNo(capabilities.ComputeFullSubgroups),
                              capabilities.MaxComputeWorkgroupSubgroups)
                : string("unavailable");
        return fmt::format(
            "Device: {} ({} {}, Vulkan {}.{}.{}); subgroup size {} in {} [{}], quad in all stages "
            "{}; size control {}; float16 {}, int16 {}, 16-bit storage buffers {}; storage writes "
            "B10G11R11Ufloat {}, RGBA16Sfloat {}; MetalFX temporal {}, spatial {}",
            capabilities.DeviceName, capabilities.DriverName, capabilities.DriverInfo,
            (capabilities.ApiVersion >> 22U) & 0x7FU, (capabilities.ApiVersion >> 12U) & 0x3FFU,
            capabilities.ApiVersion & 0xFFFU, capabilities.SubgroupSize,
            JoinStages(capabilities.SubgroupStages),
            JoinOperations(capabilities.SubgroupOperations),
            YesNo(capabilities.QuadOperationsInAllStages), sizeControl,
            YesNo(capabilities.ShaderFloat16), YesNo(capabilities.ShaderInt16),
            YesNo(capabilities.StorageBuffer16BitAccess),
            YesNo(capabilities.B10G11R11UfloatStorage), YesNo(capabilities.RGBA16SfloatStorage),
            YesNo(capabilities.MetalFxTemporal), YesNo(capabilities.MetalFxSpatial));
    }
}
