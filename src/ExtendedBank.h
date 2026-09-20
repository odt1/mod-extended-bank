/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * mod-extended-bank gives each character several banks instead of one. A player picks which
 * one to open from a menu on any banker NPC. They are called "vaults" here, and vault 1 is
 * the bank the character already had.
 *
 * The hard part is that the 3.3.5a client has no idea any of this exists, and cannot be
 * patched. Its bank window reads a fixed block of slots on the player and nothing else, so
 * there is exactly one way to show a second bank: take the first one's items out of those
 * slots in server memory, put the second one's items in, and send the client the same
 * "open the bank" packet the normal banker sends. The client never learns why the contents
 * changed.
 *
 * That trick is cheap. Keeping it safe is not, and one rule is why this code looks the way
 * it does:
 *
 *     Vault 1 lives in the core's own `character_inventory` table.
 *     Vaults 2 and up live in `mod_extended_bank_vault_items`.
 *     No item is ever moved from one of those tables to the other.
 *
 * Obey that and the worst a bug can do is lose an edit. Break it and a character's real bank
 * gets overwritten, because both tables key rows on (character, bag, slot) and the core will
 * happily write vault 5's item into the row describing vault 1's slot 45.
 *
 * Which leads to the single thing most of this class is defending. While a vault other than
 * vault 1 is open, its items are sitting in the slots the core believes are the character's
 * real bank. If the core saves the inventory in that moment, it writes them into
 * `character_inventory` and vault 1's contents are gone. The core saves whenever it likes,
 * from a dozen unrelated places, so the module cannot prevent the save. What it does instead
 * is take its items out of the list the core saves from (`Player::m_itemUpdateQueue`) before
 * any of those places can run. That removal is called "draining" throughout, and most of the
 * awkward-looking machinery below exists to make sure it always happens first.
 *
 * A vault other than vault 1 is only ever open while the player is standing at a banker.
 * Walk away, change map, log out or pick another vault, and vault 1 goes straight back into
 * the slots. So at any quiet moment, including a crash, the database looks exactly as it
 * would if this module had never been installed.
 *
 * IMPLEMENTATION.md explains the reasoning at length. CLAUDE.md is the short version plus the
 * mistakes that have already been made once.
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

// Vault 1 is the bank the character already had, so it always exists and is never listed in
// the module's own item table. Most of the code branches on "is this the default vault?"
// because that is the same question as "does the core own these rows?".
constexpr uint8 EXTENDED_BANK_DEFAULT_VAULT = 1;

// Ceiling on what an administrator can configure. The gossip menu has 32 slots and has to fit
// the vault list plus whatever else the NPC offers, so this leaves comfortable room.
constexpr uint8 EXTENDED_BANK_VAULT_LIMIT = 20;

// How often to check whether the player has wandered away from the banker, in milliseconds.
// The client never tells the server that the bank window closed, so walking out of range is
// the only reliable signal that the visit is over.
constexpr uint32 EXTENDED_BANK_RANGE_CHECK_INTERVAL = 1000;

constexpr uint32 EXTENDED_BANK_COPPER_PER_GOLD = 10000;

// Longest vault name accepted, counted in characters rather than bytes, so a name in any
// language gets the same allowance.
//
// Nothing else enforces a limit, which is the surprising part. The client's text box declares
// no maximum, and the core writes the string straight into the gossip packet without checking
// either. So if this constant were not here, a pasted essay would reach the database and fail
// the VARCHAR(255) column, leaving the menu permanently broken for that character. 240 is
// comfortably below both the column and the longest menu line Blizzard ships (397 characters),
// with room left for the rename prompt that wraps the name in "Enter a new name for {}:".
constexpr std::size_t EXTENDED_BANK_VAULT_NAME_MAX_CHARS = 240;

// Sort position meaning "put this at the end". Players can reorder their vault menu, and the
// reorder code renumbers the whole list from zero each time, so the only rows still holding
// this value are vaults bought since the last reorder. Sorting those last is exactly where a
// newly bought vault belongs.
constexpr uint8 EXTENDED_BANK_SORT_LAST = 255;

