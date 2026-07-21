#pragma once

#include <Veng/Net/AccountId.h>
#include <Veng/Net/Blob.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

namespace Veng
{
    /// @brief The consumer hooks the local account store mints and validates ids through.
    ///
    /// Both are optional; the defaults mint a random id and accept any nonzero one. A consumer
    /// whose ids carry an invariant of their own (a scheme version, reserved bits, a derivation)
    /// supplies both, so the engine's AccountId stays opaque in both directions.
    struct LocalAccountInfo
    {
        /// @brief Mints a fresh id when the root holds no adoptable record.
        ///
        /// Unset mints with Net::GenerateAccountId. A hook returning an id its own ValidateId
        /// would reject is a consumer bug the store does not police.
        function<Net::AccountId()> MintId;
        /// @brief Validates an id read off disk; a rejected id is preserved and replaced, never
        ///        adopted.
        ///
        /// Unset accepts any valid (nonzero) id. AccountId::IsValid is a non-static member and does
        /// not itself convert to this signature.
        function<bool(Net::AccountId)> ValidateId;
    };

    /// @brief The durable local identity of record: a stable account id at a consumer-supplied
    ///        root, with an opaque consumer-defined profile beside it.
    ///
    /// Load-or-mint over one small binary file, `<root>/account`: an existing record is adopted, an
    /// absent one is minted and written before Open returns, so the id a consumer hands to
    /// Net::GameNetInfo::Identity is durable from the moment it exists. The store interprets
    /// exactly one field — the id. Everything else an application keeps on an account lives in the
    /// profile blob, which the store persists and returns verbatim and never decodes.
    ///
    /// The id is treated as irreplaceable, because it is: sessions key on it and a consumer's
    /// record families will too, so re-minting on surprise would orphan every account-keyed record
    /// on disk. Hence three distinct outcomes rather than one — an unreadable or rejected record is
    /// preserved as `<root>/account.corrupt` *before* a replacement is minted (and the open fails
    /// outright when those bytes cannot be preserved), a record written by a newer format version
    /// refuses the open rather than being overwritten, and a failed mint-write is a returned error
    /// rather than a store reporting durability it does not have.
    ///
    /// Open holds an exclusive advisory lock on `<root>/account.lock` for the store's lifetime, so
    /// a second process of one application cannot both mint over the other's record and publish an
    /// id that will not survive the next launch. The store resolves no root of its own;
    /// Platform/UserPaths.h is the natural provider of a per-user one. Where a consumer roots the
    /// account file alongside save slots, the name `account` is reserved against slot names.
    class VE_API LocalAccountStore
    {
    public:
        /// @brief The account record's file name within the root; reserved against slot names.
        ///
        /// The store's other two files extend it — `account.lock` and `account.corrupt` — so
        /// reserving this one name, together with the slot enumeration's non-directory skip, keeps
        /// a slot from ever resolving onto one of them.
        static constexpr string_view FileName = "account";

        /// @brief Opens the account record at a root, minting and persisting one when absent.
        ///
        /// Creates the root when absent, takes the exclusive account lock, and adopts the stored
        /// id when the record parses and passes ValidateId. An unreadable or rejected record is
        /// renamed to `<root>/account.corrupt` and replaced by a freshly minted, immediately
        /// written id — a success, reported through WasIdentityReset.
        /// @param root  The directory the account record lives in; created when absent.
        /// @param info  The mint/validate hooks; defaults mint randomly and accept any valid id.
        /// @return The opened store, or a recoverable error: an unwritable root, a failed
        /// mint-write, a lock another process holds, a record whose bytes could not be preserved,
        /// or a record written by a newer format version.
        [[nodiscard]] static Result<LocalAccountStore> Open(const path& root,
                                                            LocalAccountInfo info = {});

        /// @brief Mints an id held in memory only: nothing is read, written, or locked.
        ///
        /// The zero-config posture, requested by name rather than inferred from an empty root — a
        /// consumer whose root resolution failed gets an error from Open, not an identity that
        /// silently evaporates on exit.
        /// @param info  The mint/validate hooks; only MintId is consulted.
        /// @return An ephemeral store.
        [[nodiscard]] static LocalAccountStore Ephemeral(LocalAccountInfo info = {});

        /// @brief Releases the account lock and drops the in-memory record.
        ~LocalAccountStore();

        LocalAccountStore(const LocalAccountStore&) = delete;
        LocalAccountStore& operator=(const LocalAccountStore&) = delete;

        /// @brief Move-constructs, transferring the held lock.
        LocalAccountStore(LocalAccountStore&&) noexcept;
        /// @brief Move-assigns, releasing this store's lock and transferring the other's.
        LocalAccountStore& operator=(LocalAccountStore&&) noexcept;

        /// @brief Returns the account id this root presents — the value an Identity hook returns.
        [[nodiscard]] Net::AccountId GetId() const;

        /// @brief Returns whether the store is memory-only (nothing on disk, SetProfile a no-op).
        [[nodiscard]] bool IsEphemeral() const;

        /// @brief Returns whether Open replaced an unreadable or rejected record with a fresh id.
        ///
        /// True means the machine's previous identity is gone and its account-keyed records are
        /// orphaned; the bytes that were there are preserved at `<root>/account.corrupt`. A
        /// consumer surfaces this rather than letting a player discover it through empty records.
        [[nodiscard]] bool WasIdentityReset() const;

        /// @brief Returns the consumer-defined profile persisted beside the id; empty when never
        ///        set.
        ///
        /// Opaque end to end: the store keeps the type tag and the bytes as given and never reads
        /// past them. The reflection record encoding is the recommended codec, its tolerant read
        /// being what lets a consumer's profile fields accrete without a format break here.
        [[nodiscard]] const Net::Blob& GetProfile() const;

        /// @brief Replaces the profile and persists the record immediately; a no-op when ephemeral.
        ///
        /// The whole record is rewritten atomically, so a crash mid-write leaves the previous
        /// record — id included — byte-identical. An ephemeral store has no record to update, so
        /// the call succeeds having changed nothing and GetProfile stays empty.
        /// @param profile  The opaque profile to store.
        /// @return Empty on success, or a recoverable error describing the failed write, after
        /// which GetProfile still returns the profile that is durable.
        VoidResult SetProfile(Net::Blob profile);

    private:
        struct State;

        explicit LocalAccountStore(Unique<State> state);

        Unique<State> m_State;
    };
}
