/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * Everything about a vault except its contents: what it is called, how many bank bag slots
 * have been paid for on it, where it sits in the menu, and what it costs to buy the next one.
 *
 * The useful thing to know about this file is what it cannot do. No function here moves,
 * creates or destroys an item, so no bug in it can lose somebody's belongings. That is the
 * point of the split: the risky work lives in ExtendedBankStorage.cpp and nowhere else, which
 * keeps the surface you have to be careful about small.
 *
 * One function bends that rule. SelfHealVaultRows deletes rows from the item table, but only
 * rows that have been proven wrong, and never the items themselves. It lives here because it
 * runs at login and login belongs to this file.
 */

#include "ExtendedBank.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

void ExtendedBankMgr::LoadVaultList(ObjectGuid playerGuid)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    _vaults.erase(playerGuid);

    // Sorted by menu position, falling back to the vault number when two rows claim the same
    // position. That fallback is not an edge case: until somebody reorders their menu for the
    // first time every row still holds the same default position, so the vault number is doing
    // all the work.
    QueryResult result = CharacterDatabase.Query(
        "SELECT vault, name, bag_slots, sort_order FROM mod_extended_bank_vaults "
        "WHERE owner_guid = {} ORDER BY sort_order, vault",
        playerGuid.GetCounter());

    if (!result)
        return;

    std::vector<ExtendedBankVault>& vaults = _vaults[playerGuid];

    do
    {
        Field* fields = result->Fetch();

        ExtendedBankVault vault;
        vault.Vault = fields[0].Get<uint8>();
        vault.Name = fields[1].Get<std::string>();
        vault.BagSlots = fields[2].Get<uint8>();
        vault.SortOrder = fields[3].Get<uint8>();

        vaults.push_back(std::move(vault));
    } while (result->NextRow());
}

void ExtendedBankMgr::LoadPlayer(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ObjectGuid const playerGuid = player->GetGUID();

    _sessions.erase(playerGuid);
    SyncSessionCount();

    LoadVaultList(playerGuid);

    // Login is the only moment when none of this module's writes can still be in flight, and
    // that is what makes the repair below safe to run. Reads and writes use different database
    // connections, and writes are committed in the background, so the same check at any other
    // time could see a row whose deletion is still queued and "repair" a vault row that was
    // perfectly correct.
    //
    // Skipped entirely for characters who own nothing beyond their original bank, which on a
    // typical realm is almost everybody. That keeps a blocking query off the login path for
    // players who have never touched this module.
    if (GetHighestOwnedVault(playerGuid) != EXTENDED_BANK_DEFAULT_VAULT)
        SelfHealVaultRows(playerGuid.GetCounter());

    // This is the crash insurance, and the reason this hook has to run before the core loads
    // the character's inventory.
    //
    // The number of bank bag slots a character owns is stored on the character record, and
    // while a vault is open that number belongs to the vault, not to the character's real
    // bank. If the server died at that moment, the saved number is the vault's. The core is
    // about to load vault 1's bags, find it apparently owns fewer slots than it has bags, and
    // post the surplus bags to the player with their contents inside.
    //
    // Vault 1's own items are untouched and will load normally. Only this one number needs
    // putting back, and it has to happen now.
    if (ExtendedBankVault const* meta = FindVault(playerGuid, EXTENDED_BANK_DEFAULT_VAULT))
    {
        if (player->GetBankBagSlotCount() != meta->BagSlots)
        {
            LOG_INFO("module.extendedbank", "Restoring bank bag slot count {} (was {}) for player {}.",
                meta->BagSlots, player->GetBankBagSlotCount(), playerGuid.ToString());
            player->SetBankBagSlotCount(meta->BagSlots);
        }
    }
}

void ExtendedBankMgr::ForgetPlayer(ObjectGuid playerGuid)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    _vaults.erase(playerGuid);
    _sessions.erase(playerGuid);
    SyncSessionCount();
}