// Highest per-vault price an administrator can set, in gold. A character cannot hold more
// than 0x7FFFFFFF copper, so a larger price is unpayable anyway, and the multiplication into
// copper would overflow a uint32 and wrap round to a small number. A typo in the config would
// then make a vault almost free instead of refusing to sell it.
constexpr uint32 EXTENDED_BANK_MAX_VAULT_COST_GOLD = 214748;

enum class ExtendedBankSetting
{
    ENABLE,
    MAX_VAULTS,
    VAULT_COST,
    ALLOW_RESTRICTED_ITEMS,

    NUM_CONFIGS
};

class ExtendedBankConfig : public ConfigValueCache<ExtendedBankSetting>
{
public:
    ExtendedBankConfig() : ConfigValueCache(ExtendedBankSetting::NUM_CONFIGS) { }

    void BuildConfigCache() override;

    [[nodiscard]] bool IsEnabled() const { return GetConfigValue<bool>(ExtendedBankSetting::ENABLE); }

    // Whether this realm has switched off the rule that keeps unique items and items with a
    // countdown out of vaults 2 and up. Off by default, and the config file explains at length
    // both why the rule exists and what accepting the risk costs.
    [[nodiscard]] bool AllowRestrictedItems() const
    {
        return GetConfigValue<bool>(ExtendedBankSetting::ALLOW_RESTRICTED_ITEMS);
    }

    [[nodiscard]] uint8 GetMaxVaults() const
    {
        std::lock_guard<std::mutex> guard(_configMutex);
        return _maxVaults;
    }

    // Purchase price of `vault`, in copper. Only meaningful for vault 2 and up, since vault 1
    // is not for sale.
    [[nodiscard]] uint32 GetVaultCost(uint8 vault) const;

private:
    // A `.reload config` rebuilds these values on the console thread while players are reading
    // them from gossip menus on other threads. Publishing the new values under a lock, rather
    // than editing the old ones in place, is what stops a reader seeing a vector that is
    // halfway through being cleared or reallocated.
    mutable std::mutex _configMutex;
    uint8 _maxVaults{ 8 };
    std::vector<uint32> _vaultCost;
};

extern ExtendedBankConfig sExtendedBankConfig;

// What the database stores *about* a vault, as opposed to what is in it. Vault 1 gets a row
// here too, even though the core owns its items, because its name and its purchased bag slots
// have to survive a relog and the core has nowhere to keep them.
struct ExtendedBankVault
{
    uint8 Vault{ 0 };
    std::string Name;
    uint8 BagSlots{ 0 };

    // Where this vault appears in the menu, and nothing more. It is deliberately not the
    // vault's identity: items are filed under the vault *number*, so no amount of reordering
    // can move an item. That is what makes reordering safe to allow at any time, even with a
    // vault open or mid-trade.
    uint8 SortOrder{ EXTENDED_BANK_SORT_LAST };
};

// One row as the module last wrote it. The module keeps a copy of what it believes is on disk
// so that a save can compare, write only the rows that actually changed, and skip the database
// entirely when nothing moved. Without this, standing still at a banker would rewrite the
// whole vault on every tick.
struct ExtendedBankPersistedPos
{
    ObjectGuid::LowType Item{ 0 };
    ObjectGuid::LowType Bag{ 0 };
    uint8 Slot{ 0 };
};

// Everything the module needs to remember while a player has a vault open. It exists only for
// that visit: no session means the character's bank slots hold vault 1, which is the resting
// state and the state the database always agrees with.
struct ExtendedBankSession
{
    uint8 ActiveVault{ EXTENDED_BANK_DEFAULT_VAULT };
    ObjectGuid BankerGuid;
    uint32 RangeCheckTimer{ 0 };

    // The disk state described above, for the vault currently open.
    std::vector<ExtendedBankPersistedPos> PersistedLayout;
    uint8 PersistedBagSlots{ 0 };
};

