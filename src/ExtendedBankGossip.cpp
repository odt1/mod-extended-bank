/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

#include "ExtendedBank.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "StringFormat.h"
#include "TradeData.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <vector>

namespace
{
    // Core's own DB-driven options always carry sender 0 (see Player::PrepareGossipMenu),
    // and this value is far outside the range any script uses, so a select can be routed
    // by sender alone.
    constexpr uint32 EXTENDED_BANK_SENDER = 0xEB000001;

    enum ExtendedBankGossipAction : uint32
    {
        EXTENDED_BANK_ACTION_OPEN_BASE   = 1000,
        EXTENDED_BANK_ACTION_RENAME_BASE = 2000,
        EXTENDED_BANK_ACTION_BUY         = 3000,
        EXTENDED_BANK_ACTION_RENAME_MENU = 3001,
        EXTENDED_BANK_ACTION_MAIN_MENU   = 3002
    };

    // A core-built option carried across the menu rebuild, with its sub-menu link.
    struct CarriedGossipItem
    {
        GossipMenuItem Item;
        GossipMenuItemData Data{};
        bool HasData{ false };
    };

    uint32 ToGold(uint32 copper)
    {
        return copper / EXTENDED_BANK_COPPER_PER_GOLD;
    }

    // GossipMenu::AddMenuItem asserts past GOSSIP_MAX_MENU_ITEMS, and the client cannot
    // render more than that either, so every add has to be able to refuse.
    bool CanAddMenuItem(Player* player)
    {
        return player->PlayerTalkClass->GetGossipMenu().GetMenuItemCount() < GOSSIP_MAX_MENU_ITEMS;
    }

    // Whether this module should take over an NPC's gossip.
    //
    // A creature with a ScriptName is deliberately left alone. ScriptMgr::OnGossipHello asks
    // every AllCreatureScript first and stops at the first one that returns true, so claiming
    // a scripted banker would stop its CreatureScript from ever running -- and a menu that
    // script builds in code, rather than in gossip_menu_option, cannot be carried across the
    // rebuild. Losing a custom NPC's options is worse than that NPC having no vault list.
    bool ClaimsBanker(Creature* creature)
    {
        return sExtendedBankConfig.IsEnabled()
            && creature->HasNpcFlag(UNIT_NPC_FLAG_BANKER)
            && !creature->GetScriptId();
    }
}

namespace
{
    void SendMainMenu(Player* player, Creature* creature)
    {
        ObjectGuid const playerGuid = player->GetGUID();
        std::vector<uint8> const owned = sExtendedBankMgr->GetOwnedVaults(playerGuid);

        // Build whatever this NPC would normally offer, exactly as the default gossip path
        // in NPCHandler does, so a banker that also innkeeps or vends keeps those options.
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        std::vector<CarriedGossipItem> carried;

        for (auto const& menuPair : menu.GetMenuItems())
        {
            // The vault list replaces the stock banker option; everything else is kept.
            if (menuPair.second.OptionType == GOSSIP_OPTION_BANKER)
                continue;

            CarriedGossipItem entry;
            entry.Item = menuPair.second;

            if (GossipMenuItemData const* data = menu.GetItemData(menuPair.first))
            {
                entry.Data = *data;
                entry.HasData = true;
            }

            carried.push_back(std::move(entry));
        }

        // GossipMenu::ClearMenu leaves the quest menu alone, unlike ClearGossipMenuFor.
        menu.ClearMenu();

        for (uint8 vault : owned)
        {
            if (!CanAddMenuItem(player))
                break;

            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
                sExtendedBankMgr->GetVaultName(playerGuid, vault),
                EXTENDED_BANK_SENDER, EXTENDED_BANK_ACTION_OPEN_BASE + vault);
        }

        uint8 const next = owned.back() + 1;

