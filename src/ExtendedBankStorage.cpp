/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * The live bank: attaching a vault into the player's real bank slots, detaching it again, and
 * every write that decides which table an item belongs to.
 *
 * The invariant the whole module serves lives in this file. Vault 1 IS `character_inventory`;
 * vaults 2..N are `mod_extended_bank_vault_items`; nothing ever moves between the two tables.
 * ExtendedBankVaults.cpp holds the metadata table and the queries, none of which touch an item.
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
#include "WorldSession.h"
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

ExtendedBankMgr* ExtendedBankMgr::instance()
{
    static ExtendedBankMgr instance;
    return &instance;
}

namespace
{
    // The 11 item_instance columns Item::LoadFromDB expects in fields[0..10], in order,
    // followed by bag / slot / item guid / entry in fields[11..14]. Same shape as the core
    // CHAR_SEL_CHARACTER_INVENTORY projection.
    constexpr char const* ITEM_INSTANCE_COLUMNS =
        "ii.creatorGuid, ii.giftCreatorGuid, ii.count, ii.duration, ii.charges, ii.flags, "
        "ii.enchantments, ii.randomPropertyId, ii.durability, ii.playedTime, ii.text";
}

/* ------------------------------------------------------------------------ */
/* Item plumbing shared by attach, detach and eviction                       */
/* ------------------------------------------------------------------------ */

void ExtendedBankMgr::ReleaseItem(Player* player, Item* item)
{
    item->RemoveFromUpdateQueueOf(player);

    // Both are erase-if-present, so calling this after Player::RemoveItem -- which does
    // RemoveTradeableItem itself, but not DeleteRefundReference -- costs nothing. Doing both
    // is what makes the call safe on an item that never reached a slot: AttachVault runs
    // RestoreItemSideData *before* the placement attempt, so an item that then fails to place
    // is already registered in one or both. Left behind, the refund GUID makes _SaveInventory
    // complain every save, and the trade-list pointer is walked once a second by
    // Player::UpdateSoulboundTradeItems for an item that by then lives in the mail.
    player->DeleteRefundReference(item->GetGUID());
    player->RemoveTradeableItem(item);

    if (item->IsInWorld())
    {
        item->RemoveFromWorld();
        item->DestroyForPlayer(player);
    }
}

void ExtendedBankMgr::MarkClean(Player* player, Item* item)
{
    item->RemoveFromUpdateQueueOf(player);
    item->SetState(ITEM_UNCHANGED);
}

void ExtendedBankMgr::MailItemsBack(Player* player, std::vector<Item*>& items,
    CharacterDatabaseTransaction trans, char const* subject, char const* body)
{
    // MAX_MAIL_ITEMS is 12 and a full vault holds up to 280, so this has to be able to send
    // more than one letter. Always mailed in the caller's transaction, never one of its own:
    // the same commit has to carry whatever row change took these items out of the bank, or a
    // crash between the two commits leaves an item in mail_items and in a vault at once.
    for (std::size_t sent = 0; sent < items.size(); )
    {
        MailDraft draft(subject, body);

        for (uint8 i = 0; sent < items.size() && i < MAX_MAIL_ITEMS; ++i, ++sent)
            draft.AddItem(items[sent]);

        draft.SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
    }

    items.clear();
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

        // Bag::GetBagSize returns uint32, so a uint8 counter would be a hang if a bag could
        // report more than 255 slots. It cannot: Bag::Create refuses a template whose
        // ContainerSlots exceeds MAX_BAG_SIZE, which is 36 (Bag.cpp:73, Bag.h:22).
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable)
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
                    items[index].Bag, items[index].Slot);
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

    // In this transaction, not on its own: a bag slot bought moments ago and the rows of the
    // bag that went into it have to become true together.
    SyncVaultBagSlots(player, vault, trans);
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