// Where one item sits, using the same convention the core's own inventory table uses:
// Bag == 0 means Slot is one of the character's own bank slots (39 to 73); otherwise Bag is
// the item GUID of the container it is inside and Slot is an index within that container.
struct ExtendedBankItemPos
{
    Item* ItemPtr{ nullptr };
    ObjectGuid::LowType Bag{ 0 };
    uint8 Slot{ 0 };
};

// The difference between what the database holds for this vault and what the bank holds now.
// Worked out in one pass so that dragging a single item costs two statements rather than one
// per item in the vault.
struct ExtendedBankLayoutDelta
{
    // Positions in the live item list whose database row has to be written, either because the
    // item is new to this vault or because it moved.
    std::vector<std::size_t> Written;

    // A subset of Written: items that were somewhere else at the last save. Those may still
    // have a `character_inventory` row pointing at wherever they came from, which has to be
    // deleted as they enter the vault.
    std::vector<std::size_t> Entered;

    // Items this vault used to hold and no longer does.
    std::vector<ObjectGuid::LowType> Removed;

    bool BagSlotsChanged{ false };

    [[nodiscard]] bool Any() const
    {
        return !Written.empty() || !Removed.empty() || BagSlotsChanged;
    }
};

// A snapshot for the `.vault info` debug command, which reports live memory that no SQL query
// could show. Everything else about the module's state can be read straight from the database.
struct ExtendedBankDebugState
{
    bool HasSession{ false };

    // Whether the open vault has a metadata row at all. A character who has never bought or
    // opened a vault has none, and then the bag slot count read back is 0 rather than whatever
    // they have actually paid for, which is worth being able to see.
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

    // Reads the character's vault list into memory and repairs anything an unclean shutdown
    // left behind. Hooked to run before the core loads the character's inventory, because one
    // of those repairs has to happen first to stop the core mailing the character's bank bags
    // back to them.
    void LoadPlayer(Player* player);
    void ForgetPlayer(ObjectGuid playerGuid);
    void DeleteCharacterData(CharacterDatabaseTransaction trans, ObjectGuid::LowType lowGuid);

    // --- queries ---------------------------------------------------------

    // The vault numbers this character owns, in the order the menu should list them, always
    // starting with the default vault.
    [[nodiscard]] std::vector<uint8> GetOwnedVaults(ObjectGuid playerGuid) const;
    [[nodiscard]] uint8 GetHighestOwnedVault(ObjectGuid playerGuid) const;
    [[nodiscard]] bool OwnsVault(ObjectGuid playerGuid, uint8 vault) const;
    [[nodiscard]] uint8 GetActiveVault(ObjectGuid playerGuid) const;
    [[nodiscard]] std::string GetVaultName(ObjectGuid playerGuid, uint8 vault) const;
    [[nodiscard]] uint8 GetVaultBagSlots(ObjectGuid playerGuid, uint8 vault) const;
    [[nodiscard]] ExtendedBankDebugState GetDebugState(ObjectGuid playerGuid) const;

    // Walks the character's bank slots and bank bags into a flat list. Public only so the
    // debug command can report the same view the module works from.
    void CollectLiveBankItems(Player* player, std::vector<ExtendedBankItemPos>& items) const;

    // --- actions ---------------------------------------------------------

    // Swaps `target` into the character's bank slots and opens the stock bank window. Returns
    // false when the switch was refused, having already told the player why, except for a
    // vault they do not own: the menu never offers that, so reaching it means a forged packet
    // and there is nobody honest to apologise to.
    bool OpenVault(Player* player, uint8 target, ObjectGuid bankerGuid);

    // Puts vault 1 back without opening any window. This is the one that runs when the player
    // walks away, and it is why the database is always consistent at rest.
    void RevertToDefaultVault(Player* player);

    // Saves the open vault and takes its items out of memory without loading vault 1 back in.
    // Used at logout, where restoring vault 1 would be thrown away microseconds later.
    void FlushAndDetachForLogout(Player* player);

