#include <Veng/Cook/ShaderTargets.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <unordered_map>

#include <fmt/format.h>

#include <slang/slang.h>

#include <Veng/Asset/Path.h>
#include <Veng/Cook/MetalBindingSlots.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Types.h>

#include "Importers/MetalCompile.h"

#if defined(__APPLE__)
#include <unistd.h>
#endif

#if !defined(VENG_COOK_SHADER_TARGETS) || !defined(VENG_COOK_GPU_BACKEND)
#error "the cook's default shader targets and GPU backend are build definitions"
#endif

namespace Veng::Cook
{
    namespace
    {
        bool EqualsIgnoringCase(const std::string_view a, const std::string_view b)
        {
            return std::ranges::equal(a, b, [](const char x, const char y)
                                      { return std::tolower(x) == std::tolower(y); });
        }

        std::string_view Trim(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
            {
                text.remove_suffix(1);
            }
            return text;
        }

        // Single-quotes an argument for /bin/sh.
        string ShellQuote(const std::string_view arg)
        {
            string quoted = "'";
            for (const char c : arg)
            {
                if (c == '\'')
                {
                    quoted += "'\\''";
                }
                else
                {
                    quoted += c;
                }
            }
            quoted += "'";
            return quoted;
        }

        // Runs a command line, capturing its stdout and stderr together. Returns the exit status,
        // or -1 when the process could not be started.
        int RunProcess(const vector<string>& args, string& output)
        {
            string command;
            for (const string& arg : args)
            {
                command += command.empty() ? "" : " ";
                command += ShellQuote(arg);
            }
            command += " 2>&1";

            FILE* pipe = popen(command.c_str(), "r");
            if (pipe == nullptr)
            {
                return -1;
            }
            char buffer[512];
            while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
            {
                output += buffer;
            }
            const int status = pclose(pipe);
            return status;
        }

        optional<u32> MapShaderStage(const SlangStage stage)
        {
            switch (stage)
            {
            case SLANG_STAGE_VERTEX:
                return static_cast<u32>(Renderer::ShaderStage::Vertex);
            case SLANG_STAGE_FRAGMENT:
                return static_cast<u32>(Renderer::ShaderStage::Fragment);
            case SLANG_STAGE_COMPUTE:
                return static_cast<u32>(Renderer::ShaderStage::Compute);
            default:
                return std::nullopt;
            }
        }

        void SetName(char (&dest)[ShaderNameCapacity], const std::string_view name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::copy_n(name.data(), n, dest);
            dest[n] = '\0';
        }

        string NameOf(slang::VariableLayoutReflection* variable)
        {
            const char* name = variable->getName();
            return name != nullptr ? string(name) : string("<unnamed>");
        }

        bool HasCategory(slang::VariableLayoutReflection* variable,
                         const slang::ParameterCategory category)
        {
            for (unsigned i = 0; i < variable->getCategoryCount(); ++i)
            {
                if (variable->getCategoryByIndex(i) == category)
                {
                    return true;
                }
            }
            return false;
        }

        // Maps a binding's reflected type (Texture2D, SamplerState, Sampler2D, ConstantBuffer<T>,
        // [RW]StructuredBuffer<T>, ...) to a Renderer::DescriptorType ordinal. Read-only vs.
        // read-write images are told apart by the leaf type's resource access.
        Result<u32> MapBindingType(slang::TypeLayoutReflection* typeLayout, const string& name)
        {
            if (typeLayout->getBindingRangeCount() == 0)
            {
                return std::unexpected(fmt::format("binding '{}' has no binding range", name));
            }

            const slang::BindingType bindingType = typeLayout->getBindingRangeType(0);
            switch (bindingType)
            {
            case slang::BindingType::CombinedTextureSampler:
                return static_cast<u32>(Renderer::DescriptorType::CombinedImageSampler);
            case slang::BindingType::Texture:
            {
                slang::TypeLayoutReflection* leaf = typeLayout->getBindingRangeLeafTypeLayout(0);
                return leaf->getResourceAccess() == SLANG_RESOURCE_ACCESS_READ
                           ? static_cast<u32>(Renderer::DescriptorType::SampledImage)
                           : static_cast<u32>(Renderer::DescriptorType::StorageImage);
            }
            // RWTexture2D and friends carry the mutable flag, so Slang reports them as
            // MutableTexture rather than Texture; they are always read-write storage images.
            case slang::BindingType::MutableTexture:
                return static_cast<u32>(Renderer::DescriptorType::StorageImage);
            case slang::BindingType::Sampler:
                return static_cast<u32>(Renderer::DescriptorType::Sampler);
            case slang::BindingType::ConstantBuffer:
                return static_cast<u32>(Renderer::DescriptorType::UniformBuffer);
            case slang::BindingType::TypedBuffer:
            case slang::BindingType::RawBuffer:
            case slang::BindingType::MutableTypedBuffer:
            case slang::BindingType::MutableRawBuffer:
                return static_cast<u32>(Renderer::DescriptorType::StorageBuffer);
            default:
                return std::unexpected(fmt::format("binding '{}' has unsupported binding type {}",
                                                   name, static_cast<int>(bindingType)));
            }
        }