bool ExtendedBankMgr::AnyItemQueued(std::vector<ExtendedBankItemPos> const& items)
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
        ReleaseItem(player, item);

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

QueryResult ExtendedBankMgr::QueryVaultContents(ObjectGuid::LowType lowGuid, uint8 vault) const
{
    if (vault == EXTENDED_BANK_DEFAULT_VAULT)
    {
        // The default vault is simply the bank half of character_inventory: the top level
        // bank slots, plus everything sitting inside a bag that occupies a bank bag slot.
        return CharacterDatabase.Query(
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

    // No self-heal here: it reads character_inventory on a synchronous connection while this
    // module's deletes of those same rows are committed asynchronously, so mid-session it can
    // see a row whose deletion is still queued and destroy a valid vault row.
    // SelfHealVaultRows runs once per login instead, where nothing is in flight.
    return CharacterDatabase.Query(
        "SELECT {}, v.bag, v.slot, v.item, ii.itemEntry "
        "FROM mod_extended_bank_vault_items v "
        "JOIN item_instance ii ON v.item = ii.guid "
        "WHERE v.owner_guid = {} AND v.vault = {} "
        "ORDER BY v.bag, v.slot",
        ITEM_INSTANCE_COLUMNS, lowGuid, vault);
}

void ExtendedBankMgr::AttachVault(Player* player, uint8 vault)
{
    ObjectGuid const playerGuid = player->GetGUID();

    // Records what is now live so the first flush of this vault can tell that nothing moved,
    // and pins the session's idea of which vault it holds before anything else can read it.
    auto const openSession = [&](std::vector<ExtendedBankItemPos> const& attached)
    {
        if (vault == EXTENDED_BANK_DEFAULT_VAULT)
            return;

        ExtendedBankSession& session = _sessions[playerGuid];
        session.ActiveVault = vault;
        CaptureLayout(session, attached, player->GetBankBagSlotCount());
        SyncSessionCount();
    };

    QueryResult result = QueryVaultContents(playerGuid.GetCounter(), vault);

    if (!result)
    {
        // An empty vault is still a known state; record it so the first flush can skip.
        openSession({});
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
                playerGuid.ToString(), itemEntry, vault);
            dropRow(itemGuid);
            Item::DeleteFromDB(trans, itemGuid);
            continue;
        }

        Item* item = NewItemOrBag(proto);
        if (!item->LoadFromDB(itemGuid, playerGuid, fields, itemEntry))
        {
            LOG_ERROR("module.extendedbank", "Player {} has a broken item (entry {}) in vault {}, removing.",
                playerGuid.ToString(), itemEntry, vault);
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
                        playerGuid.ToString(), itemGuid, vault);
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
                        playerGuid.ToString(), itemGuid, vault, bagGuid);
                }

                dropRow(itemGuid);
                ReleaseItem(player, item);
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
                        playerGuid.ToString(), itemGuid, vault);
                    continue;
                }
            }
        }

        if (err == EQUIP_ERR_OK)
        {
            // Cancels the ITEM_CHANGED that storing just queued, the same way
            // Player::_LoadInventory does, so nothing ever reaches character_inventory.
            MarkClean(player, item);
        }
        else
        {
            LOG_ERROR("module.extendedbank",
                "Player {} could not be given item {} from vault {} (reason {}); mailing it back.",
                playerGuid.ToString(), itemGuid, vault, uint32(err));

            dropRow(itemGuid);
            ReleaseItem(player, item);
            problematicItems.push_back(item);
        }
    } while (result->NextRow());

    MailItemsBack(player, problematicItems, trans,
        "Bank vault", "Some items could not be placed back into your bank.");

    // Un-queueing each item as it was stored is not enough on its own: Player::_StoreItem
    // marks the *containing bag* ITEM_CHANGED as well, so every bank bag is pushed back into
    // the update queue by the first item stored into it. The core sidesteps this by holding
    // m_itemUpdateQueueBlocked across the whole of _LoadInventory, which is private, so the
    // module sweeps the finished bank instead.
    std::vector<ExtendedBankItemPos> attached;
    CollectLiveBankItems(player, attached);

    for (ExtendedBankItemPos const& pos : attached)
        MarkClean(player, pos.ItemPtr);

    CharacterDatabase.CommitTransaction(trans);

    openSession(attached);
}

