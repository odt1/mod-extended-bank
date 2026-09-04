/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

#ifndef MOD_EXTENDED_BANK_H
#define MOD_EXTENDED_BANK_H

#include "ConfigValueCache.h"
#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Item;
class Player;

// Vault 1 is the vanilla bank and always exists; it is never stored in
// `mod_extended_bank_vault_items`.
constexpr uint8 EXTENDED_BANK_DEFAULT_VAULT = 1;
constexpr uint8 EXTENDED_BANK_VAULT_LIMIT = 20;

// How often the "player walked away from the banker" check runs, in milliseconds.
constexpr uint32 EXTENDED_BANK_RANGE_CHECK_INTERVAL = 1000;

constexpr uint32 EXTENDED_BANK_COPPER_PER_GOLD = 10000;

// Longest vault name accepted, in characters (not bytes -- utf8truncate counts characters,
// so this holds for any encoding the client can send).
//
// The client itself imposes no reachable limit on a gossip option string, and neither does
// the core: PlayerMenu::SendGossipMenu writes the std::string straight into
// SMSG_GOSSIP_MESSAGE. The practical ceiling is what Blizzard shipped -- the longest
// OptionText in 3.3.5a's own `gossip_menu_option` data is 397 characters (the longest
// BoxText, 102). This limit sits below that with room to spare for the rename prompt, which
// embeds the name in "Enter a new name for {}:", and below the VARCHAR(255) `name` column.
constexpr std::size_t EXTENDED_BANK_VAULT_NAME_MAX_CHARS = 240;

// Highest per-vault price accepted from the config, in gold. MAX_MONEY_AMOUNT is 0x7FFFFFFF
// copper, so anything above this cannot be paid anyway, and multiplying it by
// EXTENDED_BANK_COPPER_PER_GOLD would wrap a uint32 and quietly make the vault cheap.
constexpr uint32 EXTENDED_BANK_MAX_VAULT_COST_GOLD = 214748;

enum class ExtendedBankSetting
{
    ENABLE,
    MAX_VAULTS,
    VAULT_COST,

    NUM_CONFIGS
};

class ExtendedBankConfig : public ConfigValueCache<ExtendedBankSetting>
{
public:
    ExtendedBankConfig() : ConfigValueCache(ExtendedBankSetting::NUM_CONFIGS) { }

    void BuildConfigCache() override;

    [[nodiscard]] bool IsEnabled() const { return GetConfigValue<bool>(ExtendedBankSetting::ENABLE); }
    [[nodiscard]] uint8 GetMaxVaults() const
    {
        std::lock_guard<std::mutex> guard(_configMutex);
        return _maxVaults;
    }

    // Purchase price of `vault` in copper. Only meaningful for vault >= 2.
    [[nodiscard]] uint32 GetVaultCost(uint8 vault) const;

private:
    // BuildConfigCache runs on .reload config while GetVaultCost/GetMaxVaults are being read
    // from gossip on other threads. The values are published under this lock rather than
    // mutated in place, so a reader can never observe a cleared vector or a reallocating one.
    mutable std::mutex _configMutex;
    uint8 _maxVaults{ 8 };
    std::vector<uint32> _vaultCost;
};

extern ExtendedBankConfig sExtendedBankConfig;

// Persisted metadata for one vault. Vault 1 gets a row too, but only so that its
// name and its purchased bag-slot count survive a relog.
struct ExtendedBankVault
{
    uint8 Vault{ 0 };
    std::string Name;
    uint8 BagSlots{ 0 };
};

// One entry of the layout that was last written to `mod_extended_bank_vault_items`.
// Kept so a flush can write only the rows that differ, and skip entirely when none do.
struct ExtendedBankPersistedPos
{
    ObjectGuid::LowType Item{ 0 };
    ObjectGuid::LowType Bag{ 0 };
    uint8 Slot{ 0 };
};

// Live session state, present only while a player has a vault other than the
// default one loaded into their real bank slots.
struct ExtendedBankSession
{
    uint8 ActiveVault{ EXTENDED_BANK_DEFAULT_VAULT };
    ObjectGuid BankerGuid;
    uint32 RangeCheckTimer{ 0 };

    // What the module last wrote for this vault, so DrainUpdateQueue and FlushActiveVault
    // can tell "nothing moved" from "needs rewriting" without querying.
    std::vector<ExtendedBankPersistedPos> PersistedLayout;
    uint8 PersistedBagSlots{ 0 };
};

// One item as it sits in a vault, mirroring `character_inventory`'s layout:
// Bag == 0 means Slot is a global player slot (39..73), otherwise Bag is the
// containing bag's item GUID and Slot is an index inside that bag.
struct ExtendedBankItemPos
{
    Item* ItemPtr{ nullptr };
    ObjectGuid::LowType Bag{ 0 };
    uint8 Slot{ 0 };
};