        // The number of argument-buffer elements a parameter-block field occupies on Metal: two for
        // a combined image sampler (the texture, then the sampler), one per array element.
        u32 MetalElementCount(slang::TypeLayoutReflection* type)
        {
            if (type->getKind() == slang::TypeReflection::Kind::Array)
            {
                return static_cast<u32>(type->getElementCount()) *
                       MetalElementCount(type->getElementTypeLayout());
            }
            if (type->getBindingRangeCount() > 0 &&
                type->getBindingRangeType(0) == slang::BindingType::CombinedTextureSampler)
            {
                return 2;
            }
            return 1;
        }

        // The structured buffer a field's type names, if any: its element layout.
        slang::TypeLayoutReflection* StructuredElement(slang::TypeLayoutReflection* type)
        {
            if (type->getKind() != slang::TypeReflection::Kind::Resource)
            {
                return nullptr;
            }
            const SlangResourceShape shape = type->getResourceShape();
            if ((shape & SLANG_RESOURCE_BASE_SHAPE_MASK) != SLANG_STRUCTURED_BUFFER)
            {
                return nullptr;
            }
            return type->getElementTypeLayout();
        }

        // Holds two layouts of one type to the same member offsets (and, where the type is laid
        // out in an array or a buffer, the same size), recursing into structs and arrays. A
        // vector's own size may differ — a Metal float3 is 16 bytes in a constant block — so only
        // the offsets of what follows it, and the strides and sizes that index past it, are held.
        VoidResult CompareLayouts(slang::TypeLayoutReflection* spirv,
                                  slang::TypeLayoutReflection* metal, const string& structName,
                                  const string& path, const bool sizeMatters)
        {
            constexpr SlangParameterCategory Uniform = SLANG_PARAMETER_CATEGORY_UNIFORM;
            const slang::TypeReflection::Kind kind = spirv->getKind();
            if (kind == slang::TypeReflection::Kind::Struct)
            {
                const string name = spirv->getName() != nullptr ? spirv->getName() : structName;
                for (unsigned i = 0; i < spirv->getFieldCount() && i < metal->getFieldCount(); ++i)
                {
                    slang::VariableLayoutReflection* a = spirv->getFieldByIndex(i);
                    slang::VariableLayoutReflection* b = metal->getFieldByIndex(i);
                    const string member = path.empty() ? NameOf(a) : path + "." + NameOf(a);
                    if (a->getOffset(Uniform) != b->getOffset(Uniform))
                    {
                        return std::unexpected(fmt::format(
                            "struct '{}' member '{}' is at offset {} on SPIR-V but {} on Metal; "
                            "pack a float3 and the scalar after it into a float4, or pad, so both "
                            "agree",
                            name, member, a->getOffset(Uniform), b->getOffset(Uniform)));
                    }
                    const VoidResult inner =
                        CompareLayouts(a->getTypeLayout(), b->getTypeLayout(), name, member, false);
                    if (!inner)
                    {
                        return inner;
                    }
                }
                if (sizeMatters && spirv->getSize(Uniform) != metal->getSize(Uniform))
                {
                    return std::unexpected(fmt::format(
                        "struct '{}'{} is {} bytes on SPIR-V but {} on Metal, so an array of it "
                        "strides differently; pad it so both agree",
                        name, path.empty() ? "" : fmt::format(" (at '{}')", path),
                        spirv->getSize(Uniform), metal->getSize(Uniform)));
                }
                return {};
            }
            if (kind == slang::TypeReflection::Kind::Array)
            {
                if (spirv->getElementStride(Uniform) != metal->getElementStride(Uniform))
                {
                    return std::unexpected(fmt::format(
                        "struct '{}' array '{}' strides {} bytes on SPIR-V but {} on Metal; use an "
                        "element type both lay out alike (a float4 rather than a float3 or scalar)",
                        structName, path, spirv->getElementStride(Uniform),
                        metal->getElementStride(Uniform)));
                }
                return CompareLayouts(spirv->getElementTypeLayout(), metal->getElementTypeLayout(),
                                      structName, path + "[]", true);
            }
            if (kind == slang::TypeReflection::Kind::Matrix &&
                spirv->getSize(Uniform) != metal->getSize(Uniform))
            {
                return std::unexpected(fmt::format(
                    "struct '{}' matrix '{}' is {} bytes on SPIR-V but {} on Metal", structName,
                    path, spirv->getSize(Uniform), metal->getSize(Uniform)));
            }
            return {};
        }

