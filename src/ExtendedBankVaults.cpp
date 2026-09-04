/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * Vault bookkeeping: the `mod_extended_bank_vaults` metadata table, the in-memory list built
 * from it, and everything that only reads, names, buys or renames a vault.
 *
 * Nothing here moves an item. SelfHealVaultRows is the one function that touches the item
 * table, and it only deletes rows -- it belongs to login, and login lives here.
 * ExtendedBankStorage.cpp owns the live bank and every item that passes through it.
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

    QueryResult result = CharacterDatabase.Query(
        "SELECT vault, name, bag_slots FROM mod_extended_bank_vaults WHERE owner_guid = {} ORDER BY vault",
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

    // Login is the only moment at which no write of this module's can still be in flight, so
    // it is the only moment at which reading character_inventory to repair vault rows is
    // safe. Reads take a synchronous connection while writes are committed on the async
    // worker pool, so running this check mid-session could see a row whose deletion has been
    // queued but not executed, and delete a perfectly good vault row because of it.
    //
    // Gated on the list loaded just above: only a character who owns a vault beyond the
    // default one can have rows in the item table at all, so on a realm where most characters
    // never buy a vault this synchronous query disappears from the login path entirely
    // instead of running once per login for everyone.
    if (GetHighestOwnedVault(playerGuid) != EXTENDED_BANK_DEFAULT_VAULT)
        SelfHealVaultRows(playerGuid.GetCounter());

    // The live bank always holds the default vault here: character_inventory has not been
    // read yet and it is the only thing Player::_LoadInventory will restore. PLAYER_BYTES_2,
    // however, may still carry the bag slot count of whatever vault was open when the server
    // died. Left alone, _LoadInventory would refuse the default vault's bank bags and mail
    // them back to the owner.
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
    // An unclean shutdown can leave an item listed in a vault while it has since gone somewhere
    // the core owns. Anywhere else always wins: a vault row is only a position, whereas every
    // table below is the item genuinely being somewhere, and leaving the row would let
    // AttachVault load a second copy of an item that still has its item_instance row.
    //
    // character_inventory is the case this was written for. The other three come from handlers
    // that resolve items by GUID -- Player::GetItemByGuid scans bank slots and bank bags
    // (PlayerStorage.cpp:423,435), so CMSG_SEND_MAIL and CMSG_AUCTION_SELL_ITEM can take an
    // item straight out of a live vault. The stock client cannot do it, since its mail and
    // auction frames only accept items dragged from the bags, but a forged packet can. The
    // drain that follows within a tick removes the vault row correctly; this covers a crash
    // inside that tick.
    //
    // Every joined column is indexed (mail_items and auctionhouse on the item, guild_bank_item
    // by Idx_item_guid), so this stays an index lookup per vault row.
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

    // Player::DeleteFromDB already drops every item_instance row owned by this character,
    // so only the module's own bookkeeping needs clearing here.
    trans->Append("DELETE FROM mod_extended_bank_vault_items WHERE owner_guid = {}", lowGuid);
    trans->Append("DELETE FROM mod_extended_bank_vaults WHERE owner_guid = {}", lowGuid);
}

/* ------------------------------------------------------------------------ */
/* Queries                                                                   */
/* ------------------------------------------------------------------------ */

std::vector<uint8> ExtendedBankMgr::GetOwnedVaults(ObjectGuid playerGuid) const
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    std::vector<uint8> owned{ EXTENDED_BANK_DEFAULT_VAULT };

    auto const itr = _vaults.find(playerGuid);
    if (itr != _vaults.end())
        for (ExtendedBankVault const& entry : itr->second)
            if (entry.Vault != EXTENDED_BANK_DEFAULT_VAULT)
                owned.push_back(entry.Vault);

    // LoadVaultList reads in vault order, but BuyNextVault and EnsureDefaultVaultRow append to
    // the live list, so it is only sorted by construction until one of those runs.
    std::sort(owned.begin(), owned.end());
    return owned;
}