// What a flush has to write: the difference between the layout last persisted for the live
// vault and the layout the bank holds now. Computed once per flush in a single pass, so a
// drag inside a full vault costs two statements instead of one per item.
struct ExtendedBankLayoutDelta
{
    // Indices into the live item vector whose row has to be written -- the item is new to
    // this vault, or it moved.
    std::vector<std::size_t> Written;

    // A subset of Written: items that were not in this vault at the last flush, and so may
    // still carry a `character_inventory` row from wherever they came from.
    std::vector<std::size_t> Entered;

    // Item GUIDs the vault used to hold and no longer does.
    std::vector<ObjectGuid::LowType> Removed;

    bool BagSlotsChanged{ false };

    [[nodiscard]] bool Any() const
    {
        return !Written.empty() || !Removed.empty() || BagSlotsChanged;
    }
};

// Everything the debug command needs that is not otherwise observable from outside.
struct ExtendedBankDebugState
{
    bool HasSession{ false };

    // Whether the active vault has a `mod_extended_bank_vaults` row at all. A character who
    // has never bought or opened a vault has none, and the bag slot count read back for them
    // is 0 rather than whatever they have actually paid for.
    bool HasVaultRow{ false };

    uint8 ActiveVault{ EXTENDED_BANK_DEFAULT_VAULT };
    ObjectGuid BankerGuid;
    std::size_t PersistedItems{ 0 };
    uint8 PersistedBagSlots{ 0 };
};

class ExtendedBankMgr
{
public:
    static ExtendedBankMgr* instance();

    // --- lifecycle -------------------------------------------------------
    // Called from OnPlayerLoadFromDB, which runs before Player::_LoadInventory.
    void LoadPlayer(Player* player);
    void ForgetPlayer(ObjectGuid playerGuid);
    void DeleteCharacterData(CharacterDatabaseTransaction trans, ObjectGuid::LowType lowGuid);

    // --- queries ---------------------------------------------------------
    // Vault numbers this character owns, ascending, always starting with the default one.
    [[nodiscard]] std::vector<uint8> GetOwnedVaults(ObjectGuid playerGuid) const;
    [[nodiscard]] uint8 GetHighestOwnedVault(ObjectGuid playerGuid) const;
    [[nodiscard]] bool OwnsVault(ObjectGuid playerGuid, uint8 vault) const;
    [[nodiscard]] uint8 GetActiveVault(ObjectGuid playerGuid) const;
    [[nodiscard]] std::string GetVaultName(ObjectGuid playerGuid, uint8 vault) const;
    [[nodiscard]] uint8 GetVaultBagSlots(ObjectGuid playerGuid, uint8 vault) const;
    [[nodiscard]] ExtendedBankDebugState GetDebugState(ObjectGuid playerGuid) const;

    // Public so the debug command can report the live bank without duplicating the walk.
    void CollectLiveBankItems(Player* player, std::vector<ExtendedBankItemPos>& items) const;

    // --- actions ---------------------------------------------------------
    // Swaps `target`'s contents into the player's real bank slots and opens the stock bank
    // frame. Returns false when the switch is refused; a chat message has already been sent
    // in every case except an unowned vault, which can only come from a forged packet.
    bool OpenVault(Player* player, uint8 target, ObjectGuid bankerGuid);

    // Puts the default vault back into the bank slots without opening anything.
    void RevertToDefaultVault(Player* player);

    // Persists the live vault and detaches it without loading anything back. Used at logout,
    // where reloading the default vault's items would be thrown away microseconds later.
    void FlushAndDetachForLogout(Player* player);

    // Takes the live vault's items back out of Player::m_itemUpdateQueue, rewriting the vault
    // if anything moved. Must run before any code path that can reach Player::_SaveInventory,
    // which is not only Player::SaveToDB -- see Player::SaveInventoryAndGoldToDB.
    void DrainUpdateQueue(Player* player);

    // Writes the live bank layout of a non-default vault back to the module
    // tables and un-queues its items so the core never persists them into
    // `character_inventory`. Called from OnPlayerSave.
    void FlushActiveVault(Player* player);

    void UpdateRangeCheck(Player* player, uint32 diff);

    bool BuyNextVault(Player* player);
    // False when the character does not own that vault, which is the only way it can
    // fail. Nothing is written in that case.
    bool RenameVault(Player* player, uint8 vault, std::string const& name);

private:
    /* -- vault metadata, in ExtendedBankVaults.cpp ------------------------ */

    // Refills only the vault metadata list. Unlike LoadPlayer it leaves the session and the
    // live bank alone, so it is safe to call while a vault is open.
    void LoadVaultList(ObjectGuid playerGuid);
    void SelfHealVaultRows(ObjectGuid::LowType lowGuid);
    void EnsureDefaultVaultRow(Player* player);

