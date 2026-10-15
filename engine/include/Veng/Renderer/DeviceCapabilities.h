#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    /// @brief A class of subgroup (wave) operation a shader stage may use.
    ///
    /// One bit per VkSubgroupFeatureFlagBits class. A shader using an operation of a class the
    /// device does not report for its stage is undefined; check DeviceCapabilities::SupportsSubgroup
    /// before building a pipeline that relies on one.
    enum class SubgroupOperation : u32
    {
        /// @brief No operation class.
        None = 0,
        /// @brief Lane index, lane count, elect, subgroup barriers (WaveGetLaneIndex, WaveIsFirstLane).
        Basic = 1 << 0,
        /// @brief All/any/equal votes (WaveActiveAllTrue, WaveActiveAnyTrue).
        Vote = 1 << 1,
        /// @brief Reductions and scans (WaveActiveSum, WavePrefixSum).
        Arithmetic = 1 << 2,
        /// @brief Ballots, broadcasts and bit counts (WaveActiveBallot, WaveReadLaneFirst,
        ///        WaveActiveCountBits, WavePrefixCountBits).
        Ballot = 1 << 3,
        /// @brief Reads from an arbitrary lane (WaveReadLaneAt).
        Shuffle = 1 << 4,
        /// @brief Reads from a lane at a relative offset.
        ShuffleRelative = 1 << 5,
        /// @brief Reductions over fixed-size clusters of lanes.
        Clustered = 1 << 6,
        /// @brief Exchanges within a quad of four consecutive lanes (QuadReadAcrossX/Y/Diagonal).
        Quad = 1 << 7,
    };

    VE_ENUM_FLAGS(SubgroupOperation)

    /// @brief What the device offers the shaders the engine builds, read once at device creation.
    ///
    /// A plain record of the device's answers, for a feature that has a fallback to decide which
    /// path to take and for a log or a diagnostic to report. Every field is what the device
    /// supports, except where a field says it was also enabled.
    struct DeviceCapabilities
    {
        /// @brief The physical device's name (VkPhysicalDeviceProperties::deviceName).
        string DeviceName;
        /// @brief The driver's name (VkPhysicalDeviceDriverProperties::driverName).
        string DriverName;
        /// @brief The driver's version string (VkPhysicalDeviceDriverProperties::driverInfo).
        string DriverInfo;
        /// @brief The device's Vulkan version, as VK_MAKE_API_VERSION packs it.
        u32 ApiVersion = 0;

        /// @brief The subgroup size a compute or fragment invocation runs at by default.
        ///
        /// The device's default; a stage may run narrower subgroups where MinSubgroupSize allows
        /// unless its pipeline requires full subgroups.
        u32 SubgroupSize = 0;
        /// @brief The shader stages subgroup operations are legal in.
        ShaderStage SubgroupStages{};
        /// @brief The subgroup operation classes legal in SubgroupStages.
        SubgroupOperation SubgroupOperations = SubgroupOperation::None;
        /// @brief True when quad operations are legal in every stage of SubgroupStages, rather than
        ///        in the fragment and compute stages only.
        bool QuadOperationsInAllStages = false;

        /// @brief True when subgroup size control was supported and enabled at device creation.
        ///
        /// Vulkan 1.3's subgroupSizeControl, or VK_EXT_subgroup_size_control on an older device.
        /// MinSubgroupSize, MaxSubgroupSize and MaxComputeWorkgroupSubgroups are meaningful only
        /// when it is true.
        bool SubgroupSizeControl = false;
        /// @brief True when a compute pipeline may require full subgroups, supported and enabled at
        ///        device creation (computeFullSubgroups).
        bool ComputeFullSubgroups = false;
        /// @brief The narrowest subgroup a stage may run at.
        u32 MinSubgroupSize = 0;
        /// @brief The widest subgroup a stage may run at.
        u32 MaxSubgroupSize = 0;
        /// @brief The most subgroups one compute workgroup may hold.
        u32 MaxComputeWorkgroupSubgroups = 0;

        /// @brief True when shaders may use 16-bit floats in arithmetic (shaderFloat16; enabled).
        bool ShaderFloat16 = false;
        /// @brief True when shaders may use 16-bit integers in arithmetic (shaderInt16; enabled).
        bool ShaderInt16 = false;
        /// @brief True when the device supports 16-bit types in storage buffers
        ///        (storageBuffer16BitAccess). Not enabled: no engine shader declares it.
        bool StorageBuffer16BitAccess = false;

        /// @brief True when Format::B10G11R11Ufloat supports storage-image writes.
        bool B10G11R11UfloatStorage = false;
        /// @brief True when Format::RGBA16Sfloat supports storage-image writes.
        bool RGBA16SfloatStorage = false;

        /// @brief True when the platform's temporal upscaler (MetalFX) can run on the device.
        ///
        /// What AntiAliasingMode::MetalFXTemporal needs; without it a renderer runs TAAU. A
        /// renderer whose allocations fall outside the scaler's scale range also runs TAAU, so this
        /// is necessary rather than sufficient.
        bool MetalFxTemporal = false;
        /// @brief True when the platform's spatial upscaler (MetalFX) can run on the device.
        ///
        /// What UpscaleFilter::MetalFXSpatial needs; without it a renderer filters bilinearly.
        bool MetalFxSpatial = false;

        /// @brief Returns true when every class in @p operations is legal in @p stage.
        /// @param stage       One shader stage.
        /// @param operations  The operation classes a shader relies on.
        [[nodiscard]] bool SupportsSubgroup(const ShaderStage stage,
                                            const SubgroupOperation operations) const
        {
            const u32 required = static_cast<u32>(operations);
            return HasFlag(SubgroupStages, stage) &&
                   (static_cast<u32>(SubgroupOperations) & required) == required;
        }
    };

    /// @brief Formats the readout as one line, for the context's creation log.
    /// @param capabilities  The readout to describe.
    /// @return A single line naming the device, driver and every field.
    [[nodiscard]] VE_API string DescribeDeviceCapabilities(const DeviceCapabilities& capabilities);
}