void ExtendedBankMgr::SelfHealVaultRows(ObjectGuid::LowType lowGuid)
{
    // Deletes vault rows that have been proven wrong by something else, and only those.
    //
    // A crash at the wrong instant can leave a vault still claiming an item that has since
    // moved on. Anywhere else always wins that argument, because a vault row records only a
    // position, whereas each table joined below records the item genuinely being somewhere.
    // Leave the stale row and the next time that vault opens the module builds a second copy
    // of an item that still exists elsewhere, which is a duplicate.
    //
    // The character's inventory is the case this was written for. The other three exist
    // because some parts of the core find items by GUID alone, without caring which container
    // they are in, and the bank slots are in range of that search. So a crafted packet can put
    // an item from an open vault into the mail or onto the auction house. The stock client
    // cannot: its mail and auction windows only accept items dragged from the bags. Normal
    // operation handles this correctly within a tick; this is here for a crash inside that
    // tick.
    //
    // Every column joined on is indexed, so this stays one index lookup per vault row rather
    // than a scan.
    CharacterDatabase.DirectExecute(
        "DELETE v FROM mod_extended_bank_vault_items v "
        "LEFT JOIN character_inventory ci ON ci.item = v.item "
        "LEFT JOIN mail_items mi ON mi.item_guid = v.item "
        "LEFT JOIN auctionhouse ah ON ah.itemguid = v.item "
        "LEFT JOIN guild_bank_item gbi ON gbi.item_guid = v.item "
        "WHERE v.owner_guid = {} AND (ci.item IS NOT NULL OR mi.item_guid IS NOT NULL "
        "OR ah.itemguid IS NOT NULL OR gbi.item_guid IS NOT NULL)", lowGuid);
}

void ExtendedBankMgr::DeleteCharacterData(CharacterDatabaseTransaction trans, ObjectGuid::LowType lowGuid)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    // The core has already deleted the items themselves as part of erasing the character, so
    // all that is left is the module's own bookkeeping. Nothing here can orphan an item,
    // because by this point there are no items.
    trans->Append("DELETE FROM mod_extended_bank_vault_items WHERE owner_guid = {}", lowGuid);
    trans->Append("DELETE FROM mod_extended_bank_vaults WHERE owner_guid = {}", lowGuid);
}

/* ------------------------------------------------------------------------ */
/* Queries                                                                   */
/* ------------------------------------------------------------------------ */

std::vector<uint8> ExtendedBankMgr::GetOwnedVaults(ObjectGuid playerGuid) const
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    // Vault 1 is always first and cannot be moved. It is the character's real bank and the
    // state everything returns to, so having it in a predictable place is worth more than
    // letting somebody bury it at the bottom of their menu. Pinning it also means its own
    // stored position is irrelevant, which removes a thing that could be wrong.
    std::vector<uint8> owned{ EXTENDED_BANK_DEFAULT_VAULT };

    auto const itr = _vaults.find(playerGuid);
    if (itr == _vaults.end())
        return owned;

    std::vector<ExtendedBankVault const*> rest;
    rest.reserve(itr->second.size());

    for (ExtendedBankVault const& entry : itr->second)
        if (entry.Vault != EXTENDED_BANK_DEFAULT_VAULT)
            rest.push_back(&entry);

    // The list arrives from the database in menu order, but buying a vault appends to it in
    // memory, so it stops being sorted the moment anybody does. Hence sorting here rather than
    // trusting the load order.
    //
    // The vault number breaks ties, and ties are the normal case: every vault carries the same
    // default position until its owner reorders the menu for the first time. Without the
    // tiebreak the menu order would be whatever the sort happened to do that run.
    std::sort(rest.begin(), rest.end(),
        [](ExtendedBankVault const* left, ExtendedBankVault const* right)
        {
            if (left->SortOrder != right->SortOrder)
                return left->SortOrder < right->SortOrder;

            return left->Vault < right->Vault;
        });

    for (ExtendedBankVault const* entry : rest)
        owned.push_back(entry->Vault);

    return owned;
}

uint8 ExtendedBankMgr::GetHighestOwnedVault(ObjectGuid playerGuid) const
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    // Deliberately not written as "the last entry of the sorted list". This runs on every
    // login and every time anybody greets a banker, and the answer needs neither the vector
    // nor the sort that would involve.
    uint8 highest = EXTENDED_BANK_DEFAULT_VAULT;

    auto const itr = _vaults.find(playerGuid);
    if (itr != _vaults.end())
        for (ExtendedBankVault const& entry : itr->second)
            highest = std::max(highest, entry.Vault);

    return highest;
}

bool ExtendedBankMgr::OwnsVault(ObjectGuid playerGuid, uint8 vault) const
{
    if (vault == EXTENDED_BANK_DEFAULT_VAULT)
        return true;

    std::lock_guard<std::recursive_mutex> guard(_mutex);
    return FindVault(playerGuid, vault) != nullptr;
}

uint8 ExtendedBankMgr::GetActiveVault(ObjectGuid playerGuid) const
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    auto const itr = _sessions.find(playerGuid);
    return itr != _sessions.end() ? itr->second.ActiveVault : EXTENDED_BANK_DEFAULT_VAULT;
}