/* ------------------------------------------------------------------------ */
/* Switching                                                                 */
/* ------------------------------------------------------------------------ */

void ExtendedBankMgr::PersistOutgoingVault(Player* player, uint8 vault)
{
    if (vault != EXTENDED_BANK_DEFAULT_VAULT)
    {
        // A non-default vault is live only through a session -- GetActiveVault reads nothing
        // else -- so this lookup cannot miss for a vault the caller found active. The Item
        // objects are about to be freed, which makes this one of the two places the core's
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

    // AttachVault opens the session for a non-default target and is the only writer of
    // ActiveVault, so there is no state in which a vault is live behind a session that still
    // claims the default one. Deliberately a lookup rather than operator[] here: creating a
    // session at this point would be creating exactly that state.
    AttachVault(player, target);

    if (target == EXTENDED_BANK_DEFAULT_VAULT)
    {
        _sessions.erase(playerGuid);
    }
    else if (auto const itr = _sessions.find(playerGuid); itr != _sessions.end())
    {
        itr->second.RangeCheckTimer = 0;
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
        ExtendedBankSession& session = _sessions[playerGuid];

        // Rebinding to a different banker restarts the interval as well, so the player is not
        // measured against one they have only just walked to. Re-opening from the *same*
        // banker leaves the timer alone: resetting it there would let a client that repeats
        // the packet faster than once a second hold the range check off indefinitely.
        if (session.BankerGuid != bankerGuid)
        {
            session.BankerGuid = bankerGuid;
            session.RangeCheckTimer = 0;
        }

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
    uint8 const active = GetActiveVault(playerGuid);

    if (active == EXTENDED_BANK_DEFAULT_VAULT)
        return;

    // Reverting properly would reload the default vault's entire contents from
    // character_inventory -- a blocking SELECT and up to ~280 Item constructions, plus
    // AttachVault's mail-back path as a failure surface -- only for LogoutPlayer to destroy
    // the Player moments later. All that is actually required is that the vault is persisted,
    // its items are out of the update queue, the bank slots are empty so nothing can be
    // mistaken for the default vault, and PLAYER_BYTES_2 carries the default vault's bag slot
    // count when the character is saved. character_inventory still holds the default vault's
    // rows untouched, so the next login restores it exactly as vanilla would.
    PersistOutgoingVault(player, active);

    std::vector<ExtendedBankItemPos> live;
    CollectLiveBankItems(player, live);
    DetachLiveBank(player, live);

    if (ExtendedBankVault const* meta = FindVault(playerGuid, EXTENDED_BANK_DEFAULT_VAULT))
        player->SetBankBagSlotCount(meta->BagSlots);

    _sessions.erase(playerGuid);
    SyncSessionCount();
}

/* ------------------------------------------------------------------------ */
/* Flushing                                                                  */
/* ------------------------------------------------------------------------ */

namespace
{
    // Two independent reasons an item may not live anywhere but the Main Vault, both of them
    // the same shape: a stowed vault is invisible to a rule the game enforces elsewhere, so
    // parking an item there would buy its owner something the game does not sell.
    //
    // Both are statements about what a realm considers an exploit rather than about how the
    // storage works, so a realm is allowed to disagree with them -- see the bypass below and
    // the disclaimer that comes with it in conf/mod_extended_bank.conf.dist.
    //
    // 1. A capped item. The game enforces its cap by counting what is in character_inventory,
    //    which a stowed vault is not part of, so a capped item parked in one would let its
    //    owner acquire another.
    //
    //    Written as the exact negation of the "no maximum" test in
    //    Player::CanTakeMoreSimilarItems (PlayerStorage.cpp:818), sentinel and all: there,
    //    MaxCount == 2147483647 means "no limit" even for an item that also carries an
    //    ItemLimitCategory, so a predicate that read the two conditions independently would
    //    evict an item the game does not in fact cap. No shipped template sits in that corner
    //    -- both forms select the same 5802 rows -- but a custom one could.
    //
    // 2. An item with a duration. Its clock is driven by Player::UpdateItemDuration over
    //    m_itemDuration, and detaching a vault runs Player::RemoveItem, which calls
    //    RemoveItemDurations -- so a stowed vault freezes the timer outright.
    //
    //    The tempting objection is that vanilla already pauses these: UpdateItemDuration is
    //    called at login as UpdateItemDuration(time_diff, true) (PlayerStorage.cpp:5584), and
    //    realtimeonly skips anything without ITEM_FLAGS_CU_DURATION_REAL_TIME, so an ordinary
    //    duration item in the vanilla bank already stops ticking while its owner is logged
    //    out. That misses what the two pauses cost. Vanilla's is paid for in playing time:
    //    to stop the clock the player has to stop playing. A vault stops the same clock for
    //    free, while they carry on. Same effect, no price -- which is the definition of the
    //    thing this predicate exists to refuse, and it applies to every duration item, not
    //    just the 70 real-time-flagged ones a narrower rule would have caught.
    //
    //    The template is authoritative rather than the live ITEM_FIELD_DURATION, because
    //    Item::LoadFromDB forces the two into agreement anyway (Item.cpp:452).
    bool IsVaultRestricted(ItemTemplate const* proto)
    {
        if (!proto)
            return false;

        // The realm's override. One line is the whole of it because every caller asks this one
        // question and nothing branches on *why* an item was refused. Switching it back off
        // needs no migration either: FlushLiveVault re-asks for the live vault on the first
        // tick after it is opened, so each vault hands its offending items back the next time
        // it is used -- the same path items already in a vault took when this rule was first
        // introduced. What it cannot undo is a duplicate the bypass allowed to exist.
        if (sExtendedBankConfig.AllowRestrictedItems())
            return false;

        if (proto->Duration != 0)
            return true;

        if (proto->MaxCount == 2147483647)
            return false;

        return proto->MaxCount > 0 || proto->ItemLimitCategory != 0;
    }

    // Allocation-free precondition for EvictRestrictedItems, which cannot answer "nothing to
    // do" without first building a set of restricted bags and a vector of rejects. A bag's
    // uncapped contents are only ever evicted because the bag itself is capped, and the bag is
    // in this list too, so no restricted item can hide behind one.
    bool AnyItemRestricted(std::vector<ExtendedBankItemPos> const& items)
    {
        for (ExtendedBankItemPos const& pos : items)
            if (IsVaultRestricted(pos.ItemPtr->GetTemplate()))
                return true;

        return false;
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

    // Two names, not one, because the notice has to name an item that actually went where the
    // notice says it went. A single name plus a "did anything mail" counter gets this wrong in
    // one reachable combination: a capped *bag* drags its uncapped contents out with it, and if
    // the bag fits back into the player's bags while a loose item from inside it does not, the
    // counter is set by the loose item while the name comes from the bag -- so the whisper told
    // the player their bag had been mailed while it was sitting in their bag panel.
    std::string firstMailed;
    std::string firstRejected;

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
                if (firstRejected.empty())
                    firstRejected = name;
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
            ReleaseItem(player, item);

            mailed.push_back(item);

            // Not gated on `capped`: an uncapped item only reaches this branch by having been
            // inside a capped bag, and it is just as gone from the bank as the bag is.
            if (firstMailed.empty())
                firstMailed = name;

            if (capped)
            {
                handler.PSendSysMessage("Can't store {} in this vault, please use the Main one. "
                    "Your bags are full, so it has been mailed to you.", name);

                if (firstRejected.empty())
                    firstRejected = name;
            }
        }
    }

    MailItemsBack(player, mailed, trans,
        "Bank vault", "This item cannot be stored in that vault, please use the Main one.");

    // A chat line is not enough. The item leaves the cursor and disappears from the bank in the
    // same instant, which reads as item loss, and nobody is watching the chat frame mid-drag.
    // Nothing here changes what was stored -- it only makes sure the player knows where the
    // item went. See tools/TESTING.md for the swap-onto-an-occupied-slot route that reaches
    // the mail case.
    //
    // A boss whisper is the whole mechanism. ChatHandler::SendNotification was tried alongside
    // it and removed: observed in a stock client the two render almost identically, the whisper
    // stays on screen longer, and the whisper is the one addons hook for an alert sound.
    // Anything mailed is named first and named as mailed, because that is the case a player
    // cannot see the answer to. Only when nothing was mailed does the notice fall back to the
    // capped item that went into the bags, which the player can watch arrive.
    std::string const& subject = firstMailed.empty() ? firstRejected : firstMailed;

    if (!subject.empty())
    {
        std::string const notice = firstMailed.empty()
            ? Acore::StringFormat("{} cannot be stored in this vault - use the Main one.", subject)
            : Acore::StringFormat("{} was sent to your mailbox - it cannot be stored in this vault.", subject);

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

    uint8 const bagSlots = player->GetBankBagSlotCount();
    bool const restricted = AnyItemRestricted(live);

    // The idle fast path, and the reason this function is shaped the way it is. It runs from
    // OnPlayerUpdate on every tick and from CanPacketReceive on every packet, for as long as a
    // vault is open -- and a player parked at a banker can hold that state open indefinitely,
    // which `PreventAFKLogout = 2` turns from a contrivance into the ordinary case, since they
    // cannot log out until they move. Everything below this point allocates: a transaction
    // object, the eviction scan's set and vector, and the delta's hash map of up to 280
    // entries. None of it may happen on a tick where nothing has changed.
    //
    // These four checks are exactly as strong as computing the delta and throwing it away,
    // which is what this used to do:
    //
    //   - a bank layout cannot change without the core marking an item ITEM_CHANGED, so
    //     anything that moved *within* the vault is caught by AnyItemQueued;
    //   - an item that left the vault is queued but no longer in `live`, so AnyItemQueued
    //     would miss it -- the size comparison is what catches that, and it is the case the
    //     single-transaction flush exists for, so it must not be got wrong;
    //   - an item that entered is both queued and in `live`, and changes the size;
    //   - a purchased bag slot changes neither, hence the explicit comparison.
    if (!saveCoreInventory && !restricted
        && session.PersistedBagSlots == bagSlots
        && session.PersistedLayout.size() == live.size()
        && !AnyItemQueued(live))
    {
        return;
    }

    // Opened here so eviction shares the commit that rewrites the vault rows. Dropped unused
    // on the backstop return below, which discards its statements without executing them.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // Not gated on the layout having changed: this also has to catch items already sitting in
    // a vault from before the rule existed, which arrive via AttachVault and would otherwise
    // look like an unchanged layout. `restricted` is only the allocation-free half of the same
    // question, so it can gate the call without narrowing it.
    bool const evicted = restricted && EvictRestrictedItems(player, live, trans, session.BankerGuid);
    if (evicted)
    {
        live.clear();
        CollectLiveBankItems(player, live);
    }

    ExtendedBankLayoutDelta const delta = ComputeLayoutDelta(session, live, bagSlots);

    // Backstop. By the argument above nothing should reach here with an empty delta, but this
    // is the check that was load-bearing before the fast path was added, and leaving it costs
    // nothing: it is only evaluated on a tick that already decided it had work to do.
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