uint8 ExtendedBankMgr::GetHighestOwnedVault(ObjectGuid playerGuid) const
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    // Deliberately not GetOwnedVaults().back(): this runs on every login and on every gossip
    // hello, and the answer needs neither the vector nor the sort.
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
    // Truncates a vault name in place and returns the escaped form to interpolate.
    //
    // The two steps are one function because their ORDER is a security property, not a
    // stylistic one. Escaping first and truncating after can cut an escape pair in half and
    // leave a trailing backslash, which then escapes the statement's own closing quote and
    // hands the rest of the name to the parser as SQL. Truncating first cannot: utf8truncate
    // cuts on a character boundary, and whatever survives is escaped whole afterwards.
    //
    // This is the module's only statement built by interpolation rather than by a prepared
    // statement. A prepared statement is not available to a module -- they are registered in
    // the core's CharacterDatabaseStatements enum and DoPrepareStatements, which a module
    // cannot extend without editing core files -- so the property is maintained here instead
    // of being structural. Keep it that way:
    //
    //   - the format string stays a literal, so the name is only ever an fmt *argument* and
    //     braces in it are inert;
    //   - the escaped value stays inside single quotes, the only context
    //     mysql_real_escape_string is valid for;
    //   - nothing else interpolates a player-supplied string into SQL anywhere in the module.
    //
    // The escape itself is charset-aware -- MySQLConnection::EscapeString passes the live
    // connection handle, and the connection is set to utf8mb4 (MySQLConnection.cpp:157), which
    // is what rules out the multi-byte lead-byte bypass that defeats a connection-less escape.
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

    // ExecuteOrAppend is the core's own "join the caller's transaction if there is one"
    // helper. Joining matters where the count changed in the same breath as the vault's item
    // rows: a crash between two separate commits leaves a vault whose bank bags the core will
    // mail back at the next login.
    //
    // The in-memory assignment above is optimistic -- commits are asynchronous and report
    // nothing back -- so an aborted transaction leaves this cache claiming a count the row
    // does not have, and the guard above then suppresses every later attempt to write it. Same
    // exposure CaptureLayout has, repaired the same way, at the next login's LoadVaultList.
    // See "Optimistic bookkeeping" in CLAUDE.md.
    CharacterDatabase.ExecuteOrAppend(trans, Acore::StringFormat(
        "UPDATE mod_extended_bank_vaults SET bag_slots = {} WHERE owner_guid = {} AND vault = {}",
        liveCount, player->GetGUID().GetCounter(), vault));
}

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

    // Charge only once the row is known not to exist. The in-memory list is normally
    // authoritative, but it is rebuilt from the database at login and a desync would otherwise
    // mean the player pays for a vault whose INSERT then fails on the primary key -- an async
    // failure nothing surfaces. One indexed read on a rare action is worth that certainty.
    if (QueryResult existing = CharacterDatabase.Query(
        "SELECT 1 FROM mod_extended_bank_vaults WHERE owner_guid = {} AND vault = {}",
        playerGuid.GetCounter(), next))
    {
        LOG_ERROR("module.extendedbank",
            "Player {} tried to buy vault {}, which already exists in the database but not in memory. "
            "Reloading their vaults; no money was taken.", playerGuid.ToString(), next);

        // Only the metadata list: LoadPlayer would drop the session and reset the bank bag
        // slot count, which would strand a vault that is open right now.
        LoadVaultList(playerGuid);
        handler.PSendSysMessage("Your vaults were out of date and have been reloaded. Please try again.");
        return false;
    }

    EnsureDefaultVaultRow(player);
    player->ModifyMoney(-int32(cost));

    ExtendedBankVault vault;
    vault.Vault = next;
    vault.BagSlots = 0;

    // A no-op on collision rather than a plain INSERT. The check above rules a collision out
    // on a healthy realm, and a statement that aborts would take the rest of its async batch
    // down with it over a row that already says exactly what was wanted.
    CharacterDatabase.Execute(
        "INSERT INTO mod_extended_bank_vaults (owner_guid, vault, name, bag_slots, created) "
        "VALUES ({}, {}, '', 0, UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE vault = VALUES(vault)",
        playerGuid.GetCounter(), next);

    _vaults[playerGuid].push_back(std::move(vault));

    handler.PSendSysMessage("Vault {} purchased for {} gold.", next, cost / EXTENDED_BANK_COPPER_PER_GOLD);
    return true;
}

bool ExtendedBankMgr::RenameVault(Player* player, uint8 vault, std::string const& name)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ObjectGuid const playerGuid = player->GetGUID();

    // The default vault is always owned but only gets a row once something writes one, so a
    // character who has never bought or switched a vault has no row to rename -- and the
    // Rename option is offered to them from the very first banker visit.
    if (vault == EXTENDED_BANK_DEFAULT_VAULT)
        EnsureDefaultVaultRow(player);

    ExtendedBankVault* meta = FindVault(playerGuid, vault);
    if (!meta)
        return false;

    // The client doubles every '|' when it sends the contents of an edit box, so a player who
    // types |cffff0000Herbs|r arrives here as ||cffff0000Herbs||r -- and '||' renders as a
    // literal pipe, which is why colour codes and icons showed up as plain text. Collapsing
    // the pairs back restores what the player actually typed. A literal '|' in a vault name
    // is not reachable as a result, which is the same trade the game itself makes.
    std::string stored;
    stored.reserve(name.size());

    for (std::size_t i = 0; i < name.size(); ++i)
    {
        stored.push_back(name[i]);
        if (name[i] == '|' && i + 1 < name.size() && name[i + 1] == '|')
            ++i;
    }

    // Content is not filtered: colour codes and inline icons in a vault name are a feature.
    // Length is, though, and the module is the only thing imposing one -- the client's own
    // rename box is StaticPopupDialogs["GOSSIP_ENTER_CODE"], which declares no maxLetters, and
    // WorldSession::HandleGossipSelectOptionOpcode reads the code string with no cap either.
    // utf8truncate cuts on a character boundary rather than a byte one, so a multi-byte name
    // can never be left with a split sequence, and it clears the string outright when the
    // input is not valid UTF-8 -- both of which keep malformed text out of the client's gossip
    // parser. An empty name falls back to "Vault N" in GetVaultName.
    //
    // See TruncateAndQuote above before changing any of this: it also carries a security
    // property that this call site cannot show on its own.
    std::string const escaped = TruncateAndQuote(stored);

    CharacterDatabase.Execute(
        "UPDATE mod_extended_bank_vaults SET name = '{}' WHERE owner_guid = {} AND vault = {}",
        escaped, playerGuid.GetCounter(), vault);

    // Assign the same string that was written. Caching the untruncated name here is what
    // turned an over-length rename into a broken gossip window that survived the failed
    // UPDATE: the menu kept re-sending a name the database had rejected.
    meta->Name = std::move(stored);
    return true;
}