std::string ExtendedBankMgr::GetVaultName(ObjectGuid playerGuid, uint8 vault) const
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    if (ExtendedBankVault const* entry = FindVault(playerGuid, vault))
        if (!entry->Name.empty())
            return entry->Name;

    return vault == EXTENDED_BANK_DEFAULT_VAULT
        ? std::string("Main Vault")
        : Acore::StringFormat("Vault {}", vault);
}

uint8 ExtendedBankMgr::GetVaultBagSlots(ObjectGuid playerGuid, uint8 vault) const
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ExtendedBankVault const* entry = FindVault(playerGuid, vault);
    return entry ? entry->BagSlots : 0;
}

ExtendedBankDebugState ExtendedBankMgr::GetDebugState(ObjectGuid playerGuid) const
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ExtendedBankDebugState state;

    auto const itr = _sessions.find(playerGuid);
    if (itr != _sessions.end())
    {
        state.HasSession = true;
        state.ActiveVault = itr->second.ActiveVault;
        state.BankerGuid = itr->second.BankerGuid;
        state.PersistedItems = itr->second.PersistedLayout.size();
        state.PersistedBagSlots = itr->second.PersistedBagSlots;
    }

    state.HasVaultRow = FindVault(playerGuid, state.ActiveVault) != nullptr;
    return state;
}

ExtendedBankVault* ExtendedBankMgr::FindVault(ObjectGuid playerGuid, uint8 vault)
{
    return const_cast<ExtendedBankVault*>(std::as_const(*this).FindVault(playerGuid, vault));
}

ExtendedBankVault const* ExtendedBankMgr::FindVault(ObjectGuid playerGuid, uint8 vault) const
{
    auto const itr = _vaults.find(playerGuid);
    if (itr == _vaults.end())
        return nullptr;

    for (ExtendedBankVault const& entry : itr->second)
        if (entry.Vault == vault)
            return &entry;

    return nullptr;
}

namespace
{
    // Shortens a vault name to the allowed length and returns a version safe to put in a
    // query. If you only read one comment in this file, read this one.
    //
    // A vault name is the only piece of player-typed text this module ever puts into SQL, so
    // it is the only place an injection could live. The two steps below are deliberately
    // welded into one function because the ORDER of them is what makes it safe.
    //
    // Escape first and shorten afterwards, and the cut can land in the middle of an escape
    // sequence, leaving a trailing backslash. That backslash then escapes the query's own
    // closing quote, and everything the player typed after it is handed to the database as
    // SQL. Shorten first and that cannot happen: the truncation cuts on a character boundary,
    // and whatever survives is escaped whole.
    //
    // A prepared statement would make the whole question moot, and is not available: the core
    // registers them in an enum a module cannot extend without editing core files. So the
    // safety is maintained by hand here, and three things keep it true. Change any of them and
    // this becomes exploitable:
    //
    //   - the query's format string stays a literal, so the name is only ever a substituted
    //     argument and any braces inside it are inert
    //   - the escaped value stays inside single quotes, which is the only context the escape
    //     function is valid for
    //   - nothing else anywhere in the module puts player-typed text into a query
    //
    // The escape is charset-aware, because it goes through the live connection, and that
    // connection speaks utf8mb4. That is what rules out the multi-byte trick which defeats a
    // escape function called without a connection.
    std::string TruncateAndQuote(std::string& name)
    {
        utf8truncate(name, EXTENDED_BANK_VAULT_NAME_MAX_CHARS);

        std::string quoted = name;
        CharacterDatabase.EscapeString(quoted);
        return quoted;
    }
}

/* ------------------------------------------------------------------------ */
/* Bag slots, buying and renaming                                            */
/* ------------------------------------------------------------------------ */

void ExtendedBankMgr::SyncVaultBagSlots(Player* player, uint8 vault, CharacterDatabaseTransaction trans)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ExtendedBankVault* meta = FindVault(player->GetGUID(), vault);
    if (!meta)
        return;

    uint8 const liveCount = player->GetBankBagSlotCount();
    if (meta->BagSlots == liveCount)
        return;

    meta->BagSlots = liveCount;

    // Joins the caller's transaction when there is one, and goes out alone when there is not.
    // Joining matters when this count changes in the same breath as the vault's item rows: a
    // crash between two separate commits leaves a vault holding more bags than it has slots,
    // and the core posts the surplus to the player at their next login.
    //
    // Note that the assignment above is optimistic. Commits happen in the background and
    // report nothing back, so if this transaction is rolled back, memory now claims a count
    // the database does not have, and the early return above will suppress every later attempt
    // to correct it. Nothing visible happens in game and the next login repairs it. The same
    // trade-off applies to the layout bookkeeping in the storage file, and CLAUDE.md discusses
    // it under "Optimistic bookkeeping".
    CharacterDatabase.ExecuteOrAppend(trans, Acore::StringFormat(
        "UPDATE mod_extended_bank_vaults SET bag_slots = {} WHERE owner_guid = {} AND vault = {}",
        liveCount, player->GetGUID().GetCounter(), vault));
}

