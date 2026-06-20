#include <Veng/Reflection/Serialize.h>

#include <Veng/Assert.h>
#include <Veng/Scene/Entity.h>

#include <cstring>

namespace Veng
{
    namespace
    {
        Result<usize> ReadFieldsInner(std::span<const u8> in, void* obj, const TypeInfo& type,
                                      const TypeRegistry& registry);

        // Raw append/read in host byte order — this is an in-memory seam, not an on-disk format.

        void AppendBytes(vector<u8>& out, const void* src, usize size)
        {
            const auto* p = static_cast<const u8*>(src);
            out.insert(out.end(), p, p + size);
        }

        void AppendU32(vector<u8>& out, u32 value)
        {
            AppendBytes(out, &value, sizeof(value));
        }

        Result<u32> ReadU32(std::span<const u8> in, usize& cursor)
        {
            if (cursor + sizeof(u32) > in.size())
                return std::unexpected("ReadFields: truncated u32");
            u32 value = 0;
            std::memcpy(&value, in.data() + cursor, sizeof(value));
            cursor += sizeof(value);
            return value;
        }

        // Writes a leaf/struct field's value bytes (no name, no length) for the
        // field at obj+offset, branching on FieldClass.
        void WriteValue(vector<u8>& out, const void* fieldPtr, const FieldDescriptor& field,
                        const TypeRegistry& registry)
        {
            switch (field.Class)
            {
            case FieldClass::Scalar:
            case FieldClass::Vector:
            case FieldClass::Quaternion:
            case FieldClass::Matrix:
            case FieldClass::Enum:
            {
                // Trivially-copyable, fixed-size leaves — copy Size raw bytes.
                const usize size = registry.Info(field.Type).Size;
                AppendBytes(out, fieldPtr, size);
                break;
            }
            case FieldClass::String:
            {
                const auto& value = *static_cast<const string*>(fieldPtr);
                AppendU32(out, static_cast<u32>(value.size()));
                AppendBytes(out, value.data(), value.size());
                break;
            }
            case FieldClass::AssetHandle:
            {
                // The underlying AssetId is the first member of AssetHandle<T>
                // (an AssetId, itself a leading u64). Rehydration to a resident
                // handle is the deferred loader's job; only the id is recorded.
                u64 id = 0;
                std::memcpy(&id, fieldPtr, sizeof(id));
                AppendBytes(out, &id, sizeof(id));
                break;
            }
            case FieldClass::Reference:
            {
                const auto& entity = *static_cast<const Entity*>(fieldPtr);
                AppendU32(out, entity.Index);
                AppendU32(out, entity.Generation);
                break;
            }
            case FieldClass::Struct:
            {
                const TypeInfo& nested = registry.Info(field.Type);
                WriteFields(out, fieldPtr, nested, registry);
                break;
            }
            }
        }

        // Reads a field's value from in[cursor..] into obj+offset. The byte count
        // consumed must match WriteValue exactly. A value that runs past the end of
        // `in` is a recoverable error; a descriptor naming an unregistered type is
        // a fatal schema fault.
        VoidResult ReadValue(std::span<const u8> in, usize& cursor, void* fieldPtr,
                             const FieldDescriptor& field, const TypeRegistry& registry)
        {
            switch (field.Class)
            {
            case FieldClass::Scalar:
            case FieldClass::Vector:
            case FieldClass::Quaternion:
            case FieldClass::Matrix:
            case FieldClass::Enum:
            {
                const usize size = registry.Info(field.Type).Size;
                if (cursor + size > in.size())
                    return std::unexpected("ReadFields: truncated leaf");
                std::memcpy(fieldPtr, in.data() + cursor, size);
                cursor += size;
                break;
            }
            case FieldClass::String:
            {
                const Result<u32> length = ReadU32(in, cursor);
                if (!length)
                    return std::unexpected(length.error());
                if (cursor + *length > in.size())
                    return std::unexpected("ReadFields: truncated string");
                auto& value = *static_cast<string*>(fieldPtr);
                value.assign(reinterpret_cast<const char*>(in.data() + cursor), *length);
                cursor += *length;
                break;
            }
            case FieldClass::AssetHandle:
            {
                if (cursor + sizeof(u64) > in.size())
                    return std::unexpected("ReadFields: truncated asset id");
                u64 id = 0;
                std::memcpy(&id, in.data() + cursor, sizeof(id));
                cursor += sizeof(id);
                // Write the id back into the handle's leading AssetId; the
                // cache entry stays null (no AssetManager rehydration here).
                std::memcpy(fieldPtr, &id, sizeof(id));
                break;
            }
            case FieldClass::Reference:
            {
                auto& entity = *static_cast<Entity*>(fieldPtr);
                const Result<u32> index = ReadU32(in, cursor);
                if (!index)
                    return std::unexpected(index.error());
                const Result<u32> generation = ReadU32(in, cursor);
                if (!generation)
                    return std::unexpected(generation.error());
                entity.Index = *index;
                entity.Generation = *generation;
                break;
            }
            case FieldClass::Struct:
            {
                const TypeInfo& nested = registry.Info(field.Type);
                // The nested record is already isolated to its value bytes; recurse
                // and advance cursor by what it consumed.
                const Result<usize> consumed =
                    ReadFieldsInner(in.subspan(cursor), fieldPtr, nested, registry);
                if (!consumed)
                    return std::unexpected(consumed.error());
                cursor += *consumed;
                break;
            }
            }

            return {};
        }