        // Compares the data a binding's type carries: a constant block's element struct, or a
        // structured buffer's element.
        VoidResult CompareBindingData(slang::TypeLayoutReflection* spirv,
                                      slang::TypeLayoutReflection* metal, const string& binding)
        {
            if (spirv->getKind() == slang::TypeReflection::Kind::ConstantBuffer)
            {
                return CompareLayouts(spirv->getElementTypeLayout(), metal->getElementTypeLayout(),
                                      binding, "", false);
            }
            if (spirv->getKind() == slang::TypeReflection::Kind::Array)
            {
                return CompareBindingData(spirv->getElementTypeLayout(),
                                          metal->getElementTypeLayout(), binding);
            }
            slang::TypeLayoutReflection* spirvElement = StructuredElement(spirv);
            slang::TypeLayoutReflection* metalElement = StructuredElement(metal);
            if (spirvElement != nullptr && metalElement != nullptr)
            {
                return CompareLayouts(spirvElement, metalElement, binding, "", true);
            }
            return {};
        }

        // Finds a top-level parameter of a layout by name.
        slang::VariableLayoutReflection* FindParameter(slang::ProgramLayout& layout,
                                                       const string& name)
        {
            for (unsigned i = 0; i < layout.getParameterCount(); ++i)
            {
                slang::VariableLayoutReflection* param = layout.getParameterByIndex(i);
                if (NameOf(param) == name)
                {
                    return param;
                }
            }
            return nullptr;
        }

        // What the Metal side of one SPIR-V binding resolved to.
        struct MetalPlacement
        {
            CookedMetalBinding Kind = CookedMetalBinding::None;
            u32 Index = 0;
        };

        // The Metal side of one parameter block: each field's first element, checked against the
        // elements Slang reports where it reports them.
        Result<vector<u32>> MetalBlockElements(slang::VariableLayoutReflection* block,
                                               const string& blockName)
        {
            slang::TypeLayoutReflection* fields =
                block->getTypeLayout()->getElementVarLayout()->getTypeLayout();
            vector<u32> elements;
            u32 next = 0;
            for (unsigned i = 0; i < fields->getFieldCount(); ++i)
            {
                slang::VariableLayoutReflection* field = fields->getFieldByIndex(i);
                if (HasCategory(field, slang::ParameterCategory::MetalArgumentBufferElement))
                {
                    const auto reported = static_cast<u32>(
                        field->getOffset(slang::ParameterCategory::MetalArgumentBufferElement));
                    if (reported != next)
                    {
                        return std::unexpected(fmt::format(
                            "block '{}' field '{}' is at argument-buffer element {}, but the "
                            "fields before it take {} elements",
                            blockName, NameOf(field), reported, next));
                    }
                }
                elements.push_back(next);
                next += MetalElementCount(field->getTypeLayout());
            }
            return elements;
        }

        // The slot Slang placed a Metal block or buffer at.
        optional<u32> MetalSlot(slang::VariableLayoutReflection* param)
        {
            if (!HasCategory(param, slang::ParameterCategory::ConstantBuffer))
            {
                return std::nullopt;
            }
            return static_cast<u32>(param->getOffset(slang::ParameterCategory::ConstantBuffer));
        }

        bool IsParameterBlock(slang::VariableLayoutReflection* param)
        {
            return param->getTypeLayout()->getKind() == slang::TypeReflection::Kind::ParameterBlock;
        }

        bool IsConstantBuffer(slang::VariableLayoutReflection* param)
        {
            return param->getTypeLayout()->getKind() == slang::TypeReflection::Kind::ConstantBuffer;
        }
    }