// Vault 1 needs a row in the metadata table even though the core owns its items, because its
// name and its purchased bag slot count have to live somewhere. Nothing creates that row at
// character creation, since most characters never touch this module at all, so it is created
// the first time one of them does something that needs it.
//
// The bag slot count is copied from the character as it stands right now, which is correct
// precisely because this only ever runs while vault 1 is the open one.
void ExtendedBankMgr::EnsureDefaultVaultRow(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ObjectGuid const playerGuid = player->GetGUID();

    if (FindVault(playerGuid, EXTENDED_BANK_DEFAULT_VAULT))
        return;

    ExtendedBankVault vault;
    vault.Vault = EXTENDED_BANK_DEFAULT_VAULT;
    vault.BagSlots = player->GetBankBagSlotCount();

    CharacterDatabase.Execute(
        "INSERT INTO mod_extended_bank_vaults (owner_guid, vault, name, bag_slots, created) "
        "VALUES ({}, {}, '', {}, UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE bag_slots = VALUES(bag_slots)",
        playerGuid.GetCounter(), uint32(EXTENDED_BANK_DEFAULT_VAULT), vault.BagSlots);

    _vaults[playerGuid].push_back(std::move(vault));
}

bool ExtendedBankMgr::BuyNextVault(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ObjectGuid const playerGuid = player->GetGUID();
    ChatHandler handler(player->GetSession());

    uint8 const next = GetHighestOwnedVault(playerGuid) + 1;

    if (next > sExtendedBankConfig.GetMaxVaults())
    {
        handler.PSendSysMessage("You already own the maximum number of bank vaults ({}).",
            sExtendedBankConfig.GetMaxVaults());
        return false;
    }

    uint32 const cost = sExtendedBankConfig.GetVaultCost(next);
    if (!player->HasEnoughMoney(cost))
    {
        handler.PSendSysMessage("You need {} gold to buy that vault.", cost / EXTENDED_BANK_COPPER_PER_GOLD);
        return false;
    }

    // Check the database before taking any money, even though the list in memory should
    // already know the answer.
    //
    // It should, but it is rebuilt from the database at every login, and if the two ever
    // disagree the player pays for a vault whose insert then fails. That failure happens in
    // the background where nothing reports it, so the player is simply poorer with nothing to
    // show for it. One indexed read, on something a player does a handful of times ever, buys
    // certainty that this cannot happen.
    if (QueryResult existing = CharacterDatabase.Query(
        "SELECT 1 FROM mod_extended_bank_vaults WHERE owner_guid = {} AND vault = {}",
        playerGuid.GetCounter(), next))
    {
        LOG_ERROR("module.extendedbank",
            "Player {} tried to buy vault {}, which already exists in the database but not in memory. "
            "Reloading their vaults; no money was taken.", playerGuid.ToString(), next);

        // Reload the vault list only. The full login routine would also discard the session
        // and reset the bank bag slot count, which for a player standing in front of an open
        // vault would strand it.
        LoadVaultList(playerGuid);
        handler.PSendSysMessage("Your vaults were out of date and have been reloaded. Please try again.");
        return false;
    }

    EnsureDefaultVaultRow(player);
    player->ModifyMoney(-int32(cost));

    ExtendedBankVault vault;
    vault.Vault = next;
    vault.BagSlots = 0;
    vault.SortOrder = EXTENDED_BANK_SORT_LAST;

    // Written to do nothing on a collision rather than to fail on one. The check above should
    // have ruled that out already, but if it somehow has not, a failing statement would take
    // the rest of its batch down with it, and all over a row that already says exactly what
    // was wanted.
    CharacterDatabase.Execute(
        "INSERT INTO mod_extended_bank_vaults (owner_guid, vault, name, bag_slots, sort_order, created) "
        "VALUES ({}, {}, '', 0, {}, UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE vault = VALUES(vault)",
        playerGuid.GetCounter(), next, uint32(EXTENDED_BANK_SORT_LAST));

    _vaults[playerGuid].push_back(std::move(vault));

    handler.PSendSysMessage("Vault {} purchased for {} gold.", next, cost / EXTENDED_BANK_COPPER_PER_GOLD);
    return true;
}