    // The drain described at the top of this file. Takes the open vault's items back out of
    // the list the core saves from, and rewrites the vault's rows if anything moved. It has to
    // run before anything that can reach the core's inventory save, and that is a much longer
    // list of things than it first appears: see the table in CLAUDE.md.
    void DrainUpdateQueue(Player* player);

    // The same work, hooked to the core's full character save.
    void FlushActiveVault(Player* player);

    void UpdateRangeCheck(Player* player, uint32 diff);

    bool BuyNextVault(Player* player);

    // False only when the character does not own that vault, and nothing is written in that
    // case. The caller decides whether that is worth reporting.
    bool RenameVault(Player* player, uint8 vault, std::string const& name);

    // Moves a vault one place up the menu. False when the character does not own it, when it
    // is the default vault (which is pinned to the top), or when it is already as high as it
    // can go. Nothing but a sort order changes, so unlike opening a vault this is safe in
    // combat, mid-trade, and with another vault open.
    bool MoveVaultUp(Player* player, uint8 vault);

private:
    /* -- vault metadata, in ExtendedBankVaults.cpp ------------------------ */

    // Reloads just the list of vaults, leaving any open vault and its items alone. LoadPlayer
    // would reset those too, which would strand a vault the player is standing in front of.
    void LoadVaultList(ObjectGuid playerGuid);

    // Drops vault rows whose item has since turned up somewhere the core owns: the character's
    // inventory, their mail, an auction, or a guild bank. A crash at the wrong instant can
    // leave a row behind claiming an item that has moved on, and opening that vault afterwards
    // would load a second copy of it. Login is the only safe moment to run this, because reads
    // and writes use different database connections and a mid-session check could see a row
    // whose deletion is still queued.
    void SelfHealVaultRows(ObjectGuid::LowType lowGuid);

    void EnsureDefaultVaultRow(Player* player);

    // Renumbers every vault's sort order from the given list, rather than swapping the two
    // rows that moved. That is a handful of extra writes on an action a player performs by
    // hand, and it buys a useful property: the result does not depend on what the old values
    // were, so duplicate positions, gaps, and rows still holding the "put me last" default all
    // sort themselves out the first time anything is moved.
    void PersistVaultOrder(ObjectGuid playerGuid, std::vector<uint8> const& order);

    // Records how many bank bag slots the open vault has, when that has changed. Buying a slot
    // only updates the character in memory, so without this the purchase would vanish the
    // moment the player switched to another vault.
    void SyncVaultBagSlots(Player* player, uint8 vault, CharacterDatabaseTransaction trans = nullptr);

    [[nodiscard]] ExtendedBankVault* FindVault(ObjectGuid playerGuid, uint8 vault);
    [[nodiscard]] ExtendedBankVault const* FindVault(ObjectGuid playerGuid, uint8 vault) const;

    /* -- the live bank, in ExtendedBankStorage.cpp ------------------------- */

    void SwitchTo(Player* player, uint8 target);
    void PersistOutgoingVault(Player* player, uint8 vault);

    // Records the layout just written, so the next save can tell "nothing moved" from "needs
    // rewriting" without asking the database.
    void CaptureLayout(ExtendedBankSession& session, std::vector<ExtendedBankItemPos> const& items,
        uint8 bagSlots) const;

    // Compares the bank against the last recorded layout in a single pass. The obvious
    // implementation, comparing each item against each remembered row, is quadratic, and a
    // full vault is 28 slots plus seven 36-slot bags. That ran to five figures of comparisons
    // on every packet the player sent.
    [[nodiscard]] ExtendedBankLayoutDelta ComputeLayoutDelta(ExtendedBankSession const& session,
        std::vector<ExtendedBankItemPos> const& items, uint8 bagSlots) const;
    [[nodiscard]] static bool AnyItemQueued(std::vector<ExtendedBankItemPos> const& items);