        if (next <= sExtendedBankConfig.GetMaxVaults() && CanAddMenuItem(player))
        {
            uint32 const cost = sExtendedBankConfig.GetVaultCost(next);

            AddGossipItemFor(player, GOSSIP_ICON_VENDOR,
                Acore::StringFormat("Buy Next Vault ({} gold)", ToGold(cost)),
                EXTENDED_BANK_SENDER, EXTENDED_BANK_ACTION_BUY,
                Acore::StringFormat("Purchase vault {} for {} gold?", next, ToGold(cost)),
                cost, false);
        }

        // Shown even with only the default vault: naming it is useful on its own, and there is
        // no reason the option should appear only after a purchase.
        if (CanAddMenuItem(player))
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Rename Vaults",
                EXTENDED_BANK_SENDER, EXTENDED_BANK_ACTION_RENAME_MENU);
        }

        // Re-add the NPC's own options below the vault list, keeping their sub-menu links.
        for (std::size_t i = 0; i < carried.size(); ++i)
        {
            CarriedGossipItem const& entry = carried[i];

            if (!CanAddMenuItem(player))
            {
                LOG_WARN("module.extendedbank",
                    "Gossip menu of creature {} is full; {} of its own options were dropped.",
                    creature->GetEntry(), carried.size() - i);
                break;
            }

            AddGossipItemFor(player, entry.Item.MenuItemIcon, entry.Item.Message, entry.Item.Sender,
                entry.Item.OptionType, entry.Item.BoxMessage, entry.Item.BoxMoney, entry.Item.IsCoded);

            if (entry.HasData && !menu.GetMenuItems().empty())
            {
                menu.AddGossipMenuItemData(menu.GetMenuItems().rbegin()->first,
                    entry.Data.GossipActionMenuId, entry.Data.GossipActionPoi);
            }
        }

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature);
    }
}

namespace
{
    void SendRenameMenu(Player* player, Creature* creature)
    {
        ObjectGuid const playerGuid = player->GetGUID();
        std::vector<uint8> const owned = sExtendedBankMgr->GetOwnedVaults(playerGuid);

        ClearGossipMenuFor(player);

        for (uint8 vault : owned)
        {
            if (!CanAddMenuItem(player))
                break;

            std::string const name = sExtendedBankMgr->GetVaultName(playerGuid, vault);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, name,
                EXTENDED_BANK_SENDER, EXTENDED_BANK_ACTION_RENAME_BASE + vault,
                Acore::StringFormat("Enter a new name for {}:", name), 0, true);
        }

        AddGossipItemFor(player, GOSSIP_ICON_TALK, "Back",
            EXTENDED_BANK_SENDER, EXTENDED_BANK_ACTION_MAIN_MENU);

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature);
    }
}

class ExtendedBankGossipScript : public AllCreatureScript
{
public:
    ExtendedBankGossipScript() : AllCreatureScript("ExtendedBankGossipScript") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (!ClaimsBanker(creature))
            return false;

        SendMainMenu(player, creature);
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (!sExtendedBankConfig.IsEnabled() || sender != EXTENDED_BANK_SENDER)
            return false;

        if (action > EXTENDED_BANK_ACTION_OPEN_BASE && action < EXTENDED_BANK_ACTION_RENAME_BASE)
        {
            uint8 const vault = static_cast<uint8>(action - EXTENDED_BANK_ACTION_OPEN_BASE);

            // On success the client closes the gossip frame itself when SMSG_SHOW_BANK
            // arrives, which is exactly what the stock banker option relies on.
            if (!sExtendedBankMgr->OpenVault(player, vault, creature->GetGUID()))
                CloseGossipMenuFor(player);

            return true;
        }

        switch (action)
        {
            case EXTENDED_BANK_ACTION_BUY:
                sExtendedBankMgr->BuyNextVault(player);
                SendMainMenu(player, creature);
                break;
            case EXTENDED_BANK_ACTION_RENAME_MENU:
                SendRenameMenu(player, creature);
                break;
            case EXTENDED_BANK_ACTION_MAIN_MENU:
                SendMainMenu(player, creature);
                break;
            default:
                CloseGossipMenuFor(player);
                break;
        }

