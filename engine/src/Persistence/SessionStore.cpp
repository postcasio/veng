#include <Veng/Persistence/SessionStore.h>

#include <Veng/Log.h>
#include <Veng/Net/Session.h>
#include <Veng/Reflection/TypeId.h>

#include <utility>

namespace Veng
{
    namespace
    {
        // The record's key: both halves of the account id, verbatim. Keying on the low word alone
        // would collapse two accounts differing only in the high half onto one record.
        StoreKey KeyOf(const Net::AccountId& account)
        {
            return StoreKey{.Lo = account.Lo, .Hi = account.Hi};
        }
    }

    void RegisterSessionFamily(Store& store)
    {
        if (store.IsFamilyRegistered(SessionsFamily))
        {
            return;
        }
        store.RegisterFamily(
            StoreFamily{.Id = SessionsFamily, .FileStem = string(SessionsFileStem), .Version = 1});
    }

    SessionHooks MakeSessionHooks(function<Store*()> storeSource, const SessionStoreInfo& info)
    {
        SessionHooks hooks;
        hooks.LoadSession =
            [storeSource](const Net::AccountId account) -> optional<vector<std::byte>>
        {
            Store* const store = storeSource ? storeSource() : nullptr;
            if (store == nullptr)
            {
                return std::nullopt;
            }
            const optional<StoreRecord> record = store->Read(SessionsFamily, KeyOf(account));
            if (!record.has_value() || record->Components.empty())
            {
                return std::nullopt;
            }
            // Stored verbatim as one blob: the bytes are already the record's reflection-binary
            // encoding, which the registry decodes tolerantly.
            const vector<u8>& bytes = record->Components.front().Bytes;
            const auto* const first = reinterpret_cast<const std::byte*>(bytes.data());
            return vector<std::byte>(first, first + bytes.size());
        };
        hooks.SaveSession = [storeSource = std::move(storeSource), flushOnSave = info.FlushOnSave](
                                const Net::AccountId account, const std::span<const std::byte> blob)
        {
            Store* const store = storeSource ? storeSource() : nullptr;
            if (store == nullptr)
            {
                return;
            }
            // The type tag is what identifies these bytes to a dump, an inspector, or a later
            // migration; the store does not re-model the record beyond carrying it.
            ComponentBlob component{.Type = TypeIdOf<Net::SessionRecord>()};
            const auto* const first = reinterpret_cast<const u8*>(blob.data());
            component.Bytes.assign(first, first + blob.size());
            StoreRecord record{.CapturedAtWall = Store::WallClockSeconds()};
            record.Components.push_back(std::move(component));
            store->Write(SessionsFamily, KeyOf(account), std::move(record));
            if (!flushOnSave)
            {
                return;
            }
            if (const VoidResult flushed = store->Flush(); !flushed)
            {
                Log::Error("session store: flushing a saved session record failed: {}",
                           flushed.error());
            }
        };
        return hooks;
    }
}
