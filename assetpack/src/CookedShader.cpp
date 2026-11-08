#include <Veng/Asset/CookedShader.h>

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace Veng
{
    namespace
    {
        template <usize N>
        void SetName(char (&dest)[N], std::string_view name)
        {
            const usize n = std::min(name.size(), N - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }

        template <usize N>
        string ReadName(const char (&name)[N])
        {
            return string(name, strnlen(name, N));
        }

        bool IsKnownCode(const u32 code)
        {
            return code == static_cast<u32>(CookedShaderCode::SpirV) ||
                   code == static_cast<u32>(CookedShaderCode::MetalLibrary);
        }
    }

    vector<u8> BuildCookedShaderBlob(CookedShaderInterfaceHeader interfaceHeader,
                                     const std::span<const CookedDescriptorBinding> bindings,
                                     const std::span<const CookedPushConstantBlock> pushConstants,
                                     const std::span<const CookedShaderSectionInput> sections)
    {
        interfaceHeader.BindingCount = static_cast<u32>(bindings.size());
        interfaceHeader.PushConstantCount = static_cast<u32>(pushConstants.size());

        const usize bindingBytes = bindings.size_bytes();
        const usize pushConstantBytes = pushConstants.size_bytes();
        const usize interfaceBytes =
            sizeof(CookedShaderInterfaceHeader) + bindingBytes + pushConstantBytes;
        const usize tableBytes = sections.size() * sizeof(CookedShaderSection);

        usize codeBytes = 0;
        for (const CookedShaderSectionInput& section : sections)
        {
            codeBytes += section.Bytes.size();
        }

        const CookedShaderHeader header{
            .Version = CookedShaderVersion,
            .InterfaceBytes = static_cast<u32>(interfaceBytes),
            .SectionCount = static_cast<u32>(sections.size()),
        };

        vector<u8> blob(sizeof(CookedShaderHeader) + tableBytes + interfaceBytes + codeBytes);
        std::memcpy(blob.data(), &header, sizeof(header));

        usize code = sizeof(CookedShaderHeader) + tableBytes + interfaceBytes;
        for (usize i = 0; i < sections.size(); ++i)
        {
            CookedShaderSection entry{
                .Code = static_cast<u32>(sections[i].Code),
                .Offset = static_cast<u32>(code),
                .Size = static_cast<u32>(sections[i].Bytes.size()),
            };
            SetName(entry.EntryPoint, sections[i].EntryPoint);
            std::memcpy(blob.data() + sizeof(CookedShaderHeader) + i * sizeof(CookedShaderSection),
                        &entry, sizeof(entry));
            if (!sections[i].Bytes.empty())
            {
                std::memcpy(blob.data() + code, sections[i].Bytes.data(), sections[i].Bytes.size());
            }
            code += sections[i].Bytes.size();
        }

        usize cursor = sizeof(CookedShaderHeader) + tableBytes;
        std::memcpy(blob.data() + cursor, &interfaceHeader, sizeof(interfaceHeader));
        cursor += sizeof(interfaceHeader);
        if (bindingBytes != 0)
        {
            std::memcpy(blob.data() + cursor, bindings.data(), bindingBytes);
        }
        cursor += bindingBytes;
        if (pushConstantBytes != 0)
        {
            std::memcpy(blob.data() + cursor, pushConstants.data(), pushConstantBytes);
        }

        return blob;
    }

    Result<CookedShaderView> ParseCookedShader(const std::span<const u8> blob)
    {
        if (blob.size() < sizeof(CookedShaderHeader))
        {
            return std::unexpected("cooked blob smaller than CookedShaderHeader");
        }

        CookedShaderHeader header;
        std::memcpy(&header, blob.data(), sizeof(header));
        if (header.Version != CookedShaderVersion)
        {
            return std::unexpected(fmt::format("cooked shader version {} is not the supported {}",
                                               header.Version, CookedShaderVersion));
        }
        if (header.SectionCount == 0)
        {
            return std::unexpected("cooked shader carries no compiled code");
        }

        usize cursor = sizeof(CookedShaderHeader);
        const usize tableBytes =
            static_cast<usize>(header.SectionCount) * sizeof(CookedShaderSection);
        if (blob.size() < cursor + tableBytes)
        {
            return std::unexpected("cooked blob smaller than its section table");
        }

        CookedShaderView view;
        view.Sections.reserve(header.SectionCount);
        for (u32 i = 0; i < header.SectionCount; ++i)
        {
            CookedShaderSection entry;
            std::memcpy(&entry, blob.data() + cursor + i * sizeof(CookedShaderSection),
                        sizeof(entry));
            if (!IsKnownCode(entry.Code))
            {
                return std::unexpected(
                    fmt::format("section {} has unrecognized code kind {}", i, entry.Code));
            }
            if (static_cast<usize>(entry.Offset) + entry.Size > blob.size())
            {
                return std::unexpected(
                    fmt::format("section {} ({} bytes at offset {}) runs past the {}-byte blob", i,
                                entry.Size, entry.Offset, blob.size()));
            }
            view.Sections.push_back(CookedShaderSectionView{
                .Code = static_cast<CookedShaderCode>(entry.Code),
                .Bytes = blob.subspan(entry.Offset, entry.Size),
                .EntryPoint = ReadName(entry.EntryPoint),
            });
        }
        cursor += tableBytes;

        if (blob.size() < cursor + sizeof(CookedShaderInterfaceHeader))
        {
            return std::unexpected("cooked blob smaller than CookedShaderInterfaceHeader");
        }
        std::memcpy(&view.Interface, blob.data() + cursor, sizeof(view.Interface));
        cursor += sizeof(CookedShaderInterfaceHeader);

        const usize bindingBytes =
            static_cast<usize>(view.Interface.BindingCount) * sizeof(CookedDescriptorBinding);
        const usize pushConstantBytes =
            static_cast<usize>(view.Interface.PushConstantCount) * sizeof(CookedPushConstantBlock);
        if (blob.size() < cursor + bindingBytes + pushConstantBytes)
        {
            return std::unexpected("cooked blob smaller than its binding and push-constant tables");
        }
        if (header.InterfaceBytes !=
            sizeof(CookedShaderInterfaceHeader) + bindingBytes + pushConstantBytes)
        {
            return std::unexpected(fmt::format(
                "InterfaceBytes {} does not match the reflected interface size {}",
                header.InterfaceBytes,
                sizeof(CookedShaderInterfaceHeader) + bindingBytes + pushConstantBytes));
        }

        view.Bindings.resize(view.Interface.BindingCount);
        if (bindingBytes != 0)
        {
            std::memcpy(view.Bindings.data(), blob.data() + cursor, bindingBytes);
        }
        cursor += bindingBytes;
        view.PushConstants.resize(view.Interface.PushConstantCount);
        if (pushConstantBytes != 0)
        {
            std::memcpy(view.PushConstants.data(), blob.data() + cursor, pushConstantBytes);
        }

        return view;
    }

    Result<const CookedShaderSectionView*> FindCookedShaderSection(const CookedShaderView& shader,
                                                                   const CookedShaderCode code,
                                                                   const std::string_view assetName)
    {
        string present;
        for (const CookedShaderSectionView& section : shader.Sections)
        {
            if (section.Code == code)
            {
                return &section;
            }
            present += present.empty() ? "" : ", ";
            present += CookedShaderCodeName(section.Code);
        }
        return std::unexpected(fmt::format(
            "shader '{}' carries no {} code (it was cooked for: {}); recook its pack for this "
            "build's GPU backend",
            assetName, CookedShaderCodeName(code), present));
    }

    std::string_view CookedShaderCodeName(const CookedShaderCode code)
    {
        switch (code)
        {
        case CookedShaderCode::SpirV:
            return "SPIR-V";
        case CookedShaderCode::MetalLibrary:
            return "Metal library";
        }
        return "unknown";
    }
}
