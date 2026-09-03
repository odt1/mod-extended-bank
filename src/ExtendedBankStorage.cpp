/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

#include "ExtendedBank.h"
#include "Bag.h"
#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "Util.h"
#include "WorldSession.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
    // The 11 item_instance columns Item::LoadFromDB expects in fields[0..10], in order,
    // followed by bag / slot / item guid / entry in fields[11..14]. Same shape as the core
    // CHAR_SEL_CHARACTER_INVENTORY projection.
    constexpr char const* ITEM_INSTANCE_COLUMNS =
        "ii.creatorGuid, ii.giftCreatorGuid, ii.count, ii.duration, ii.charges, ii.flags, "
        "ii.enchantments, ii.randomPropertyId, ii.durability, ii.playedTime, ii.text";
}

ExtendedBankMgr* ExtendedBankMgr::instance()
{
    static ExtendedBankMgr instance;
    return &instance;
}

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

    std::sort(owned.begin(), owned.end());
    return owned;
}

uint8 ExtendedBankMgr::GetHighestOwnedVault(ObjectGuid playerGuid) const
{
    return GetOwnedVaults(playerGuid).back();
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
    if (itr == _sessions.end())
        return state;

    state.HasSession = true;
    state.ActiveVault = itr->second.ActiveVault;
    state.BankerGuid = itr->second.BankerGuid;
    state.PersistedItems = itr->second.PersistedLayout.size();
    state.PersistedBagSlots = itr->second.PersistedBagSlots;
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

/* ------------------------------------------------------------------------ */
/* Collecting and persisting the live bank                                   */
/* ------------------------------------------------------------------------ */

void ExtendedBankMgr::CollectLiveBankItems(Player* player, std::vector<ExtendedBankItemPos>& items) const
{
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items.push_back({ item, 0, slot });

    // Each bank bag is followed immediately by its own contents, so walking the result
    // forwards places bags before their items and walking it backwards removes the items
    // before their bag.
    for (uint8 slot = BANK_SLOT_BAG_START; slot < BANK_SLOT_BAG_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        items.push_back({ item, 0, slot });

        Bag* bag = item->ToBag();
        if (!bag)
            continue;

        for (uint8 bagSlot = 0; bagSlot < bag->GetBagSize(); ++bagSlot)
            if (Item* content = bag->GetItemByPos(bagSlot))
                items.push_back({ content, bag->GetGUID().GetCounter(), bagSlot });
    }
}

void ExtendedBankMgr::PersistVaultLayout(Player* player, uint8 vault,
    std::vector<ExtendedBankItemPos> const& items, ExtendedBankLayoutDelta const& delta,
    CharacterDatabaseTransaction trans)
{
    ObjectGuid::LowType const lowGuid = player->GetGUID().GetCounter();

    if (vault != EXTENDED_BANK_DEFAULT_VAULT)
    {
        // Only the rows that actually changed are touched. This used to delete the whole vault
        // and re-insert every item on any change at all, which is two statements per item --
        // over 500 of them for a single drag inside a full vault, every one of which had to be
        // parsed and executed on the async worker for a layout that differed in one row.
        //
        // Every row about to move is deleted before any of them is written back. Two items
        // swapping places inside a vault would otherwise collide on
        // UNIQUE KEY (owner_guid, vault, bag, slot): the first INSERT would land on a slot its
        // previous occupant has not vacated yet, and abort the whole transaction. A row left
        // untouched cannot collide with one that moved, because a position only becomes
        // available when whatever held it is itself in this delete list.
        std::string doomed;

        auto const addDoomed = [&doomed](ObjectGuid::LowType itemGuid)
        {
            if (!doomed.empty())
                doomed += ',';

            doomed += std::to_string(itemGuid);
        };

        for (std::size_t index : delta.Written)
            addDoomed(items[index].ItemPtr->GetGUID().GetCounter());

        for (ObjectGuid::LowType itemGuid : delta.Removed)
            addDoomed(itemGuid);

        if (!doomed.empty())
        {
            // Keyed on the item alone, deliberately: PRIMARY KEY is (item), item GUIDs are
            // globally unique, and this statement is the module claiming them. A row left for
            // one of these items under a different vault -- or under a different owner, which
            // a crash during a character delete plus GUID reuse can produce -- would survive
            // any narrower delete and then abort the INSERT on the primary key.
            //
            // The predecessor of this code used ON DUPLICATE KEY UPDATE for that instead. It
            // is not used here because this table has a second unique key on
            // (owner_guid, vault, bag, slot): an upsert that collided on *that* one would
            // quietly repoint an existing row at a different item, which loses an item, where
            // aborting the transaction only loses the flush and is repaired at next login.
            trans->Append("DELETE FROM mod_extended_bank_vault_items WHERE item IN ({})", doomed);
        }

        if (!delta.Written.empty())
        {
            std::string values;

            for (std::size_t index : delta.Written)
            {
                if (!values.empty())
                    values += ',';

                values += Acore::StringFormat("({},{},{},{},{})",
                    items[index].ItemPtr->GetGUID().GetCounter(), lowGuid, vault,
                    items[index].Bag, uint32(items[index].Slot));
            }

            trans->Append("INSERT INTO mod_extended_bank_vault_items (item, owner_guid, vault, bag, slot) "
                "VALUES {}", values);
        }

        // The module now owns these items' positions, so any character_inventory row still
        // describing where they used to sit has to go. This is delete-by-item-GUID, the very
        // call mail, auction and the guild bank make when an item leaves a player's
        // inventory; it can never touch the default vault's rows, because the default vault's
        // items are not live while another vault is open. Only items that were not already in
        // this vault at the last flush need it -- the ones that were had their row deleted
        // then, and have had no inventory position since.
        for (std::size_t index : delta.Entered)
            items[index].ItemPtr->DeleteFromInventoryDB(trans);
    }

    // item_instance has to be written for every vault, the default one included: the Item
    // objects are about to be freed and durability, charges or stack size may have changed
    // this session. Un-queueing first is what stops the core's _SaveInventory from writing
    // these items back into character_inventory.
    for (ExtendedBankItemPos const& pos : items)
    {
        pos.ItemPtr->RemoveFromUpdateQueueOf(player);
        pos.ItemPtr->SaveToDB(trans);
    }

    if (ExtendedBankVault* meta = FindVault(player->GetGUID(), vault))
    {
        uint8 const liveCount = player->GetBankBagSlotCount();
        if (meta->BagSlots != liveCount)
        {
            meta->BagSlots = liveCount;
            trans->Append("UPDATE mod_extended_bank_vaults SET bag_slots = {} WHERE owner_guid = {} AND vault = {}",
                liveCount, lowGuid, vault);
        }
    }
}

void ExtendedBankMgr::CaptureLayout(ExtendedBankSession& session,
    std::vector<ExtendedBankItemPos> const& items, uint8 bagSlots) const
{
    session.PersistedLayout.clear();
    session.PersistedLayout.reserve(items.size());

    for (ExtendedBankItemPos const& pos : items)
        session.PersistedLayout.push_back({ pos.ItemPtr->GetGUID().GetCounter(), pos.Bag, pos.Slot });

    session.PersistedBagSlots = bagSlots;
}

ExtendedBankLayoutDelta ExtendedBankMgr::ComputeLayoutDelta(ExtendedBankSession const& session,
    std::vector<ExtendedBankItemPos> const& items, uint8 bagSlots) const
{
    ExtendedBankLayoutDelta delta;
    delta.BagSlotsChanged = session.PersistedBagSlots != bagSlots;

    // Indexed by item GUID rather than by position, so an item that merely moved is told apart
    // from one that entered or left. What is left in the map once every live item has been
    // looked up is, by definition, what the vault no longer holds.
    std::unordered_map<ObjectGuid::LowType, ExtendedBankPersistedPos const*> was;
    was.reserve(session.PersistedLayout.size());

    for (ExtendedBankPersistedPos const& pos : session.PersistedLayout)
        was.emplace(pos.Item, &pos);

    for (std::size_t i = 0; i < items.size(); ++i)
    {
        auto const itr = was.find(items[i].ItemPtr->GetGUID().GetCounter());

        if (itr == was.end())
        {
            delta.Written.push_back(i);
            delta.Entered.push_back(i);
            continue;
        }

        if (itr->second->Bag != items[i].Bag || itr->second->Slot != items[i].Slot)
            delta.Written.push_back(i);

        was.erase(itr);
    }

    delta.Removed.reserve(was.size());

    for (auto const& entry : was)
        delta.Removed.push_back(entry.first);

    return delta;
}

bool ExtendedBankMgr::AnyItemQueued(std::vector<ExtendedBankItemPos> const& items) const
{
    for (ExtendedBankItemPos const& pos : items)
        if (pos.ItemPtr->IsInUpdateQueue() || pos.ItemPtr->GetState() != ITEM_UNCHANGED)
            return true;

    return false;
}

void ExtendedBankMgr::DetachLiveBank(Player* player, std::vector<ExtendedBankItemPos> const& items)
{
    // Reverse order: a bag's contents must leave before the bag itself.
    for (auto itr = items.rbegin(); itr != items.rend(); ++itr)
    {
        Item* item = itr->ItemPtr;

        // Player::RemoveItem only touches memory and the client's update fields; it never
        // changes Item::uState, so an unchanged item costs no database write here.
        //
        // update = false because with update = true the call ends in
        // pItem->SendUpdateToPlayer(this) (PlayerStorage.cpp:3073) -- a full create-object
        // block for an item that is about to be destroyed on the very next line. The slot
        // GUIDs it clears are still pushed, as one batch, by OpenVault's SendUpdateToPlayer.
        player->RemoveItem(item->GetBagSlot(), item->GetSlot(), false);
        item->RemoveFromUpdateQueueOf(player);
        player->DeleteRefundReference(item->GetGUID());

        if (item->IsInWorld())
        {
            item->RemoveFromWorld();
            item->DestroyForPlayer(player);
        }

        delete item;
    }
}

void ExtendedBankMgr::RestoreItemSideData(Player* player, Item* item, CharacterDatabaseTransaction trans)
{
    if (item->IsRefundable())
    {
        if (item->GetPlayedTime() > (2 * HOUR))
        {
            CharacterDatabasePreparedStatement* stmt =
                CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_REFUND_INSTANCE);
            stmt->SetData(0, item->GetGUID().GetCounter());
            trans->Append(stmt);

            item->RemoveFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_REFUNDABLE);
        }
        else
        {
            CharacterDatabasePreparedStatement* stmt =
                CharacterDatabase.GetPreparedStatement(CHAR_SEL_ITEM_REFUNDS);
            stmt->SetData(0, item->GetGUID().GetCounter());
            stmt->SetData(1, player->GetGUID().GetRawValue());

            if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
            {
                item->SetRefundRecipient((*result)[0].Get<uint32>());
                item->SetPaidMoney((*result)[1].Get<uint32>());
                item->SetPaidExtendedCost((*result)[2].Get<uint16>());
                player->AddRefundReference(item->GetGUID());
            }
            else
            {
                item->RemoveFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_REFUNDABLE);
            }
        }
    }
    else if (item->IsBOPTradable())
    {
        CharacterDatabasePreparedStatement* stmt =
            CharacterDatabase.GetPreparedStatement(CHAR_SEL_ITEM_BOP_TRADE);
        stmt->SetData(0, item->GetGUID().GetCounter());

        if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        {
            AllowedLooterSet looters;
            for (std::string_view guidStr : Acore::Tokenize((*result)[0].Get<std::string_view>(), ' ', false))
                if (Optional<ObjectGuid::LowType> looterGuid = Acore::StringTo<ObjectGuid::LowType>(guidStr))
                    looters.insert(ObjectGuid::Create<HighGuid::Player>(*looterGuid));

            if (looters.size() > 1 && item->GetTemplate()->GetMaxStackSize() == 1 && item->IsSoulBound())
            {
                item->SetSoulboundTradeable(looters);
                player->AddTradeableItem(item);
            }
            else
            {
                item->ClearSoulboundTradeable(player);
            }
        }
        else
        {
            item->RemoveFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_BOP_TRADEABLE);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Attaching a vault to the live bank slots                                  */
/* ------------------------------------------------------------------------ */

void ExtendedBankMgr::AttachVault(Player* player, uint8 vault)
{
    ObjectGuid::LowType const lowGuid = player->GetGUID().GetCounter();
    QueryResult result;

    if (vault == EXTENDED_BANK_DEFAULT_VAULT)
    {
        // The default vault is simply the bank half of character_inventory: the top level
        // bank slots, plus everything sitting inside a bag that occupies a bank bag slot.
        result = CharacterDatabase.Query(
            "SELECT {}, ci.bag, ci.slot, ci.item, ii.itemEntry "
            "FROM character_inventory ci "
            "JOIN item_instance ii ON ci.item = ii.guid "
            "LEFT JOIN character_inventory cb ON cb.item = ci.bag AND cb.guid = ci.guid "
            "WHERE ci.guid = {} AND ("
            "(ci.bag = 0 AND ci.slot >= {} AND ci.slot < {}) OR "
            "(ci.bag != 0 AND cb.bag = 0 AND cb.slot >= {} AND cb.slot < {})) "
            "ORDER BY ci.bag, ci.slot",
            ITEM_INSTANCE_COLUMNS, lowGuid,
            uint32(BANK_SLOT_ITEM_START), uint32(BANK_SLOT_BAG_END),
            uint32(BANK_SLOT_BAG_START), uint32(BANK_SLOT_BAG_END));
    }
    else
    {
        // No self-heal here: it reads character_inventory on a synchronous connection while
        // this module's deletes of those same rows are committed asynchronously, so mid-session
        // it can see a row whose deletion is still queued and destroy a valid vault row.
        // SelfHealVaultRows runs once per login instead, where nothing is in flight.
        result = CharacterDatabase.Query(
            "SELECT {}, v.bag, v.slot, v.item, ii.itemEntry "
            "FROM mod_extended_bank_vault_items v "
            "JOIN item_instance ii ON v.item = ii.guid "
            "WHERE v.owner_guid = {} AND v.vault = {} "
            "ORDER BY v.bag, v.slot",
            ITEM_INSTANCE_COLUMNS, lowGuid, vault);
    }

    if (!result)
    {
        // An empty vault is still a known state; record it so the first flush can skip.
        if (vault != EXTENDED_BANK_DEFAULT_VAULT)
        {
            ExtendedBankSession& session = _sessions[player->GetGUID()];
            CaptureLayout(session, {}, player->GetBankBagSlotCount());
            SyncSessionCount();
        }

        return;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    std::map<ObjectGuid::LowType, Bag*> bagMap;
    std::set<ObjectGuid::LowType> unusableBags;
    std::vector<Item*> problematicItems;

    // Removes an item's position row from whichever table owns this vault. Every early exit
    // below has to call it, or the row survives and the same failure repeats on every open.
    auto dropRow = [&](ObjectGuid::LowType guid)
    {
        if (vault != EXTENDED_BANK_DEFAULT_VAULT)
            trans->Append("DELETE FROM mod_extended_bank_vault_items WHERE item = {}", guid);
        else
            Item::DeleteFromInventoryDB(trans, guid);
    };

    do
    {
        Field* fields = result->Fetch();

        ObjectGuid::LowType const bagGuid = fields[11].Get<uint32>();
        uint8 const slot = fields[12].Get<uint8>();
        ObjectGuid::LowType const itemGuid = fields[13].Get<uint32>();
        uint32 const itemEntry = fields[14].Get<uint32>();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto)
        {
            // Same treatment Player::_LoadItem gives an unknown entry: drop the position row
            // and the item_instance row, so the error is not repeated on every future open.
            LOG_ERROR("module.extendedbank", "Player {} has unknown item entry {} in vault {}, removing.",
                player->GetGUID().ToString(), itemEntry, vault);
            dropRow(itemGuid);
            Item::DeleteFromDB(trans, itemGuid);
            continue;
        }

        Item* item = NewItemOrBag(proto);
        if (!item->LoadFromDB(itemGuid, player->GetGUID(), fields, itemEntry))
        {
            LOG_ERROR("module.extendedbank", "Player {} has a broken item (entry {}) in vault {}, removing.",
                player->GetGUID().ToString(), itemEntry, vault);
            dropRow(itemGuid);

            // Item::SaveToDB deletes the object itself on ITEM_REMOVED (Item.cpp:407), so
            // there must be no delete of our own here.
            item->FSetState(ITEM_REMOVED);
            item->SaveToDB(trans);
            continue;
        }

        RestoreItemSideData(player, item, trans);

        InventoryResult err = EQUIP_ERR_OK;

        if (!bagGuid)
        {
            item->SetContainer(nullptr);
            item->SetSlot(slot);

            ItemPosCountVec dest;
            err = player->CanBankItem(INVENTORY_SLOT_BAG_0, slot, dest, item, false, false);
            if (err == EQUIP_ERR_OK)
            {
                item = player->BankItem(dest, item, true);
                if (!item)
                {
                    LOG_ERROR("module.extendedbank", "Player {} lost item {} while attaching vault {}.",
                        player->GetGUID().ToString(), itemGuid, vault);
                    continue;
                }
            }

            if (Bag* bag = item->ToBag())
            {
                if (err == EQUIP_ERR_OK)
                    bagMap[item->GetGUID().GetCounter()] = bag;
                else
                    unusableBags.insert(itemGuid);
            }
        }
        else
        {
            item->SetSlot(NULL_SLOT);

            auto const bagItr = bagMap.find(bagGuid);
            if (bagItr == bagMap.end())
            {
                // Either the bag row is missing entirely, or the bag itself could not be
                // placed and was mailed back. Mail the contents too rather than dropping the
                // Item and leaving its row behind to fail again on every future open.
                if (unusableBags.find(bagGuid) == unusableBags.end())
                {
                    LOG_ERROR("module.extendedbank",
                        "Player {} has item {} in vault {} referencing unknown bag {}; mailing it back.",
                        player->GetGUID().ToString(), itemGuid, vault, bagGuid);
                }

                dropRow(itemGuid);
                problematicItems.push_back(item);
                continue;
            }

            ItemPosCountVec dest;
            err = player->CanStoreItem(bagItr->second->GetSlot(), slot, dest, item);
            if (err == EQUIP_ERR_OK)
            {
                item = player->StoreItem(dest, item, true);
                if (!item)
                {
                    LOG_ERROR("module.extendedbank", "Player {} lost item {} while attaching vault {}.",
                        player->GetGUID().ToString(), itemGuid, vault);
                    continue;
                }
            }
        }

        if (err == EQUIP_ERR_OK)
        {
            // Cancel the ITEM_CHANGED that StoreItem just queued, the same way
            // Player::_LoadInventory does, so nothing ever reaches character_inventory.
            item->RemoveFromUpdateQueueOf(player);
            item->SetState(ITEM_UNCHANGED);
        }
        else
        {
            LOG_ERROR("module.extendedbank",
                "Player {} could not be given item {} from vault {} (reason {}); mailing it back.",
                player->GetGUID().ToString(), itemGuid, vault, uint32(err));

            dropRow(itemGuid);
            problematicItems.push_back(item);
        }
    } while (result->NextRow());

    while (!problematicItems.empty())
    {
        MailDraft draft("Bank vault", "Some items could not be placed back into your bank.");

        for (uint8 i = 0; !problematicItems.empty() && i < MAX_MAIL_ITEMS; ++i)
        {
            draft.AddItem(problematicItems.front());
            problematicItems.erase(problematicItems.begin());
        }

        draft.SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
    }

    // Un-queueing each item as it was stored is not enough on its own: Player::_StoreItem
    // marks the *containing bag* ITEM_CHANGED as well, so every bank bag is pushed back into
    // the update queue by the first item stored into it. The core sidesteps this by holding
    // m_itemUpdateQueueBlocked across the whole of _LoadInventory, which is private, so the
    // module sweeps the finished bank instead.
    std::vector<ExtendedBankItemPos> attached;
    CollectLiveBankItems(player, attached);

    for (ExtendedBankItemPos const& pos : attached)
    {
        pos.ItemPtr->RemoveFromUpdateQueueOf(player);
        pos.ItemPtr->SetState(ITEM_UNCHANGED);
    }

    CharacterDatabase.CommitTransaction(trans);

    // Record what is now live so the first flush of this vault can tell that nothing moved.
    if (vault != EXTENDED_BANK_DEFAULT_VAULT)
    {
        ExtendedBankSession& session = _sessions[player->GetGUID()];
        CaptureLayout(session, attached, player->GetBankBagSlotCount());
        SyncSessionCount();
    }
}

/* ------------------------------------------------------------------------ */
/* Switching                                                                 */
/* ------------------------------------------------------------------------ */

void ExtendedBankMgr::SyncVaultBagSlots(Player* player, uint8 vault)
{
    ExtendedBankVault* meta = FindVault(player->GetGUID(), vault);
    if (!meta)
        return;

    uint8 const liveCount = player->GetBankBagSlotCount();
    if (meta->BagSlots == liveCount)
        return;

    meta->BagSlots = liveCount;
    CharacterDatabase.Execute(
        "UPDATE mod_extended_bank_vaults SET bag_slots = {} WHERE owner_guid = {} AND vault = {}",
        liveCount, player->GetGUID().GetCounter(), vault);
}

void ExtendedBankMgr::PersistOutgoingVault(Player* player, uint8 vault)
{
    if (vault != EXTENDED_BANK_DEFAULT_VAULT)
    {
        // The Item objects are about to be freed, so this is one of the two places the core's
        // inventory save is both needed and safe.
        auto const itr = _sessions.find(player->GetGUID());
        if (itr != _sessions.end())
            FlushLiveVault(player, itr->second, true);

        return;
    }

    // A bank bag slot bought while this vault was open lives only in PLAYER_BYTES_2 --
    // HandleBuyBankSlotOpcode writes no row -- and SwitchTo is about to overwrite the live
    // count with the incoming vault's. Capture it before that happens or the purchase, and the
    // gold, are silently lost, and any bag already placed in that slot is mailed back on the
    // way in.
    SyncVaultBagSlots(player, vault);

    // The default vault IS character_inventory, so the core owns its positions. An item the
    // player rearranged inside the vanilla bank is queued and nowhere else; without this its
    // new slot would be lost when DetachLiveBank frees the Item.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
}

void ExtendedBankMgr::SwitchTo(Player* player, uint8 target)
{
    ObjectGuid const playerGuid = player->GetGUID();
    uint8 const current = GetActiveVault(playerGuid);

    if (current == target)
        return;

    EnsureDefaultVaultRow(player);

    // Everything the outgoing vault owns has to be on disk before its Item objects are freed.
    // Player::SaveToDB is deliberately NOT used for that: it returns without doing anything,
    // and without firing OnPlayerSave, while a far teleport is pending, which would leave this
    // code deleting items whose changes -- or, for ITEM_NEW, whose entire existence -- were
    // never written. Player::SaveInventoryAndGoldToDB has no such guard and is exactly the
    // half that matters here.
    PersistOutgoingVault(player, current);

    std::vector<ExtendedBankItemPos> live;
    CollectLiveBankItems(player, live);
    DetachLiveBank(player, live);

    // The incoming vault's bag slot count must be in place before anything is stored, or
    // Player::CanBankItem refuses its bank bags.
    if (ExtendedBankVault const* meta = FindVault(playerGuid, target))
        player->SetBankBagSlotCount(meta->BagSlots);

    AttachVault(player, target);

    if (target == EXTENDED_BANK_DEFAULT_VAULT)
    {
        _sessions.erase(playerGuid);
    }
    else
    {
        ExtendedBankSession& session = _sessions[playerGuid];
        session.ActiveVault = target;
        session.RangeCheckTimer = 0;
    }

    SyncSessionCount();
}

bool ExtendedBankMgr::OpenVault(Player* player, uint8 target, ObjectGuid bankerGuid)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ObjectGuid const playerGuid = player->GetGUID();

    if (!OwnsVault(playerGuid, target))
        return false;

    bool const switching = GetActiveVault(playerGuid) != target;

    if (switching)
    {
        if (player->IsInCombat())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("You cannot change bank vaults while in combat.");
            return false;
        }

        if (player->GetTradeData())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("You cannot change bank vaults while trading.");
            return false;
        }

        SwitchTo(player, target);
    }

    if (target != EXTENDED_BANK_DEFAULT_VAULT)
    {
        _sessions[playerGuid].BankerGuid = bankerGuid;
        SyncSessionCount();
    }

    // The bank slot GUID fields were only marked dirty; without an explicit flush they would
    // ship at the end of the world tick, after the client has already drawn the bank frame
    // with the previous vault's contents. Nothing changed when no swap happened, so the
    // already-open vault -- including the default one -- takes the plain vanilla path.
    if (switching)
        player->SendUpdateToPlayer(player);

    // Precautionary, NOT a verified fix. The stock banker option relies on the client closing
    // the gossip frame itself when SMSG_SHOW_BANK arrives, and that does work here. But a bank
    // frame opened behind a closing gossip frame was seen several times to come up with a
    // stale bank bag slot count -- buying a slot then updated the display one purchase behind
    // until the frame was reopened. The server's count was correct every time, so the effect
    // was cosmetic. A controlled A/B afterwards could not reproduce it, so the cause is not
    // established and this may be treating a symptom that has nothing to do with the gossip
    // frame. Closing explicitly first is the fallback the original design named for exactly
    // this, and it costs one packet, so it stays until someone reproduces the fault properly.
    player->PlayerTalkClass->SendCloseGossip();

    player->GetSession()->SendShowBank(bankerGuid);
    return true;
}

