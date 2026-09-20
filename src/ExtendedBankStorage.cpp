/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * This file moves items in and out of the character's bank slots, and it is the only file in
 * the module that creates, places or destroys an item. If you are looking for the code that
 * could lose somebody's belongings, it is all here.
 *
 * Read the block at the top of ExtendedBank.h first. The short version: the character's bank
 * slots are a fixed piece of memory that the core believes is their one real bank. Opening a
 * second vault means emptying those slots and filling them from the module's own table, while
 * making very sure the core never saves what it finds there.
 *
 * Three jobs, which the sections below follow:
 *
 *   attach   load a vault's rows from the database into the bank slots
 *   detach   take them back out and free them
 *   flush    write down where everything ended up
 *
 * Flushing is the subtle one. It has to write the module's rows and, when an item has left the
 * vault, get the core to write its row too, both inside a single transaction. Split them and
 * an item briefly belongs to neither table, which a crash turns into a lost item.
 *
 * ExtendedBankVaults.cpp is the quiet counterpart: names, prices, menu order, and nothing that
 * can touch an item.
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
    // The core's item loader reads its fields by position, not by name, so any query feeding
    // it has to produce exactly these columns in exactly this order. Copied from the core's
    // own inventory query for that reason. Reordering them here would not fail; it would
    // quietly load the wrong value into every item.
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

    // An item can be registered in two lists that outlive the slot it sits in: one tracking
    // what is still inside its refund window, one tracking what can still be traded to the
    // other players who were present when it dropped. Both removals do nothing if the item was
    // never listed, which is what lets this run unconditionally.
    //
    // Both are needed because loading a vault registers an item in these lists *before* trying
    // to put it in a slot, so an item that then fails to place is already in them. A refund
    // entry left behind makes every later save complain about an item that is no longer there.
    // A trade entry left behind means the server walks a pointer to freed memory once a
    // second.
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
    // A letter holds twelve items and a full vault holds up to 280, so one call here can turn
    // into a stack of letters.
    //
    // It writes into the caller's transaction rather than opening its own, and that is not
    // tidiness. The same commit has to carry both the mail and whatever change removed these
    // items from the vault. Kept apart, a crash landing between the two commits leaves the
    // item posted to the player *and* still listed in the vault, which becomes a duplicate the
    // moment they open it.
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

    // Order matters to every caller, and this is the one place it is decided. Each bag is
    // emitted immediately before the items inside it, so reading the list forwards places a
    // bag before anything that belongs in it, and reading it backwards empties a bag before
    // removing the bag itself. Get this wrong and loading a vault tries to put items into a
    // container that does not exist yet.
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
        // Only the rows that actually changed are written. An earlier version deleted the
        // whole vault and re-inserted every item on any change at all, so dragging a single
        // item inside a full vault cost over five hundred statements to record a difference of
        // one row.
        //
        // Every row that is about to move is deleted before any row is written back, and that
        // ordering is load-bearing. Swapping two items means writing each to a position the
        // other currently occupies. Interleave the deletes and inserts and the first insert
        // lands on a slot its previous occupant has not vacated, the unique key on
        // (owner, vault, bag, slot) rejects it, and the entire transaction is rolled back.
        // Because the write is committed in the background there is nothing to see in game:
        // the vault simply appears to forget the player's last drag. A row nobody touched
        // cannot trigger this, since a slot only frees up when whatever held it is itself in
        // the delete list.
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
            // Deliberately keyed on the item alone rather than on owner and vault. Item GUIDs
            // are unique across the whole server, so this says "whatever else the database
            // believes about these items, forget it". A stale row filing one of them under
            // another vault, or under another character after a deletion recycled the GUID,
            // would survive a narrower delete and then collide with the insert below.
            //
            // The obvious alternative, an upsert, is worse here. This table carries a second
            // unique key on (owner, vault, bag, slot), and an upsert colliding on *that* key
            // would quietly repoint an existing row at a different item. That loses an item.
            // Letting the transaction abort instead loses only the last edit, and login
            // repairs it.
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

        // These items have just become the module's responsibility, so the core's record of
        // where they used to be has to go. This is the same delete-by-item call that mail, the
        // auction house and the guild bank each make when an item leaves a player's hands, so
        // it is a well-trodden path rather than something the module invented.
        //
        // It cannot reach vault 1's rows even by accident, because vault 1's items are not
        // loaded while another vault is open. And only items that were somewhere else at the
        // last save need it: anything already in this vault had its row deleted back then, and
        // has had no inventory position since.
        for (std::size_t index : delta.Entered)
            items[index].ItemPtr->DeleteFromInventoryDB(trans);
    }

    // Every vault reaches this, vault 1 included. The item objects are about to be destroyed,
    // and their durability, charges or stack size may have changed since they were loaded, so
    // those have to be written down or the changes die with the objects.
    //
    // The removal on the first line is the important half. It takes each item off the list the
    // core saves from, so that when the core next writes this character's inventory it does
    // not see these items at all. That is the whole defence described at the top of
    // ExtendedBank.h, and it is one line.
    for (ExtendedBankItemPos const& pos : items)
    {
        pos.ItemPtr->RemoveFromUpdateQueueOf(player);
        pos.ItemPtr->SaveToDB(trans);
    }

    // Joined to this transaction rather than sent separately, so that a bank bag slot bought
    // moments ago and the bag now sitting in it become true at the same instant. Apart, a
    // crash between the two leaves a bag in a slot the character does not own, and the core
    // mails it back at the next login.
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

    // Keyed on the item rather than on the slot, which is what tells an item that moved
    // within the vault apart from one that arrived or left. Each live item is looked up and
    // then struck off, so whatever remains in the map at the end is exactly what the vault
    // used to hold and no longer does. No second scan needed to find it.
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
    // Backwards through the list, which by the ordering established in CollectLiveBankItems
    // empties each bag before removing the bag.
    for (auto itr = items.rbegin(); itr != items.rend(); ++itr)
    {
        Item* item = itr->ItemPtr;

        // Removing an item this way is free. It touches memory and the client's view only,
        // and never marks the item as needing a write, so emptying the bank slots costs
        // nothing in database terms. That is what makes switching vaults cheap enough to do
        // every time somebody walks away from a banker.
        //
        // The `false` suppresses a per-item update packet. With it set, the core would build
        // and send a full description of an item that is destroyed on the very next line. The
        // client still learns the slots are empty: OpenVault pushes all of them together once
        // the swap has finished.
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
        // Vault 1 has no table of its own, so "read vault 1" means reading the bank-shaped
        // part of the core's inventory table: the bank slots themselves, plus the contents of
        // any bag sitting in one of the bank's bag slots. The self-join is what finds that
        // second part, since a bag's contents record only which bag they are in and not where
        // that bag is.
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

    // Deliberately no consistency check here, tempting as it is. Reads and writes travel over
    // different database connections and the module's writes commit in the background, so a
    // check run mid-session can see a row whose deletion is still sitting in a queue. It would
    // then "repair" a vault row that was perfectly valid. The equivalent check runs once at
    // login instead, when nothing is in flight.
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

    // Called once the bank slots are filled. It writes down which vault is now open and what
    // it looked like on arrival, so the next save can tell "the player moved something" from
    // "nothing has happened since I loaded this" without asking the database. A player parked
    // at a banker doing nothing is the common case, and this is what makes it free.
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
        // An empty vault is a perfectly good state and still needs recording, otherwise the
        // first save cannot tell it apart from a vault whose contents have not loaded.
        openSession({});
        return;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    std::map<ObjectGuid::LowType, Bag*> bagMap;
    std::set<ObjectGuid::LowType> unusableBags;
    std::vector<Item*> problematicItems;

    // Forgets where an item was, in whichever of the two tables owns this vault. Every failure
    // path below has to call it. Skip it and the bad row stays in the database, so the same
    // failure repeats every single time the player opens this vault, forever.
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
            // The item refers to a template the server does not have, usually because a
            // custom item was removed from the database. There is nothing to load, so follow
            // what the core does in the same situation and delete both the position and the
            // item itself, rather than logging the same error on every future visit.
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

            // Marking an item removed and saving it makes the core destroy the object for us.
            // Deleting it here as well would be a double free.
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
                // This item claims to live inside a bag that is not here. Either the bag's
                // own row is missing, or the bag failed to fit and has already been posted to
                // the player. Either way the contents follow it into the mail. Dropping them
                // instead would leave their rows behind to fail again on every future visit,
                // and the player would never see the items again.
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
            // Putting an item in a slot marks it as needing a write. Undo that immediately,
            // exactly as the core does after loading an item from the database, because a
            // write here would put a vault item into the character's real inventory.
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

    // Cleaning each item as it was placed is not enough, and this sweep is the reason why.
    // Putting an item into a bag marks the *bag* as changed too, so the first item stored into
    // each bank bag dirties the bag again behind us. The core avoids the problem by switching
    // the whole mechanism off while it loads a character, using a flag the module cannot
    // reach, so the module walks the finished bank once more instead.
    //
    // Miss this and the bags themselves get written into the character's real inventory.
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
        // A vault other than the first is only ever open by way of a session, so if the
        // caller found one active then the lookup below cannot fail.
        //
        // The `true` pulls the core's own inventory save into the same transaction. That is
        // expensive and has side effects, so it happens in only two places in the module, both
        // of them moments before the item objects are destroyed. Here is one of them: after
        // this, anything not written down is gone.
        auto const itr = _sessions.find(player->GetGUID());
        if (itr != _sessions.end())
            FlushLiveVault(player, itr->second, true);

        return;
    }

    // Buying a bank bag slot only changes a counter on the character in memory. Nothing
    // writes it down, and the switch that is about to happen will overwrite that counter with
    // the incoming vault's. Record it here or the player's gold is gone and the slot is not,
    // and any bag they already put in that slot gets mailed back to them on the way in.
    SyncVaultBagSlots(player, vault);

    // Vault 1's positions belong to the core, so the module cannot write them itself. An item
    // the player has just rearranged inside their normal bank exists only as a pending change
    // in memory, and the next step destroys the objects holding it. Asking the core to save
    // now is the only way that rearrangement survives.
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

    // Everything the outgoing vault owns has to reach the database before its item objects
    // are destroyed a few lines below.
    //
    // The obvious call for that, the core's full character save, is deliberately avoided. It
    // quietly does nothing at all while a long-distance teleport is pending, and it does not
    // even run the module's hook on the way out. Trusting it would mean destroying items whose
    // changes were never written, or in the case of something looted seconds ago, destroying
    // items that had never been written at all. The narrower inventory save used here has no
    // such escape hatch.
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

    // Swapping the slots only marked them as changed. Left alone they would reach the client
    // at the end of the world tick, which is after the bank window has already been drawn, and
    // the player would see the previous vault's contents for an instant. Pushing them now is
    // what makes the switch look instantaneous. Reopening the vault that is already open
    // changed nothing, so it takes the ordinary path instead.
    if (switching)
        player->SendUpdateToPlayer(player);

    // Precautionary, and honestly not a proven fix. The stock banker never closes the gossip
    // window explicitly; the client closes it by itself when the bank opens, and that works
    // here too.
    //
    // But a bank window opening behind a closing gossip window was seen more than once to show
    // a stale bank bag slot count, so buying a slot appeared to do nothing until the window was
    // reopened. The server's count was right every time, so nothing was actually lost. A
    // careful attempt to reproduce it afterwards failed, which means the cause is unknown and
    // this line may be treating a symptom of something else entirely. It costs one packet, so
    // it stays until somebody pins the fault down properly.
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

    // Logging out is the one case where the module does *not* put vault 1 back, and the
    // reason is that it would be pure waste. A proper revert means a blocking query, building
    // up to 280 item objects, and exposing the whole failure path that can post items to the
    // player, all so the core can destroy the character a few milliseconds later.
    //
    // Only four things actually have to be true when the character record is written: the open
    // vault is saved, its items are off the pending-write list, the bank slots are empty so
    // nothing can be mistaken for vault 1, and the bag slot count belongs to vault 1 rather
    // than to whatever was open. Vault 1's own rows were never touched, so the next login
    // loads it exactly as it would without this module installed.
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
    // Two kinds of item are refused from every vault except the first, for the same underlying
    // reason. A vault that is not currently open is invisible to the rest of the server. Any
    // rule the game enforces by looking at what a character is carrying stops applying to
    // whatever is parked in one, so storing certain items there would hand their owner
    // something the game does not otherwise sell.
    //
    // Neither rule is about how the storage works. Both are opinions about what counts as an
    // exploit, which is a realm's decision rather than this module's, so there is a setting to
    // switch them off. The config file argues the case at length.
    //
    // 1. Items the game only lets you have so many of: uniques, quest items, and anything in a
    //    limited category. The game enforces those caps by counting what the character is
    //    carrying, and a stowed vault is not part of that count, so parking one there lets the
    //    owner go and acquire another.
    //
    //    The test below is deliberately the exact mirror image of the core's own "is this
    //    capped?" check, sentinel value included. In the core, a maximum of 2147483647 means
    //    "no limit at all", even on an item that also belongs to a limited category. Testing
    //    the two conditions independently would therefore throw out items the game does not
    //    actually cap. Nothing Blizzard shipped sits in that corner, but a custom item could.
    //
    // 2. Items with a countdown. The clock only advances while the item is loaded, and stowing
    //    a vault unloads it, so a stowed vault stops the timer outright.
    //
    //    The obvious objection is that the game already does this: an item in a normal bank
    //    stops ticking while the character is logged out. True, but it misses what each pause
    //    costs. The game's pause is paid for in playing time, because stopping the clock means
    //    stopping playing. A vault stops the same clock for nothing while its owner carries on,
    //    and the item is still one banker visit away. Same benefit, no price, which is exactly
    //    what this refusal exists to prevent. That argument holds for every item with a
    //    countdown, not only for the handful whose timers run in real time.
    //
    //    The item template decides this rather than the item's own remaining time, because the
    //    core forces the two to agree when it loads an item anyway.
    bool IsVaultRestricted(ItemTemplate const* proto)
    {
        if (!proto)
            return false;

        // The realm's override, and one line is genuinely the whole of it. Every caller asks
        // this same question and nothing anywhere branches on *why* an item was refused, so
        // turning the answer off turns the entire feature off.
        //
        // Switching it back on needs no migration either. Each vault re-checks its contents the
        // first time it is opened, so offending items are handed back then, using the same path
        // that cleared out vaults filled before this rule existed. The one thing no switch can
        // undo is a duplicate that the bypass allowed somebody to create.
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
    // A refused bag takes its contents with it, so those contents have to be struck from the
    // vault's rows as well. Without this they keep rows saying they are in a vault while no
    // longer being in the bank, the rewrite further down deletes those rows, and nothing ever
    // records where they went. The items are simply gone. Thirty-six of the bags Blizzard
    // ships are refusable, so this is not a hypothetical.
    std::set<ObjectGuid::LowType> restrictedBags;

    for (ExtendedBankItemPos const& pos : items)
        if (pos.Bag == 0 && IsVaultRestricted(pos.ItemPtr->GetTemplate()) && pos.ItemPtr->ToBag())
            restrictedBags.insert(pos.ItemPtr->GetGUID().GetCounter());

    std::vector<Item*> rejected;

    // Backwards, so a bag is emptied before the bag itself is taken out.
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

    // Two names rather than one, so that the message names an item which actually went where
    // the message says it went.
    //
    // Tracking a single name plus a "did anything get mailed?" flag looks equivalent and is
    // not. A refused bag drags its contents out with it, and if the bag fits back into the
    // player's bags while one loose item from inside it does not, the flag gets set by the
    // loose item while the name comes from the bag. The player is then told their bag was
    // posted to them while they can see it sitting in their bag panel.
    std::string firstMailed;
    std::string firstRejected;

    for (Item* item : rejected)
    {
        std::string const name = item->GetTemplate()->Name1;
        bool const capped = IsVaultRestricted(item->GetTemplate());

        // Try the bags first, exactly as the core does when a player drags something out of
        // the bank by hand. Note that this check applies the very cap that got the item
        // refused in the first place, so somebody already carrying the maximum gets it posted
        // to them instead of quietly keeping it in the vault.
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
            // This item may still have a row saying it is in the character's inventory, left
            // over from wherever it was before it was dropped into the vault. Normally that row
            // is cleaned up when the vault is saved, but this item is about to go into the
            // mail and will never appear in a bank again for that to happen.
            //
            // Delete it here or the item exists in the mail and in the inventory at the same
            // time, which is a duplicate. The core does exactly this before posting an item it
            // could not load.
            item->DeleteFromInventoryDB(trans);
            ReleaseItem(player, item);

            mailed.push_back(item);

            // Recorded even for an item that is not itself refused. The only way one of those
            // reaches here is by having been inside a bag that was, and it is just as gone
            // from the bank as the bag is, so the player still needs telling.
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

    // The chat line above is not enough on its own. From the player's side, the item leaves
    // the cursor and vanishes from the bank in the same instant, which looks exactly like
    // losing it, and nobody is reading the chat frame in the middle of a drag. Nothing here
    // changes what was stored. It only makes sure the player finds out where their item went.
    //
    // A whisper from the banker is the mechanism, sent in the style bosses use for their
    // announcements. On a stock client that renders as a notice across the middle of the
    // screen rather than as speech, stays up longer than the alternatives, and is the form
    // addons already listen for to play an alert sound.
    //
    // Anything posted to the player is named first and named as posted, because that is the
    // case they cannot work out for themselves. Only when nothing was mailed does this fall
    // back to naming an item that went into their bags, which they can watch arrive.
    std::string const& subject = firstMailed.empty() ? firstRejected : firstMailed;

    if (!subject.empty())
    {
        std::string const notice = firstMailed.empty()
            ? Acore::StringFormat("{} cannot be stored in this vault - use the Main one.", subject)
            : Acore::StringFormat("{} was sent to your mailbox - it cannot be stored in this vault.", subject);

        // A GM can open a vault on themselves with no banker involved, in which case the
        // "banker" is the player and there is no creature to speak. The chat line is then the
        // only notice, which is fine for the one audience that gets it.
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

    // The early exit below is the reason this whole function is shaped the way it is.
    //
    // It runs on every server tick and on every packet the player sends, for as long as their
    // vault stays open, and a player can leave one open indefinitely by standing still. On a
    // realm that stops people logging out while away from keyboard, that stops being a
    // contrived case and becomes the normal one.
    //
    // Everything past this point allocates: a transaction, the sets and vectors the eviction
    // scan needs, and a hash map of up to 280 entries for the comparison. None of that may
    // happen on a tick where nothing has changed.
    //
    // These four checks are exactly as strong as computing the delta and throwing it away,
    // which is what this used to do:
    //
    //   - a bank layout cannot change without the core marking an item ITEM_CHANGED, so
    //     anything that moved *within* the vault is caught by AnyItemQueued;
    //   - an item that left the vault is queued but no longer in `live`, so AnyItemQueued
    //     would miss it. The size comparison is what catches that, and since it is the exact
    //     case the single-transaction save exists for, getting it wrong loses an item;
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

    // An item that has left the vault is about to lose the row saying it was here, and only
    // the core can write the row saying where it has gone instead. Unless both happen in one
    // transaction the item belongs to neither table until the next routine save, and a crash
    // inside that window strands it: the item still exists, but nothing points at it and only
    // a GM can get it back.
    //
    // This needs somebody to actually drag something out of the bank, so it is rare, and the
    // expense of involving the core's save is paid only when it happens.
    bool const itemLeftVault = !delta.Removed.empty();

    // Writes only the rows that changed. When none did, this still takes the vault's items off
    // the core's pending-write list and records any change to the items themselves, which is
    // the whole of what keeps a vault item out of the character's real inventory.
    PersistVaultLayout(player, session.ActiveVault, live, delta, trans);

    if (saveCoreInventory || itemLeftVault || evicted)
    {
        // Anything still waiting to be written at this point is, by definition, not in this
        // vault any more. The important example is an item the player has just dragged out of
        // it: only the core can record its new home, and the caller is about to destroy the
        // object. Writing it inside this transaction is what makes leaving a vault a single
        // atomic step rather than two.
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

    // This runs before every single packet and on every player tick, so it is as hot as code
    // in this module gets, and it deliberately does not involve the core's inventory save.
    //
    // Taking the vault's items off the pending-write list is the entire requirement. The core
    // writes nothing about items it cannot see there, so that alone is the whole defence.
    // Calling its save here as well would additionally purge vendor buyback slots, run a
    // cheat check that can mark an item destroyed, and wipe the pending-write list, hundreds
    // of times a minute, at arbitrary moments, for no benefit at all.
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

    // A GM can open their own bank with no banker present, and the core recognises that by
    // the "banker" being the player themselves. There is then no creature to walk away from,
    // so distance can never end the visit. Changing map or logging out still does.
    if (session.BankerGuid == player->GetGUID())
        return;

    // The same check WorldSession::CanUseBank performs. 3.3.5a never tells the server that
    // the bank frame was closed, so losing the banker is the end-of-interaction signal.
    if (!player->GetNPCIfCanInteractWith(session.BankerGuid, UNIT_NPC_FLAG_BANKER))
        RevertToDefaultVault(player);
}