    // Pushes items that are not allowed in this vault back into the player's bags, or into
    // their mailbox when the bags are full. Returns true if anything moved, in which case the
    // caller has to re-read the bank because its own copy is now stale.
    bool EvictRestrictedItems(Player* player, std::vector<ExtendedBankItemPos> const& items,
        CharacterDatabaseTransaction trans, ObjectGuid bankerGuid);

    // Writes the rows the delta says changed. Its deletes must all be emitted before any of
    // its inserts: two items swapping places would otherwise collide on the unique key,
    // because the first insert lands on a slot its previous occupant has not vacated yet, and
    // the collision aborts the whole transaction. Since writes are committed asynchronously
    // that abort is invisible in game, and the vault just appears to forget the player's last
    // edit.
    void PersistVaultLayout(Player* player, uint8 vault, std::vector<ExtendedBankItemPos> const& items,
        ExtendedBankLayoutDelta const& delta, CharacterDatabaseTransaction trans);

    // `saveCoreInventory` folds the core's own inventory save into the same transaction, which
    // is what makes an item *leaving* a vault atomic. Pass it only where the item objects are
    // about to be freed, because it drags in the rest of the core's save as well: a buyback
    // purge, a cheat check that can mark an item destroyed, and a wipe of the pending-write
    // list. None of that should happen at an arbitrary packet boundary.
    void FlushLiveVault(Player* player, ExtendedBankSession& session, bool saveCoreInventory);

    void DetachLiveBank(Player* player, std::vector<ExtendedBankItemPos> const& items);
    void AttachVault(Player* player, uint8 vault);
    void RestoreItemSideData(Player* player, Item* item, CharacterDatabaseTransaction trans);

    // Reads a vault's contents in the exact column order the core's item loader expects, from
    // whichever of the two tables owns that vault.
    [[nodiscard]] QueryResult QueryVaultContents(ObjectGuid::LowType lowGuid, uint8 vault) const;

    /* -- item plumbing, in ExtendedBankStorage.cpp ------------------------- */

    // Unhooks an item from everything the player tracks it through, short of destroying it:
    // the pending-write list, the refund record, the soulbound-trade record, and the client's
    // copy of it. Whether the item is then deleted or posted to the player is the caller's
    // business. Unlike the core's own removal this is safe on an item that never made it into
    // a slot, which is the case when loading a vault goes wrong halfway.
    static void ReleaseItem(Player* player, Item* item);

    // Marks an item as needing no write, exactly as the core does after loading one from the
    // database. Every item the module places has to go through this, or the core will see it
    // as a pending change and write it into the character's real inventory.
    static void MarkClean(Player* player, Item* item);

    // Posts items back to their owner, splitting across letters as needed, inside the caller's
    // transaction so the item cannot exist in two places if the server dies mid-write.
    static void MailItemsBack(Player* player, std::vector<Item*>& items,
        CharacterDatabaseTransaction trans, char const* subject, char const* body);

    void SyncSessionCount() { _activeSessionCount.store(_sessions.size()); }

    // Player updates run on a pool of worker threads, one per map, so two players in different
    // zones reach these containers at the same instant on any realm with more than one map
    // thread. The lock is recursive because the public entry points legitimately call one
    // another: opening a vault checks ownership and then switches, a save drains first, and
    // buying a vault reloads the list.
    mutable std::recursive_mutex _mutex;

    // Read by the two hooks that fire for every player on every tick and for every packet the
    // server receives. Taking a lock there would put every player on the realm behind one
    // mutex to answer a question that is almost always "no vault is open". Only ever written
    // while the lock above is held.
    std::atomic<std::size_t> _activeSessionCount{ 0 };

    std::unordered_map<ObjectGuid, std::vector<ExtendedBankVault>> _vaults;
    std::unordered_map<ObjectGuid, ExtendedBankSession> _sessions;
};

#define sExtendedBankMgr ExtendedBankMgr::instance()

#endif // MOD_EXTENDED_BANK_H