void ExtendedBankMgr::RevertToDefaultVault(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    if (GetActiveVault(player->GetGUID()) == EXTENDED_BANK_DEFAULT_VAULT)
        return;

    SwitchTo(player, EXTENDED_BANK_DEFAULT_VAULT);
}

void ExtendedBankMgr::FlushAndDetachForLogout(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    ObjectGuid const playerGuid = player->GetGUID();

    if (GetActiveVault(playerGuid) == EXTENDED_BANK_DEFAULT_VAULT)
        return;

    // Reverting properly would reload the default vault's entire contents from
    // character_inventory -- a blocking SELECT and up to ~280 Item constructions, plus
    // AttachVault's mail-back path as a failure surface -- only for LogoutPlayer to destroy
    // the Player moments later. All that is actually required is that the vault is persisted,
    // its items are out of the update queue, the bank slots are empty so nothing can be
    // mistaken for the default vault, and PLAYER_BYTES_2 carries the default vault's bag slot
    // count when the character is saved. character_inventory still holds the default vault's
    // rows untouched, so the next login restores it exactly as vanilla would.
    PersistOutgoingVault(player, GetActiveVault(playerGuid));

    std::vector<ExtendedBankItemPos> live;
    CollectLiveBankItems(player, live);
    DetachLiveBank(player, live);

    if (ExtendedBankVault const* meta = FindVault(playerGuid, EXTENDED_BANK_DEFAULT_VAULT))
        player->SetBankBagSlotCount(meta->BagSlots);

    _sessions.erase(playerGuid);
    SyncSessionCount();
}