        return true;
    }

    bool CanCreatureGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action,
        char const* code) override
    {
        if (!sExtendedBankConfig.IsEnabled() || sender != EXTENDED_BANK_SENDER)
            return false;

        if (action > EXTENDED_BANK_ACTION_RENAME_BASE && action < EXTENDED_BANK_ACTION_BUY)
        {
            uint8 const vault = static_cast<uint8>(action - EXTENDED_BANK_ACTION_RENAME_BASE);

            // RenameVault ignores a vault this character does not own.
            sExtendedBankMgr->RenameVault(player, vault, code ? code : "");
            SendRenameMenu(player, creature);
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }
};

// This hook does two unrelated jobs, both of which need to happen before a packet handler runs.
//
// First, most bankers carry UNIT_NPC_FLAG_BANKER without UNIT_NPC_FLAG_GOSSIP. For those the
// client never sends CMSG_GOSSIP_HELLO on right-click -- it sends CMSG_BANKER_ACTIVATE and the
// stock handler answers with SMSG_SHOW_BANK, so CanCreatureGossipHello above is never reached
// and the bank frame opens with no menu at all. Intercepting that opcode is what puts the vault
// menu in front of it, without altering any creature's npcflags.
//
// Second, it drains the live vault out of the item update queue. See DrainUpdateQueue below.
class ExtendedBankPacketScript : public ServerScript
{
public:
    ExtendedBankPacketScript() :
        ServerScript("ExtendedBankPacketScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return true;

        // Player::SaveToDB is not the only route to Player::_SaveInventory:
        // Player::SaveInventoryAndGoldToDB reaches it directly and fires no hook at all, and
        // mail, auction, guild bank, item refund and equip handlers all call it. Any of those
        // would write a REPLACE INTO character_inventory for a live vault item that an earlier
        // packet in the same batch had dirtied, whose (guid, bag, slot) then collides with the
        // default vault's row for that slot and deletes it. Draining before every handler runs
        // is the only interception point a module has. It costs one atomic load unless
        // this player actually has a vault open.
        sExtendedBankMgr->DrainUpdateQueue(player);

        // The one handler that saves somebody *else*. HandleAcceptTradeOpcode calls
        // trader->SaveInventoryAndGoldToDB (TradeHandler.cpp:660,668) on the partner, whose own
        // packets this hook never sees, so a vault item they dirtied within the last tick would
        // be written straight into their character_inventory. Both sides send CMSG_ACCEPT_TRADE
        // and only the second one completes the trade, so draining the partner on every one of
        // them covers whichever it turns out to be.
        //
        // Safe to touch another player's items from here: the opcode is PROCESS_THREADUNSAFE,
        // so it runs only in World::UpdateSessions, which does not overlap the MapUpdater pool.
        if (packet.GetOpcode() == CMSG_ACCEPT_TRADE)
        {
            if (TradeData* trade = player->GetTradeData())
                if (Player* trader = trade->GetTrader())
                    if (trader->IsInWorld())
                        sExtendedBankMgr->DrainUpdateQueue(trader);
        }

        if (packet.GetOpcode() != CMSG_BANKER_ACTIVATE || !sExtendedBankConfig.IsEnabled())
            return true;

        if (packet.size() < sizeof(uint64))
            return true;

        // Same guard the stock handler applies before opening the bank.
        ObjectGuid const bankerGuid(packet.read<uint64>(0));
        Creature* creature = player->GetNPCIfCanInteractWith(bankerGuid, UNIT_NPC_FLAG_BANKER);
        if (!creature || !ClaimsBanker(creature))
            return true;

        // The parts of HandleGossipHelloOpcode that matter for a menu being shown at all.
        player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TALK);
        creature->PauseMovementForInteraction();

        SendMainMenu(player, creature);

        // Swallow the packet: the bank frame is opened from the vault the player picks.
        return false;
    }
};

void AddExtendedBankGossipScripts()
{
    new ExtendedBankGossipScript();
    new ExtendedBankPacketScript();
}
