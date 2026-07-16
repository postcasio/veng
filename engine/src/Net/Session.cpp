#include <Veng/Net/Session.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <cstring>
#include <unordered_map>

namespace Veng::Net
{
    vector<std::byte> EncodeSessionRecord(const SessionRecord& record, const TypeRegistry& registry)
    {
        vector<u8> bytes;
        WriteFields(bytes, &record, registry.Info(TypeIdOf<SessionRecord>()), registry);
        vector<std::byte> blob(bytes.size());
        std::memcpy(blob.data(), bytes.data(), bytes.size());
        return blob;
    }

    Result<SessionRecord> DecodeSessionRecord(const std::span<const std::byte> blob,
                                              const TypeRegistry& registry)
    {
        SessionRecord record;
        const std::span<const u8> bytes{reinterpret_cast<const u8*>(blob.data()), blob.size()};
        const VoidResult read =
            ReadFields(bytes, &record, registry.Info(TypeIdOf<SessionRecord>()), registry);
        if (!read)
        {
            return std::unexpected(read.error());
        }
        return record;
    }

    struct SessionRegistry::State
    {
        SessionRegistryInfo Info;

        struct Entry
        {
            SessionRecord Record;
            bool Dirty = false;
        };

        std::unordered_map<AccountId, Entry> Records;

        // The last checkpoint save's timestamp; the first checkpoint measures from process start.
        f64 LastCheckpoint = 0.0;

        [[nodiscard]] Entry* FindEntry(const AccountId& account)
        {
            const auto it = Records.find(account);
            return it != Records.end() ? &it->second : nullptr;
        }

        Entry& EnsureEntry(const AccountId& account)
        {
            Entry& entry = Records[account];
            entry.Record.Account = account;
            return entry;
        }

        // Whether a payload's type id can be interpreted by the schema registry. An empty type id
        // (no payload shape declared) is trusted — there is nothing to interpret.
        [[nodiscard]] bool IsPayloadTypeKnown(const TravelPayload& payload) const
        {
            return payload.Type == InvalidTypeId || Info.Types->IsRegistered(payload.Type);
        }

        void SaveEntry(Entry& entry)
        {
            if (Info.SaveSession)
            {
                Info.SaveSession(entry.Record.Account,
                                 EncodeSessionRecord(entry.Record, *Info.Types));
            }
            entry.Dirty = false;
        }
    };

    SessionRegistry::SessionRegistry(Unique<State> state) : m_State(std::move(state)) {}

    SessionRegistry::~SessionRegistry() = default;

    Unique<SessionRegistry> SessionRegistry::Create(const SessionRegistryInfo& info)
    {
        VE_ASSERT(info.Types != nullptr, "SessionRegistry requires a type registry");
        auto state = CreateUnique<State>();
        state->Info = info;
        return Unique<SessionRegistry>(new SessionRegistry(std::move(state)));
    }

    void SessionRegistry::EnsureLoaded(const AccountId& account)
    {
        State& s = *m_State;
        if (!account.IsValid() || s.Records.contains(account) || !s.Info.LoadSession)
        {
            return;
        }

        const optional<vector<std::byte>> blob = s.Info.LoadSession(account);
        if (!blob.has_value())
        {
            return;
        }
        Result<SessionRecord> decoded = DecodeSessionRecord(*blob, *s.Info.Types);
        if (!decoded)
        {
            Log::Warn("SessionRegistry: persisted record for account {:016X}{:016X} is "
                      "undecodable ({}); treating as none",
                      account.Hi, account.Lo, decoded.error());
            return;
        }
        State::Entry& entry = s.EnsureEntry(account);
        entry.Record = std::move(*decoded);
        entry.Record.Account = account;
    }

    const SessionRecord* SessionRegistry::Find(const AccountId& account) const
    {
        const auto it = m_State->Records.find(account);
        return it != m_State->Records.end() ? &it->second.Record : nullptr;
    }

    void SessionRegistry::RecordStandingJoin(const AccountId& account, const WorldKey& key)
    {
        if (!account.IsValid())
        {
            return;
        }
        State::Entry& entry = m_State->EnsureEntry(account);
        for (const WorldKey& held : entry.Record.StandingJoins)
        {
            if (held == key)
            {
                return;
            }
        }
        entry.Record.StandingJoins.push_back(key);
        entry.Dirty = true;
    }