namespace
{
    // The game enforces these caps by counting what is in character_inventory, and a stowed
    // vault is invisible to that count -- so a capped item parked in a vault lets its owner
    // acquire another one. Mirrors Player::CanTakeMoreSimilarItems (PlayerStorage.cpp:818),
    // including the 2147483647 sentinel that means "no limit" despite being positive.
    bool IsVaultRestricted(ItemTemplate const* proto)
    {
        if (!proto)
            return false;

        if (proto->ItemLimitCategory != 0)
            return true;

        return proto->MaxCount > 0 && proto->MaxCount != 2147483647;
    }
}

bool ExtendedBankMgr::EvictRestrictedItems(Player* player, std::vector<ExtendedBankItemPos> const& items,
    CharacterDatabaseTransaction trans, ObjectGuid bankerGuid)
{
    // A restricted *bag* drags its contents with it: they leave the bank inside it, so they
    // must leave the vault's rows too. Without this they keep their vault rows while no longer
    // being in the live bank, the rewrite below drops those rows, and nothing writes a
    // character_inventory row for them -- silent loss. 36 shipped bag templates are capped.
    std::set<ObjectGuid::LowType> restrictedBags;

    for (ExtendedBankItemPos const& pos : items)
        if (pos.Bag == 0 && IsVaultRestricted(pos.ItemPtr->GetTemplate()) && pos.ItemPtr->ToBag())
            restrictedBags.insert(pos.ItemPtr->GetGUID().GetCounter());

    std::vector<Item*> rejected;

    // Reverse order: a bag's contents must come out before the bag itself.
    for (auto itr = items.rbegin(); itr != items.rend(); ++itr)
    {
        bool const restricted = IsVaultRestricted(itr->ItemPtr->GetTemplate())
            || (itr->Bag != 0 && restrictedBags.find(itr->Bag) != restrictedBags.end());

        if (restricted)
            rejected.push_back(itr->ItemPtr);
    }

    if (rejected.empty())
        return false;

    ChatHandler handler(player->GetSession());
    std::vector<Item*> mailed;
    uint32 mailedCount = 0;
    std::string firstName;

    for (Item* item : rejected)
    {
        std::string const name = item->GetTemplate()->Name1;
        bool const capped = IsVaultRestricted(item->GetTemplate());

        // Same shape as WorldSession::HandleAutoStoreBankItemOpcode. CanStoreItem applies the
        // very cap that got the item rejected, so a player already holding the maximum gets it
        // by mail rather than silently keeping it in the vault.
        ItemPosCountVec dest;
        InventoryResult const err = player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);

        player->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);

        if (err == EQUIP_ERR_OK)
        {
            player->StoreItem(dest, item, true);

            if (capped)
            {
                handler.PSendSysMessage("Can't store {} in this vault, please use the Main one.", name);
                if (firstName.empty())
                    firstName = name;
            }
        }
        else
        {
            // The item may still carry the character_inventory row it had before being put in
            // the vault -- PersistVaultLayout has not run for it yet, and once it is in the
            // mail it will never appear in a live bank again for that row to be cleaned up.
            // Player::_LoadInventory does exactly this before mailing (PlayerStorage.cpp:6058);
            // without it the item exists in mail_items and character_inventory at once.
            item->DeleteFromInventoryDB(trans);
            item->RemoveFromUpdateQueueOf(player);
            player->DeleteRefundReference(item->GetGUID());

            if (item->IsInWorld())
            {
                item->RemoveFromWorld();
                item->DestroyForPlayer(player);
            }

            mailed.push_back(item);

            if (capped)
            {
                handler.PSendSysMessage("Can't store {} in this vault, please use the Main one. "
                    "Your bags are full, so it has been mailed to you.", name);

                if (firstName.empty())
                    firstName = name;
            }

            ++mailedCount;
        }
    }

    // Mailed in the caller's transaction, not one of our own: the same commit has to carry the
    // vault-row rewrite that drops these items, or a crash between two commits leaves the item
    // in mail_items and in mod_extended_bank_vault_items at the same time.
    while (!mailed.empty())
    {
        MailDraft draft("Bank vault", "This item cannot be stored in that vault, please use the Main one.");

        for (uint8 i = 0; !mailed.empty() && i < MAX_MAIL_ITEMS; ++i)
        {
            draft.AddItem(mailed.front());
            mailed.erase(mailed.begin());
        }

        draft.SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
    }

    // A chat line is not enough. The item leaves the cursor and disappears from the bank in the
    // same instant, which reads as item loss, and nobody is watching the chat frame mid-drag.
    // Nothing here changes what was stored -- it only makes sure the player knows where the
    // item went. See tools/TESTING.md for the swap-onto-an-occupied-slot route that reaches
    // the mail case.
    //
    // A boss whisper is the whole mechanism. ChatHandler::SendNotification was tried alongside
    // it and removed: observed in a stock client the two render almost identically, the whisper
    // stays on screen longer, and the whisper is the one addons hook for an alert sound.
    if (!firstName.empty())
    {
        std::string const notice = mailedCount
            ? Acore::StringFormat("{} was sent to your mailbox - it cannot be stored in this vault.", firstName)
            : Acore::StringFormat("{} cannot be stored in this vault - use the Main one.", firstName);

        // Skipped when the banker GUID is the player's own, which is the GM .bank convention
        // and has no creature behind it -- then the chat line above is all there is.
        if (bankerGuid && bankerGuid != player->GetGUID())
        {
            if (Creature* banker = ObjectAccessor::GetCreature(*player, bankerGuid))
                banker->Whisper(notice, LANG_UNIVERSAL, player, true);
        }
    }

    return true;
}