        // Core reader: walks a value's records from the start of `in`, returns the
        // number of bytes consumed (so a nested struct can advance its parent).
        Result<usize> ReadFieldsInner(std::span<const u8> in, void* obj, const TypeInfo& type,
                                      const TypeRegistry& registry)
        {
            usize cursor = 0;
            const Result<u32> recordCount = ReadU32(in, cursor);
            if (!recordCount)
                return std::unexpected(recordCount.error());

            for (u32 i = 0; i < *recordCount; ++i)
            {
                const Result<u32> nameLen = ReadU32(in, cursor);
                if (!nameLen)
                    return std::unexpected(nameLen.error());
                if (cursor + *nameLen > in.size())
                    return std::unexpected("ReadFields: truncated field name");
                const string_view name(reinterpret_cast<const char*>(in.data() + cursor), *nameLen);
                cursor += *nameLen;

                const Result<u32> valueLen = ReadU32(in, cursor);
                if (!valueLen)
                    return std::unexpected(valueLen.error());
                if (cursor + *valueLen > in.size())
                    return std::unexpected("ReadFields: truncated field value");

                // No matching descriptor → skip (schema drift tolerance).
                const FieldDescriptor* match = nullptr;
                for (const FieldDescriptor& field : type.Fields)
                {
                    if (field.Name.size() == name.size() &&
                        std::memcmp(field.Name.data(), name.data(), name.size()) == 0)
                    {
                        match = &field;
                        break;
                    }
                }

                if (match != nullptr)
                {
                    void* fieldPtr = static_cast<u8*>(obj) + match->Offset;
                    usize valueCursor = cursor;
                    if (VoidResult read = ReadValue(in.subspan(0, cursor + *valueLen), valueCursor,
                                                    fieldPtr, *match, registry);
                        !read)
                        return std::unexpected(read.error());
                }

                cursor += *valueLen;
            }

            return cursor;
        }
    }

    void WriteFields(vector<u8>& out, const void* obj, const TypeInfo& type,
                     const TypeRegistry& registry)
    {
        AppendU32(out, static_cast<u32>(type.Fields.size()));

        for (const FieldDescriptor& field : type.Fields)
        {
            const void* fieldPtr = static_cast<const u8*>(obj) + field.Offset;

            // name record
            AppendU32(out, static_cast<u32>(field.Name.size()));
            AppendBytes(out, field.Name.data(), field.Name.size());

            // value, length-prefixed so an unknown record can be skipped on read
            const usize lengthSlot = out.size();
            AppendU32(out, 0);
            const usize valueStart = out.size();
            WriteValue(out, fieldPtr, field, registry);
            const u32 valueLen = static_cast<u32>(out.size() - valueStart);
            std::memcpy(out.data() + lengthSlot, &valueLen, sizeof(valueLen));
        }
    }

    VoidResult ReadFields(std::span<const u8> in, void* obj, const TypeInfo& type,
                          const TypeRegistry& registry)
    {
        Result<usize> consumed = ReadFieldsInner(in, obj, type, registry);
        if (!consumed)
            return std::unexpected(consumed.error());
        return {};
    }
}