bool ExtendedBankMgr::RenameVault(Player* player, uint8 vault, std::string const& name)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ObjectGuid const playerGuid = player->GetGUID();

    // Every character owns vault 1, but nothing writes a row for it until they interact with
    // the module, so a character who has never bought or opened a vault has nothing to rename.
    // The menu offers renaming from their very first banker visit, so this is the ordinary
    // path rather than a corner case.
    if (vault == EXTENDED_BANK_DEFAULT_VAULT)
        EnsureDefaultVaultRow(player);

    ExtendedBankVault* meta = FindVault(playerGuid, vault);
    if (!meta)
        return false;

    // The client doubles every pipe character when it sends the contents of a text box. A
    // player who types |cffff0000Herbs|r arrives here as ||cffff0000Herbs||r, and a doubled
    // pipe renders as one literal pipe, which is why coloured names and inline icons showed up
    // as raw text until this was handled.
    //
    // Collapsing the pairs restores what the player actually typed. The cost is that a literal
    // pipe in a vault name becomes unreachable, which is the same trade the game itself makes
    // everywhere else.
    std::string stored;
    stored.reserve(name.size());

    for (std::size_t i = 0; i < name.size(); ++i)
    {
        stored.push_back(name[i]);
        if (name[i] == '|' && i + 1 < name.size() && name[i + 1] == '|')
            ++i;
    }

    // What a player types is not filtered, because colour codes and inline icons in a vault
    // name are a feature rather than an attack.
    //
    // The length is filtered, and this module is the only thing doing it. The client's rename
    // box declares no maximum, and the core reads the reply without checking one either, so
    // without this a pasted essay would reach a column that cannot hold it and break that
    // character's menu until somebody fixed it by hand.
    //
    // The truncation cuts on character boundaries rather than byte ones, so a name in any
    // language survives intact instead of ending in half a character, and it empties the
    // string outright if the input was not valid text at all. Both keep malformed data out of
    // the client's own parser. An empty name falls back to "Vault N" when it is displayed.
    //
    // Read TruncateAndQuote above before changing any of this. It carries a security property
    // that is not visible from here.
    std::string const escaped = TruncateAndQuote(stored);

    CharacterDatabase.Execute(
        "UPDATE mod_extended_bank_vaults SET name = '{}' WHERE owner_guid = {} AND vault = {}",
        escaped, playerGuid.GetCounter(), vault);

    // Cache the same string that was written, never the one the player typed. Caching the
    // untruncated version is exactly what turned an over-long rename into a permanently broken
    // menu: the database rejected the name, memory kept it anyway, and the menu went on
    // sending a name that could never be stored.
    meta->Name = std::move(stored);
    return true;
}

void ExtendedBankMgr::PersistVaultOrder(ObjectGuid playerGuid, std::vector<uint8> const& order)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    for (std::size_t index = 0; index < order.size(); ++index)
    {
        // Every position is written from scratch rather than swapping the two rows that
        // moved. That costs a few more writes on something a player does by hand, and buys a
        // property worth having: the result never depends on what the old values were, so
        // duplicate positions, gaps, and rows still holding the "put me last" default all
        // sort themselves out the first time anything moves.
        //
        // There is deliberately no unique key on this column, and ties fall back to the vault
        // number, so even a half-written result is still a valid menu order rather than an
        // error. That is what makes it safe to update memory before knowing whether the write
        // succeeded.
        uint8 const position = static_cast<uint8>(std::min<std::size_t>(index, EXTENDED_BANK_SORT_LAST));

        if (ExtendedBankVault* meta = FindVault(playerGuid, order[index]))
            meta->SortOrder = position;

        trans->Append("UPDATE mod_extended_bank_vaults SET sort_order = {} WHERE owner_guid = {} AND vault = {}",
            position, playerGuid.GetCounter(), order[index]);
    }

    CharacterDatabase.CommitTransaction(trans);
}

bool ExtendedBankMgr::MoveVaultUp(Player* player, uint8 vault)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ObjectGuid const playerGuid = player->GetGUID();

    // The default vault is pinned to the front of the menu, so there is nowhere for it to go.
    if (vault == EXTENDED_BANK_DEFAULT_VAULT)
        return false;

    std::vector<uint8> order = GetOwnedVaults(playerGuid);

    auto const position = std::find(order.begin(), order.end(), vault);
    if (position == order.end())
        return false;

    // Index 0 is the pinned default vault and index 1 sits directly below it, so neither has a
    // vault above it that this may pass. Refusing here rather than silently doing nothing is
    // what lets the caller tell "already at the top" from "does not own it".
    std::size_t const index = static_cast<std::size_t>(std::distance(order.begin(), position));
    if (index < 2)
        return false;

    std::swap(order[index - 1], order[index]);

    PersistVaultOrder(playerGuid, order);
    return true;
}