void ExtendedBankMgr::FlushLiveVault(Player* player, ExtendedBankSession& session, bool saveCoreInventory)
{
    std::vector<ExtendedBankItemPos> live;
    CollectLiveBankItems(player, live);

    // Opened up front so eviction shares the commit that rewrites the vault rows. Dropped
    // unused on the early return below, which discards its statements without executing them.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // Unconditional, not gated on the layout having changed: this also has to catch items
    // already sitting in a vault from before the rule existed, which arrive via AttachVault
    // and would otherwise look like an unchanged layout.
    bool const evicted = EvictRestrictedItems(player, live, trans, session.BankerGuid);
    if (evicted)
    {
        live.clear();
        CollectLiveBankItems(player, live);
    }

    uint8 const bagSlots = player->GetBankBagSlotCount();
    ExtendedBankLayoutDelta const delta = ComputeLayoutDelta(session, live, bagSlots);

    // Nothing moved and nothing is dirty: the vault's items are already out of the queue, so
    // there is nothing for _SaveInventory to get wrong and nothing to write.
    if (!delta.Any() && !evicted && !AnyItemQueued(live) && !saveCoreInventory)
        return;

    // An item that has left the vault since the last flush is about to lose its vault row,
    // and only the core knows how to write the character_inventory row saying where it went.
    // Without both in one transaction it belongs to neither table until the next periodic
    // save, and a crash in that window strands it. This is rare -- it needs an actual move
    // out of the bank -- so the cost of pulling in _SaveInventory is paid only then.
    bool const itemLeftVault = !delta.Removed.empty();

    // Writes only the rows the delta says changed. When it says none did, this just un-queues
    // the vault's items and writes their item_instance changes -- which is the whole of what
    // keeps the core from putting a vault item into character_inventory.
    PersistVaultLayout(player, session.ActiveVault, live, delta, trans);

    if (saveCoreInventory || itemLeftVault || evicted)
    {
        // Whatever is still queued after PersistVaultLayout is by definition not in this
        // vault -- most importantly an item just dragged OUT of it, whose new position only
        // the core can record, and whose Item object the caller is about to free. Doing it in
        // this transaction makes that move atomic with the vault rows it is leaving.
        player->SaveInventoryAndGoldToDB(trans);
    }

    CharacterDatabase.CommitTransaction(trans);
    CaptureLayout(session, live, bagSlots);
}