    Result<ShaderTargetSet> ResolveShaderTargets(const std::string_view setting,
                                                 const std::string_view backend)
    {
        if (EqualsIgnoringCase(setting, "Native"))
        {
            if (EqualsIgnoringCase(backend, "Vulkan"))
            {
                return ShaderTargetSet{}.With(ShaderTarget::SpirV);
            }
            if (EqualsIgnoringCase(backend, "Metal"))
            {
                return ShaderTargetSet{}.With(ShaderTarget::Metal);
            }
            return std::unexpected(fmt::format(
                "GPU backend '{}' is not one of Vulkan or Metal, so 'Native' names no target",
                backend));
        }
        if (EqualsIgnoringCase(setting, "All"))
        {
            return ShaderTargetSet{}.With(ShaderTarget::SpirV).With(ShaderTarget::Metal);
        }

        ShaderTargetSet targets;
        std::string_view rest = setting;
        while (!rest.empty())
        {
            const usize comma = rest.find(',');
            const std::string_view name = Trim(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            if (EqualsIgnoringCase(name, "spirv"))
            {
                targets = targets.With(ShaderTarget::SpirV);
            }
            else if (EqualsIgnoringCase(name, "metal"))
            {
                targets = targets.With(ShaderTarget::Metal);
            }
            else
            {
                return std::unexpected(fmt::format(
                    "shader target '{}' is not one of Native, All, spirv or metal", name));
            }
        }
        if (targets.IsEmpty())
        {
            return std::unexpected("the shader-target setting names no target");
        }
        return targets;
    }

    std::string_view NativeGpuBackend()
    {
        return VENG_COOK_GPU_BACKEND;
    }

    ShaderTarget NativeShaderTarget()
    {
        return EqualsIgnoringCase(VENG_COOK_GPU_BACKEND, "Metal") ? ShaderTarget::Metal
                                                                  : ShaderTarget::SpirV;
    }

    ShaderTargetSet DefaultShaderTargets()
    {
        const Result<ShaderTargetSet> targets =
            ResolveShaderTargets(VENG_COOK_SHADER_TARGETS, VENG_COOK_GPU_BACKEND);
        return targets ? *targets : ShaderTargetSet{}.With(NativeShaderTarget());
    }

    string FormatShaderTargets(const ShaderTargetSet targets)
    {
        string names;
        if (targets.Has(ShaderTarget::SpirV))
        {
            names += "spirv";
        }
        if (targets.Has(ShaderTarget::Metal))
        {
            names += names.empty() ? "metal" : ",metal";
        }
        return names;
    }

    const optional<string>& ProbeMetalToolchain()
    {
        static const optional<string> version = []() -> optional<string>
        {
#if defined(__APPLE__)
            string output;
            if (RunProcess({"xcrun", "-sdk", "macosx", "metal", "--version"}, output) != 0)
            {
                return std::nullopt;
            }
            const string line = output.substr(0, output.find('\n'));
            return line.empty() ? std::nullopt : optional<string>(line);
#else
            return std::nullopt;
#endif
        }();
        return version;
    }

    void DescribeSlangTarget(slang::IGlobalSession& session, const ShaderTarget target,
                             slang::TargetDesc& desc)
    {
        if (target == ShaderTarget::Metal)
        {
            desc.format = SLANG_METAL;
            desc.floatingPointMode = SLANG_FLOATING_POINT_MODE_FAST;
            return;
        }
        desc.format = SLANG_SPIRV;
        desc.profile = session.findProfile("spirv_1_5");
    }

    Result<ReflectedShaderInterface> ReflectShaderInterface(slang::ProgramLayout& spirv,
                                                            slang::ProgramLayout* metal,
                                                            const std::string_view shaderName)
    {
        namespace Slots = MetalBindingSlots;
        const auto fail = [&](const string& detail) -> Result<ReflectedShaderInterface>
        { return std::unexpected(fmt::format("shader '{}': {}", shaderName, detail)); };

        if (spirv.getEntryPointCount() != 1)
        {
            return fail("expected exactly one entry point in the linked program");
        }
        const optional<u32> stageMask = MapShaderStage(spirv.getEntryPointByIndex(0)->getStage());
        if (!stageMask)
        {
            return fail("the entry point has an unsupported stage");
        }

        ReflectedShaderInterface reflected{.StageMask = *stageMask};
        if (spirv.getEntryPointByIndex(0)->getStage() == SLANG_STAGE_COMPUTE)
        {
            std::array<SlangUInt, 3> size{};
            spirv.getEntryPointByIndex(0)->getComputeThreadGroupSize(3, size.data());
            for (usize axis = 0; axis < size.size(); ++axis)
            {
                reflected.ThreadgroupSize[axis] = static_cast<u32>(size[axis]);
            }
        }

        // Records one binding, finding its Metal side when Metal is compiled.
        const auto addBinding = [&](slang::VariableLayoutReflection* variable, const u32 set,
                                    const u32 binding, const MetalPlacement placement) -> VoidResult
        {
            // The bindless registries' sets are prepended to every pipeline layout by the engine,
            // so they are recognized here and left out of the declared interface.
            if (set < Renderer::BindlessRegistry::FirstUserSet)
            {
                return {};
            }
            const string name = NameOf(variable);
            const Result<u32> type = MapBindingType(variable->getTypeLayout(), name);
            if (!type)
            {
                return std::unexpected(type.error());
            }
            slang::TypeLayoutReflection* typeLayout = variable->getTypeLayout();
            const u32 count = typeLayout->getKind() == slang::TypeReflection::Kind::Array
                                  ? static_cast<u32>(typeLayout->getElementCount())
                                  : 1;
            CookedDescriptorBinding cooked{
                .Set = set,
                .Binding = binding,
                .Type = *type,
                .Count = count,
                .StageMask = *stageMask,
                .MetalBinding = static_cast<u32>(placement.Kind),
                .MetalIndex = placement.Index,
            };
            SetName(cooked.Name, name);
            reflected.Bindings.push_back(cooked);
            return {};
        };

        for (unsigned i = 0; i < spirv.getParameterCount(); ++i)
        {
            slang::VariableLayoutReflection* param = spirv.getParameterByIndex(i);
            const string name = NameOf(param);
            slang::VariableLayoutReflection* metalParam =
                metal != nullptr ? FindParameter(*metal, name) : nullptr;
            if (metal != nullptr && metalParam == nullptr)
            {
                return fail(fmt::format("'{}' has no counterpart in the Metal layout", name));
            }

            if (HasCategory(param, slang::ParameterCategory::PushConstantBuffer))
            {
                slang::TypeLayoutReflection* element =
                    param->getTypeLayout()->getElementTypeLayout();
                const usize size =
                    element != nullptr ? element->getSize(slang::ParameterCategory::Uniform) : 0;
                if (size == 0 || size > 128)
                {
                    return fail(
                        fmt::format("push constant '{}' has invalid size {} (must be 1-128 bytes)",
                                    name, size));
                }
                if (metalParam != nullptr)
                {
                    const optional<u32> slot = MetalSlot(metalParam);
                    if (slot != Slots::PushConstants)
                    {
                        return fail(fmt::format(
                            "push constant '{}' is in Metal buffer slot {}, not the reserved slot "
                            "{}; declare it with VE_PUSH_CONSTANTS (Veng/binding.slang)",
                            name, slot.value_or(~0u), Slots::PushConstants));
                    }
                    const VoidResult layout = CompareBindingData(param->getTypeLayout(),
                                                                 metalParam->getTypeLayout(), name);
                    if (!layout)
                    {
                        return fail(layout.error());
                    }
                }
                CookedPushConstantBlock block{
                    .Offset = static_cast<u32>(
                        param->getOffset(slang::ParameterCategory::PushConstantBuffer)),
                    .Size = static_cast<u32>(size),
                    .StageMask = *stageMask,
                };
                SetName(block.Name, name);
                reflected.PushConstants.push_back(block);
                continue;
            }

            if (IsParameterBlock(param))
            {
                const auto set = static_cast<u32>(
                    param->getOffset(slang::ParameterCategory::SubElementRegisterSpace));
                slang::TypeLayoutReflection* fields =
                    param->getTypeLayout()->getElementVarLayout()->getTypeLayout();

                vector<u32> elements;
                slang::TypeLayoutReflection* metalFields = nullptr;
                if (metalParam != nullptr)
                {
                    if (!IsParameterBlock(metalParam) || MetalSlot(metalParam) != set)
                    {
                        return fail(fmt::format(
                            "set {} block '{}' is not in Metal buffer slot {}; declare it with "
                            "VE_DESCRIPTOR_SET (Veng/binding.slang)",
                            set, name, Slots::DescriptorSetBase + set));
                    }
                    if (set >= Slots::MaxDescriptorSets)
                    {
                        return fail(
                            fmt::format("set {} is past the {} sets a layout binds on Metal", set,
                                        Slots::MaxDescriptorSets));
                    }
                    Result<vector<u32>> placed = MetalBlockElements(metalParam, name);
                    if (!placed)
                    {
                        return fail(placed.error());
                    }
                    elements = std::move(*placed);
                    metalFields =
                        metalParam->getTypeLayout()->getElementVarLayout()->getTypeLayout();
                }

                for (unsigned f = 0; f < fields->getFieldCount(); ++f)
                {
                    slang::VariableLayoutReflection* field = fields->getFieldByIndex(f);
                    if (!HasCategory(field, slang::ParameterCategory::DescriptorTableSlot))
                    {
                        return fail(fmt::format(
                            "set {} block '{}' field '{}' is not a resource; a set's block holds "
                            "only resources, and plain data belongs in a constant buffer",
                            set, name, NameOf(field)));
                    }
                    const auto binding = static_cast<u32>(
                        field->getOffset(slang::ParameterCategory::DescriptorTableSlot));
                    MetalPlacement placement;
                    if (metalFields != nullptr)
                    {
                        if (f >= metalFields->getFieldCount())
                        {
                            return fail(fmt::format(
                                "set {} binding {} '{}.{}' has no counterpart in the Metal layout",
                                set, binding, name, NameOf(field)));
                        }
                        const VoidResult layout =
                            CompareBindingData(field->getTypeLayout(),
                                               metalFields->getFieldByIndex(f)->getTypeLayout(),
                                               name + "." + NameOf(field));
                        if (!layout)
                        {
                            return fail(layout.error());
                        }
                        placement =
                            MetalPlacement{.Kind = CookedMetalBinding::ArgumentBufferElement,
                                           .Index = elements[f]};
                    }
                    const VoidResult added = addBinding(field, set, binding, placement);
                    if (!added)
                    {
                        return fail(added.error());
                    }
                }
                continue;
            }

            const auto set = static_cast<u32>(param->getBindingSpace());
            const auto binding = static_cast<u32>(param->getBindingIndex());
            MetalPlacement placement;
            if (metalParam != nullptr)
            {
                const optional<u32> slot = MetalSlot(metalParam);
                const bool dynamicSlot =
                    slot.has_value() && *slot >= Slots::DynamicUniformBufferBase &&
                    *slot < Slots::DynamicUniformBufferBase + Slots::MaxDynamicUniformBuffers;
                if (!IsConstantBuffer(param))
                {
                    return fail(fmt::format(
                        "set {} binding {} '{}' is declared outside its set's parameter block, "
                        "which Metal cannot bind; declare the set with VE_DESCRIPTOR_SET "
                        "(Veng/binding.slang)",
                        set, binding, name));
                }
                if (!dynamicSlot)
                {
                    return fail(fmt::format(
                        "set {} binding {} '{}' is a uniform buffer outside its set's parameter "
                        "block, so it must be a dynamic uniform buffer at a reserved Metal slot "
                        "({}-{}); declare it with VE_DYNAMIC_UNIFORM_BUFFER (Veng/binding.slang)",
                        set, binding, name, Slots::DynamicUniformBufferBase,
                        Slots::DynamicUniformBufferBase + Slots::MaxDynamicUniformBuffers - 1));
                }
                const VoidResult layout =
                    CompareBindingData(param->getTypeLayout(), metalParam->getTypeLayout(), name);
                if (!layout)
                {
                    return fail(layout.error());
                }
                placement = MetalPlacement{.Kind = CookedMetalBinding::BufferSlot, .Index = *slot};
            }
            const VoidResult added = addBinding(param, set, binding, placement);
            if (!added)
            {
                return fail(added.error());
            }
        }

        // Every Metal parameter has a SPIR-V counterpart, so neither side binds what the other
        // does not.
        if (metal != nullptr)
        {
            for (unsigned i = 0; i < metal->getParameterCount(); ++i)
            {
                const string name = NameOf(metal->getParameterByIndex(i));
                if (FindParameter(spirv, name) == nullptr)
                {
                    return fail(
                        fmt::format("Metal parameter '{}' has no counterpart in SPIR-V", name));
                }
            }
        }

        return reflected;
    }

    namespace
    {
        bool IsIdentifierChar(const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        // The struct definitions of an MSL source, each as its fields' types in declaration order.
        // A field's type is its declaration with the trailing name (and any attribute) removed.
        std::unordered_map<string, vector<string>> ParseMetalStructs(const std::string_view source)
        {
            std::unordered_map<string, vector<string>> structs;
            usize at = 0;
            while ((at = source.find("struct ", at)) != std::string_view::npos)
            {
                // Only a declaration at the start of a line, not "struct" inside another token.
                if (at > 0 && source[at - 1] != '\n')
                {
                    at += 7;
                    continue;
                }
                const usize nameBegin = at + 7;
                usize nameEnd = nameBegin;
                while (nameEnd < source.size() && IsIdentifierChar(source[nameEnd]))
                {
                    ++nameEnd;
                }
                const usize open = source.find('{', nameEnd);
                const usize semicolon = source.find(';', nameEnd);
                if (nameEnd == nameBegin || open == std::string_view::npos ||
                    (semicolon != std::string_view::npos && semicolon < open))
                {
                    at = nameEnd;
                    continue;
                }
                const usize close = source.find('}', open);
                if (close == std::string_view::npos)
                {
                    break;
                }
                vector<string> fields;
                std::string_view body = source.substr(open + 1, close - open - 1);
                while (!body.empty())
                {
                    const usize end = body.find(';');
                    const std::string_view field = body.substr(0, end);
                    body =
                        end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
                    // Drop #line directives and attributes, then the trailing name.
                    string text;
                    for (usize i = 0; i < field.size();)
                    {
                        if (field[i] == '#')
                        {
                            const usize eol = field.find('\n', i);
                            i = eol == std::string_view::npos ? field.size() : eol;
                            continue;
                        }
                        if (field.substr(i, 2) == "[[")
                        {
                            const usize attrEnd = field.find("]]", i);
                            i = attrEnd == std::string_view::npos ? field.size() : attrEnd + 2;
                            continue;
                        }
                        text += field[i++];
                    }
                    const std::string_view trimmed = Trim(text);
                    usize nameStart = trimmed.size();
                    while (nameStart > 0 && IsIdentifierChar(trimmed[nameStart - 1]))
                    {
                        --nameStart;
                    }
                    const std::string_view type = Trim(trimmed.substr(0, nameStart));
                    if (!type.empty())
                    {
                        fields.emplace_back(type);
                    }
                }
                structs.emplace(string(source.substr(nameBegin, nameEnd - nameBegin)),
                                std::move(fields));
                at = close;
            }
            return structs;
        }

        // The argument-buffer elements a field of MSL type @p type occupies: one per resource or
        // pointer, an array's count times its element's, a struct the sum of its fields'.
        u32 MetalTypeElements(const std::string_view type,
                              const std::unordered_map<string, vector<string>>& structs,
                              const u32 depth = 0)
        {
            if (depth > 16 || type.find('*') != std::string_view::npos)
            {
                return 1;
            }
            if (type.starts_with("array<") && type.ends_with('>'))
            {
                const std::string_view inner = type.substr(6, type.size() - 7);
                // The element count follows the last comma outside any nested template.
                int nesting = 0;
                usize comma = std::string_view::npos;
                for (usize i = 0; i < inner.size(); ++i)
                {
                    nesting += inner[i] == '<' ? 1 : inner[i] == '>' ? -1 : 0;
                    if (inner[i] == ',' && nesting == 0)
                    {
                        comma = i;
                    }
                }
                if (comma == std::string_view::npos)
                {
                    return 1;
                }
                u32 count = 0;
                for (const char c : inner.substr(comma + 1))
                {
                    if (std::isdigit(static_cast<unsigned char>(c)) != 0)
                    {
                        count = (count * 10) + static_cast<u32>(c - '0');
                    }
                }
                return count * MetalTypeElements(Trim(inner.substr(0, comma)), structs, depth + 1);
            }
            const auto found = structs.find(string(type));
            if (found == structs.end())
            {
                return 1;
            }
            u32 total = 0;
            for (const string& field : found->second)
            {
                total += MetalTypeElements(field, structs, depth + 1);
            }
            return total;
        }

        // The struct type of each buffer the stage function takes by pointer, by buffer slot.
        std::unordered_map<u32, string> MetalStageBuffers(const std::string_view source)
        {
            std::unordered_map<u32, string> buffers;
            for (const std::string_view attribute : {"[[kernel]]", "[[vertex]]", "[[fragment]]"})
            {
                const usize at = source.find(attribute);
                if (at == std::string_view::npos)
                {
                    continue;
                }
                const usize open = source.find('(', at);
                if (open == std::string_view::npos)
                {
                    return buffers;
                }
                // The parameter list, split at its top-level commas.
                int nesting = 0;
                usize begin = open + 1;
                for (usize i = open + 1; i < source.size(); ++i)
                {
                    const char c = source[i];
                    nesting += (c == '(' || c == '<') ? 1 : (c == ')' || c == '>') ? -1 : 0;
                    if ((c == ',' && nesting == 0) || nesting < 0)
                    {
                        const std::string_view param = Trim(source.substr(begin, i - begin));
                        begin = i + 1;
                        const usize slotAt = param.find("[[buffer(");
                        const usize typeEnd = param.find_first_of(" \t*");
                        if (slotAt != std::string_view::npos && typeEnd != std::string_view::npos &&
                            param.find('*') != std::string_view::npos)
                        {
                            u32 slot = 0;
                            for (usize d = slotAt + 9;
                                 d < param.size() &&
                                 std::isdigit(static_cast<unsigned char>(param[d])) != 0;
                                 ++d)
                            {
                                slot = (slot * 10) + static_cast<u32>(param[d] - '0');
                            }
                            buffers.emplace(slot, string(param.substr(0, typeEnd)));
                        }
                        if (nesting < 0)
                        {
                            break;
                        }
                    }
                }
                return buffers;
            }
            return buffers;
        }
    }

    VoidResult VerifyMetalArgumentBuffers(const ReflectedShaderInterface& reflected,
                                          const std::string_view metalSource,
                                          const std::string_view shaderName)
    {
        // Each user set's argument-buffer bindings, in binding order (their field order).
        std::map<u32, vector<const CookedDescriptorBinding*>> sets;
        for (const CookedDescriptorBinding& binding : reflected.Bindings)
        {
            if (binding.MetalBinding == static_cast<u32>(CookedMetalBinding::ArgumentBufferElement))
            {
                sets[binding.Set].push_back(&binding);
            }
        }
        if (sets.empty())
        {
            return {};
        }

        const auto structs = ParseMetalStructs(metalSource);
        const auto buffers = MetalStageBuffers(metalSource);
        for (auto& [set, bindings] : sets)
        {
            std::ranges::sort(bindings, {}, &CookedDescriptorBinding::Binding);
            const auto buffer = buffers.find(MetalBindingSlots::DescriptorSetBase + set);
            if (buffer == buffers.end())
            {
                continue;
            }
            const auto block = structs.find(buffer->second);
            if (block == structs.end())
            {
                return std::unexpected(
                    fmt::format("shader '{}': set {}'s argument buffer type '{}' is not defined in "
                                "the generated Metal source",
                                shaderName, set, buffer->second));
            }
            const vector<string>& fields = block->second;
            if (fields.size() != bindings.size())
            {
                return std::unexpected(fmt::format(
                    "shader '{}': set {}'s block has {} fields in the generated Metal source but "
                    "{} in reflection, so its fields would bind at the wrong argument-buffer "
                    "elements",
                    shaderName, set, fields.size(), bindings.size()));
            }
            u32 element = 0;
            for (usize f = 0; f < fields.size(); ++f)
            {
                const CookedDescriptorBinding& binding = *bindings[f];
                const bool combined =
                    binding.Type ==
                    static_cast<u32>(Renderer::DescriptorType::CombinedImageSampler);
                const u32 expected = binding.Count * (combined ? 2u : 1u);
                const u32 emitted = MetalTypeElements(fields[f], structs);
                if (element != binding.MetalIndex || emitted != expected)
                {
                    const string hint =
                        combined && emitted == binding.Count
                            ? "; a combined sampler the stage never samples is emitted as a lone "
                              "texture on Metal, so declare a Texture and a SamplerState in its "
                              "place"
                            : "";
                    return std::unexpected(fmt::format(
                        "shader '{}': set {} binding {} '{}' takes {} argument-buffer element(s) "
                        "from {} in the generated Metal source but {} from {} in reflection, so "
                        "it and every field after it would bind at the wrong element{}",
                        shaderName, set, binding.Binding, binding.Name, emitted, element, expected,
                        binding.MetalIndex, hint));
                }
                element += emitted;
            }
        }
        return {};
    }

    optional<string> FindMetalEntryPoint(const std::string_view source)
    {
        for (const std::string_view attribute : {"[[kernel]]", "[[vertex]]", "[[fragment]]"})
        {
            const usize at = source.find(attribute);
            if (at == std::string_view::npos)
            {
                continue;
            }
            const usize paren = source.find('(', at);
            if (paren == std::string_view::npos)
            {
                return std::nullopt;
            }
            usize end = paren;
            while (end > at && std::isspace(static_cast<unsigned char>(source[end - 1])) != 0)
            {
                --end;
            }
            usize begin = end;
            while (begin > at &&
                   (std::isalnum(static_cast<unsigned char>(source[begin - 1])) != 0 ||
                    source[begin - 1] == '_'))
            {
                --begin;
            }
            if (begin == end)
            {
                return std::nullopt;
            }
            return string(source.substr(begin, end - begin));
        }
        return std::nullopt;
    }

    Result<vector<u8>> CompileMetalLibrary(const std::string_view source,
                                           const std::string_view label)
    {
#if defined(__APPLE__)
        static std::atomic<u32> counter{0};
        std::error_code ec;
        const path dir = std::filesystem::temp_directory_path(ec) /
                         fmt::format("veng-metal-{}-{}", getpid(), counter.fetch_add(1));
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            return std::unexpected(fmt::format("shader '{}': cannot create '{}': {}", label,
                                               dir.string(), ec.message()));
        }
        const auto cleanup = [&dir]()
        {
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        };

        const path metalPath = dir / "shader.metal";
        const path airPath = dir / "shader.air";
        const path libraryPath = dir / "shader.metallib";
        {
            std::ofstream out(metalPath, std::ios::binary);
            out.write(source.data(), static_cast<std::streamsize>(source.size()));
            if (!out)
            {
                cleanup();
                return std::unexpected(
                    fmt::format("shader '{}': cannot write '{}'", label, metalPath.string()));
            }
        }

        string output;
        if (RunProcess({"xcrun", "-sdk", "macosx", "metal", "-std=metal4.0",
                        "-fmetal-math-mode=fast", "-c", metalPath.string(), "-o", airPath.string()},
                       output) != 0)
        {
            cleanup();
            return std::unexpected(
                fmt::format("shader '{}': the Metal compiler rejected it:\n{}", label, output));
        }
        output.clear();
        if (RunProcess({"xcrun", "-sdk", "macosx", "metallib", airPath.string(), "-o",
                        libraryPath.string()},
                       output) != 0)
        {
            cleanup();
            return std::unexpected(fmt::format("shader '{}': metallib failed:\n{}", label, output));
        }

        std::ifstream in(libraryPath, std::ios::binary);
        vector<u8> library((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        cleanup();
        if (library.empty())
        {
            return std::unexpected(fmt::format("shader '{}': metallib wrote no library", label));
        }
        return library;
#else
        return std::unexpected(fmt::format(
            "shader '{}': the Metal toolchain exists only on Apple platforms ({} bytes of MSL "
            "were not compiled)",
            label, source.size()));
#endif
    }
}