    // Writes the live bank bag slot count into a vault's row when it has changed. Buying a
    // slot only calls Player::SetBankBagSlotCount, so without this the purchase is lost the
    // moment the player switches away from the vault they bought it on. Given a transaction
    // the write joins it; given none it goes out on its own.
    void SyncVaultBagSlots(Player* player, uint8 vault, CharacterDatabaseTransaction trans = nullptr);

    [[nodiscard]] ExtendedBankVault* FindVault(ObjectGuid playerGuid, uint8 vault);
    [[nodiscard]] ExtendedBankVault const* FindVault(ObjectGuid playerGuid, uint8 vault) const;

    /* -- the live bank, in ExtendedBankStorage.cpp ------------------------- */

    void SwitchTo(Player* player, uint8 target);
    void PersistOutgoingVault(Player* player, uint8 vault);

    void CaptureLayout(ExtendedBankSession& session, std::vector<ExtendedBankItemPos> const& items,
        uint8 bagSlots) const;

    // One O(n + m) pass replacing what used to be a separate order-sensitive comparison and a
    // nested "did anything leave" scan. A full vault is 28 slots plus seven 36-slot bags, so
    // the nested form ran into five figures of comparisons on every packet the player sent.
    [[nodiscard]] ExtendedBankLayoutDelta ComputeLayoutDelta(ExtendedBankSession const& session,
        std::vector<ExtendedBankItemPos> const& items, uint8 bagSlots) const;
    [[nodiscard]] static bool AnyItemQueued(std::vector<ExtendedBankItemPos> const& items);

    // Moves items the game caps per character out of a module-owned vault and back into the
    // player's bags. Returns true if anything was moved, in which case the caller must
    // re-collect the live bank.
    bool EvictRestrictedItems(Player* player, std::vector<ExtendedBankItemPos> const& items,
        CharacterDatabaseTransaction trans, ObjectGuid bankerGuid);

    void PersistVaultLayout(Player* player, uint8 vault, std::vector<ExtendedBankItemPos> const& items,
        ExtendedBankLayoutDelta const& delta, CharacterDatabaseTransaction trans);

    // saveCoreInventory pulls Player::SaveInventoryAndGoldToDB into the same transaction. Only
    // pass true where the Item objects are about to be freed -- it drags in the whole of
    // _SaveInventory (buyback purge, position cheat-detection, queue clear), which must not
    // run on an arbitrary packet boundary.
    void FlushLiveVault(Player* player, ExtendedBankSession& session, bool saveCoreInventory);
    void DetachLiveBank(Player* player, std::vector<ExtendedBankItemPos> const& items);
    void AttachVault(Player* player, uint8 vault);
    void RestoreItemSideData(Player* player, Item* item, CharacterDatabaseTransaction trans);

    // Reads a vault's contents in the 15-column shape Item::LoadFromDB expects, whichever
    // table owns that vault.
    [[nodiscard]] QueryResult QueryVaultContents(ObjectGuid::LowType lowGuid, uint8 vault) const;

    /* -- item plumbing, in ExtendedBankStorage.cpp ------------------------- */

    // Takes an item out of everything the player tracks it through, short of freeing it: the
    // update queue, the refund set, the soulbound-trade list, and the client's view of the
    // world. Whether the Item is then deleted or handed to a MailDraft is the caller's
    // business. Safe on an item that never reached a slot, which is the case AttachVault
    // needs -- Player::RemoveItem covers only the trade list, and only for an item that was
    // in a slot to be removed from.
    static void ReleaseItem(Player* player, Item* item);

    // Un-queues an item and marks it clean, exactly as Player::_LoadInventory does after
    // storing one, so nothing the module places can reach character_inventory.
    static void MarkClean(Player* player, Item* item);

    // Hands items back by mail, MAX_MAIL_ITEMS to a letter, in the caller's transaction.
    static void MailItemsBack(Player* player, std::vector<Item*>& items,
        CharacterDatabaseTransaction trans, char const* subject, char const* body);

    void SyncSessionCount() { _activeSessionCount.store(_sessions.size()); }

    // Player::Update, and therefore OnPlayerUpdate, runs on the MapUpdater worker pool, so
    // two players on different maps reach these containers concurrently whenever
    // MapUpdate.Threads > 1. Recursive because the public entry points call one another --
    // OpenVault into OwnsVault and SwitchTo, FlushActiveVault into DrainUpdateQueue,
    // BuyNextVault into LoadVaultList and EnsureDefaultVaultRow.
    mutable std::recursive_mutex _mutex;

    // Lock-free fast path for the two hooks that run for every player every tick and for
    // every packet received. Only ever written while _mutex is held.
    std::atomic<std::size_t> _activeSessionCount{ 0 };

    std::unordered_map<ObjectGuid, std::vector<ExtendedBankVault>> _vaults;
    std::unordered_map<ObjectGuid, ExtendedBankSession> _sessions;
};

#define sExtendedBankMgr ExtendedBankMgr::instance()

#endif // MOD_EXTENDED_BANK_H