void ExtendedBankMgr::DrainUpdateQueue(Player* player)
{
    if (!_activeSessionCount.load())
        return;

    std::lock_guard<std::recursive_mutex> guard(_mutex);

    auto const itr = _sessions.find(player->GetGUID());
    if (itr == _sessions.end())
        return;

    // Hot path: runs before every packet and every player tick. It deliberately does NOT call
    // Player::SaveInventoryAndGoldToDB. Taking the vault's items out of the update queue is
    // the entire requirement -- _SaveInventory returns early on an empty queue and writes
    // nothing about items that are not in it. Pulling the core's inventory save in here as
    // well would run its buyback purge, its position cheat-detection (which can mark an item
    // ITEM_REMOVED) and its queue clear at arbitrary packet boundaries, hundreds of times a
    // minute, for no gain.
    FlushLiveVault(player, itr->second, false);
}

void ExtendedBankMgr::FlushActiveVault(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(_mutex);

    uint8 const active = GetActiveVault(player->GetGUID());

    if (active == EXTENDED_BANK_DEFAULT_VAULT)
    {
        // Nothing to move: character_inventory is already the default vault's storage. Only
        // the bag slot count needs keeping current, so that a crash while another vault was
        // open can be repaired at the next login.
        SyncVaultBagSlots(player, active);
        return;
    }

    // Deliberately not gated on the vault's metadata row existing. _SaveInventory runs six
    // lines after OnPlayerSave (PlayerStorage.cpp:7253 then :7261), so skipping the drain here
    // leaves live vault items in the update queue for it to write into character_inventory --
    // the one failure the whole design exists to prevent. A missing row is a reason to skip
    // the bookkeeping above, never the drain.
    DrainUpdateQueue(player);
}