    void SessionRegistry::RemoveStandingJoin(const AccountId& account, const WorldKey& key)
    {
        State::Entry* entry = m_State->FindEntry(account);
        if (entry == nullptr)
        {
            return;
        }
        const usize erased = std::erase(entry->Record.StandingJoins, key);
        if (erased > 0)
        {
            entry->Dirty = true;
        }
    }

    void SessionRegistry::RecordGameplay(const AccountId& account, const WorldKey& key,
                                         const TravelPayload& params, const TravelPayload& pose)
    {
        if (!account.IsValid())
        {
            return;
        }
        State::Entry& entry = m_State->EnsureEntry(account);
        entry.Record.Gameplay = SessionGameplayEntry{.Key = key, .Params = params, .Pose = pose};
        entry.Dirty = true;
    }

    void SessionRegistry::CaptureGameplayPose(const AccountId& account, const WorldInstanceId world,
                                              const Entity seat)
    {
        State& s = *m_State;
        if (!s.Info.CaptureTravelPose)
        {
            return;
        }
        State::Entry* entry = s.FindEntry(account);
        if (entry == nullptr || entry->Record.Gameplay.Key == WorldKey{})
        {
            return;
        }
        entry->Record.Gameplay.Pose = s.Info.CaptureTravelPose(world, seat);
        entry->Dirty = true;
    }

    void SessionRegistry::ClearGameplay(const AccountId& account)
    {
        State::Entry* entry = m_State->FindEntry(account);
        if (entry == nullptr)
        {
            return;
        }
        entry->Record.Gameplay = SessionGameplayEntry{};
        entry->Dirty = true;
    }

    optional<SessionRecord> SessionRegistry::BeginReattach(const AccountId& account)
    {
        State& s = *m_State;
        State::Entry* entry = s.FindEntry(account);
        if (entry == nullptr)
        {
            return std::nullopt;
        }

        // The consumer's policy transform rewrites the record, and the rewrite is kept — the
        // record after reattach is what the transform said, not what was stored.
        if (s.Info.TransformOnReattach)
        {
            entry->Record = s.Info.TransformOnReattach(std::move(entry->Record));
            entry->Record.Account = account;
            entry->Dirty = true;
        }

        // Persisted blobs are untrusted at reattach: a gameplay entry whose payloads carry a type
        // id the registry cannot interpret is a resolve failure — cleared, so the account lands at
        // its front door. Deeper semantic validation is the transform hook's.
        if (!(entry->Record.Gameplay.Key == WorldKey{}) &&
            (!s.IsPayloadTypeKnown(entry->Record.Gameplay.Params) ||
             !s.IsPayloadTypeKnown(entry->Record.Gameplay.Pose)))
        {
            Log::Warn("SessionRegistry: account {:016X}{:016X} gameplay entry carries an "
                      "unregistered payload type; clearing to the front door",
                      account.Hi, account.Lo);
            entry->Record.Gameplay = SessionGameplayEntry{};
            entry->Dirty = true;
        }

        return entry->Record;
    }

    void SessionRegistry::Save(const AccountId& account)
    {
        if (State::Entry* entry = m_State->FindEntry(account))
        {
            m_State->SaveEntry(*entry);
        }
    }

    void SessionRegistry::SaveAll()
    {
        for (auto& [account, entry] : m_State->Records)
        {
            if (entry.Dirty)
            {
                m_State->SaveEntry(entry);
            }
        }
    }

    void SessionRegistry::Checkpoint(const f64 now, const function<void()>& refresh)
    {
        State& s = *m_State;
        if (now - s.LastCheckpoint < s.Info.SaveDebounceSeconds)
        {
            return;
        }
        bool anyDirty = false;
        for (const auto& [account, entry] : s.Records)
        {
            if (entry.Dirty)
            {
                anyDirty = true;
                break;
            }
        }
        if (!anyDirty)
        {
            return;
        }
        s.LastCheckpoint = now;
        if (refresh)
        {
            refresh();
        }
        SaveAll();
    }

    usize SessionRegistry::Count() const
    {
        return m_State->Records.size();
    }
}