void ExtendedBankMgr::UpdateRangeCheck(Player* player, uint32 diff)
{
    if (!_activeSessionCount.load())
        return;

    std::lock_guard<std::recursive_mutex> guard(_mutex);

    auto const itr = _sessions.find(player->GetGUID());
    if (itr == _sessions.end())
        return;

    ExtendedBankSession& session = itr->second;
    session.RangeCheckTimer += diff;

    if (session.RangeCheckTimer < EXTENDED_BANK_RANGE_CHECK_INTERVAL)
        return;

    session.RangeCheckTimer = 0;

    // A banker GUID equal to the player's own is the GM ".bank" case, which
    // WorldSession::CanUseBank also special-cases -- there is no creature to be in range of,
    // so range can never end the interaction. Map change and logout still revert it.
    if (session.BankerGuid == player->GetGUID())
        return;

    // The same check WorldSession::CanUseBank performs. 3.3.5a never tells the server that
    // the bank frame was closed, so losing the banker is the end-of-interaction signal.
    if (!player->GetNPCIfCanInteractWith(session.BankerGuid, UNIT_NPC_FLAG_BANKER))
        RevertToDefaultVault(player);
}

/* ------------------------------------------------------------------------ */
/* Buying and renaming                                                       */
/* ------------------------------------------------------------------------ */

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

    CharacterDatabase.Execute(
        "INSERT INTO mod_extended_bank_vaults (owner_guid, vault, name, bag_slots, created) "
        "VALUES ({}, {}, '', 0, UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE vault = VALUES(vault)",
        playerGuid.GetCounter(), next);

    _vaults[playerGuid].push_back(std::move(vault));

    handler.PSendSysMessage("Vault {} purchased for {} gold.", next, cost / EXTENDED_BANK_COPPER_PER_GOLD);
    return true;
}

void ExtendedBankMgr::RenameVault(Player* player, uint8 vault, std::string const& name)
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
        return;

    // Content is not filtered: colour codes and inline icons in a vault name are a feature.
    // Length is, though. utf8truncate cuts on a character boundary rather than a byte one, so
    // a multi-byte name can never be left with a split sequence, and it clears the string
    // outright when the input is not valid UTF-8 -- both of which keep malformed text out of
    // the client's gossip parser. An empty name falls back to "Vault N" in GetVaultName.
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

    utf8truncate(stored, EXTENDED_BANK_VAULT_NAME_MAX_CHARS);

    std::string escaped = stored;
    CharacterDatabase.EscapeString(escaped);

    CharacterDatabase.Execute(
        "UPDATE mod_extended_bank_vaults SET name = '{}' WHERE owner_guid = {} AND vault = {}",
        escaped, playerGuid.GetCounter(), vault);

    // Assign the same string that was written. Caching the untruncated name here is what
    // turned an over-length rename into a broken gossip window that survived the failed
    // UPDATE: the menu kept re-sending a name the database had rejected.
    meta->Name = std::move(stored);
}
