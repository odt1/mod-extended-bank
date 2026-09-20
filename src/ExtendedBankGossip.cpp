/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * The menu on the banker, and the packet hook that gets it in front of players.
 *
 * Two separate problems are solved here, and they look unrelated until you know why.
 *
 * The first is that a banker NPC has to offer a list of vaults where it would normally have
 * offered "let me see my bank". The module rebuilds the NPC's menu to do that, carefully
 * keeping whatever else the NPC offers, because plenty of bankers are also innkeepers or
 * vendors and nobody wants their stable master to stop working.
 *
 * The second is that most bankers never show a menu at all. Right-clicking one sends a
 * "open my bank" packet straight to the server, which answers by opening the bank window,
 * and no gossip code runs anywhere in that path. The only way in is to intercept that packet
 * before the core handles it, which is why this file also owns a packet hook.
 *
 * That same hook carries a second, entirely unrelated duty described at the bottom of the
 * file: it is the earliest point at which the module can protect an open vault from the
 * core's inventory save. Two jobs in one hook is not elegant, but both need to happen before
 * any packet is handled, and there is only one place that runs there.
 */

#include "ExtendedBank.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TradeData.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Stamped on every menu entry this module adds, so that when a player clicks something
    // the module can tell its own entries apart from the NPC's without guessing. Menu entries
    // the core builds from the database always carry zero here, and this value is far outside
    // anything a script would pick, so there is no chance of a collision.
    constexpr uint32 EXTENDED_BANK_SENDER = 0xEB000001;

    enum ExtendedBankGossipAction : uint32
    {
        EXTENDED_BANK_ACTION_OPEN_BASE   = 1000,
        EXTENDED_BANK_ACTION_RENAME_BASE = 2000,
        EXTENDED_BANK_ACTION_MOVE_BASE   = 3000,
        EXTENDED_BANK_ACTION_BUY         = 4000,
        EXTENDED_BANK_ACTION_MANAGE_MENU = 4001,
        EXTENDED_BANK_ACTION_MAIN_MENU   = 4002
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

    // A gossip menu holds 32 entries and the core asserts rather than truncating if you add a
    // 33rd, so every single add in this file has to be able to say no. The client cannot draw
    // more than 32 either, so there is nothing to be gained by trying.
    //
    // `reserve` holds slots back for entries the menu would be unusable without. The rename
    // list uses it to guarantee room for its own Back button, because a submenu a player
    // cannot leave is worse than one that is short an entry.
    bool CanAddMenuItem(Player* player, uint32 reserve = 0)
    {
        return player->PlayerTalkClass->GetGossipMenu().GetMenuItemCount() + reserve < GOSSIP_MAX_MENU_ITEMS;
    }

    // Whether this module should take over an NPC's menu at all.
    //
    // A banker that has its own script attached is deliberately left completely alone. The
    // core asks every module first and stops at the first one that claims the NPC, so taking
    // one over would stop its own script from ever running. On top of that, a menu built in
    // code rather than stored in the database cannot be read back and re-added, so the
    // rebuild below would silently throw it away. An NPC without a vault list is a much
    // smaller loss than an NPC whose custom behaviour has vanished.
    bool ClaimsBanker(Creature* creature)
    {
        return sExtendedBankConfig.IsEnabled()
            && creature->HasNpcFlag(UNIT_NPC_FLAG_BANKER)
            && !creature->GetScriptId();
    }

    // Builds and sends the vault list. Returns false when this player must not be offered one
    // on this creature, having left the menu exactly as the core built it and sent nothing, so
    // the caller can hand the NPC back and the result is identical to the module not existing.
    bool SendMainMenu(Player* player, Creature* creature)
    {
        ObjectGuid const playerGuid = player->GetGUID();
        std::vector<uint8> const owned = sExtendedBankMgr->GetOwnedVaults(playerGuid);

        // Ask the core to build whatever this NPC would normally offer, exactly as it would
        // without this module, so that a banker who is also an innkeeper or a vendor keeps
        // those options. They are re-added below the vault list further down.
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        std::vector<CarriedGossipItem> carried;

        // The vault list stands in for the NPC's normal "let me see my bank" option, so it may
        // only appear where that option would have. This is how the module inherits access
        // rules for free rather than reimplementing them.
        //
        // The core has already dropped any option whose conditions this player fails, and the
        // banker option has no further check beyond that, so one surviving into the built menu
        // is the core stating that this player may bank here. In the stock game exactly one
        // creature gates it: Jeeves, the engineering robot, who requires Engineering 350.
        // Before this check existed the vault list handed his bank to anybody who could reach
        // him.
        bool bankerOffered = false;

        for (auto const& menuPair : menu.GetMenuItems())
        {
            // The vault list replaces this one. Everything else the NPC offers is kept.
            if (menuPair.second.OptionType == GOSSIP_OPTION_BANKER)
            {
                bankerOffered = true;
                continue;
            }

            CarriedGossipItem entry;
            entry.Item = menuPair.second;

            if (GossipMenuItemData const* data = menu.GetItemData(menuPair.first))
            {
                entry.Data = *data;
                entry.HasData = true;
            }

            carried.push_back(std::move(entry));
        }

        // Nothing has been modified at this point, since the loop above only read, so the menu
        // the core built is still sitting there intact and ready for it to send.
        if (!bankerOffered)
            return false;

        // This clears the options only. The other obvious call for this also wipes the quest
        // list, which would make a banker who hands out quests stop offering them.
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

        // Offered even to a player who owns nothing but their original bank. Naming it is
        // useful on its own, and there is no reason to hide the option until after a
        // purchase.
        if (CanAddMenuItem(player))
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Rename and Reorder Vaults",
                EXTENDED_BANK_SENDER, EXTENDED_BANK_ACTION_MANAGE_MENU);
        }

        // Put the NPC's own options back, underneath the vault list, along with the links that
        // make their submenus work.
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
        return true;
    }

    // Renaming and reordering share a submenu because they are the two things a player does
    // to a vault rather than to what is inside it, and because a "move this up" entry is only
    // readable when it sits next to the name of the thing it moves.
    //
    // They are otherwise completely independent. A move never touches a name, a rename never
    // touches a position, and an unnamed vault keeps showing its number rather than its place
    // in the list, so moving one vault can never look like it renamed another.
    void SendManageMenu(Player* player, Creature* creature)
    {
        ObjectGuid const playerGuid = player->GetGUID();
        std::vector<uint8> const owned = sExtendedBankMgr->GetOwnedVaults(playerGuid);

        ClearGossipMenuFor(player);

        for (std::size_t index = 0; index < owned.size(); ++index)
        {
            uint8 const vault = owned[index];

            // Keep one slot for the Back button. A submenu the player cannot leave is worse
            // than one missing an entry. With the vault limit at 20 this can never actually
            // trigger, and it costs nothing to be certain.
            if (!CanAddMenuItem(player, 1))
                break;

            std::string const name = sExtendedBankMgr->GetVaultName(playerGuid, vault);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Acore::StringFormat("Rename \"{}\"", name),
                EXTENDED_BANK_SENDER, EXTENDED_BANK_ACTION_RENAME_BASE + vault,
                Acore::StringFormat("Enter a new name for {}:", name), 0, true);

            // The first entry is the pinned default vault, and the second sits directly below
            // it, so neither has anywhere to move. Offering the option only where it does
            // something keeps the top of the list from carrying a line that quietly does
            // nothing when clicked.
            if (index < 2)
                continue;

            if (!CanAddMenuItem(player, 1))
                break;

            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                Acore::StringFormat("Move \"{}\" Up", name),
                EXTENDED_BANK_SENDER, EXTENDED_BANK_ACTION_MOVE_BASE + vault);
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

        // Returning false hands the NPC back to the core, which rebuilds the menu from
        // scratch and sends it through its own path. That path has behaviour this module does
        // not reproduce, such as falling back to the quest list and picking the right greeting
        // text, so letting the core do it is what makes a refusal here completely
        // indistinguishable from the module not being installed. Rebuilding is safe because
        // it starts by clearing.
        return SendMainMenu(player, creature);
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (!sExtendedBankConfig.IsEnabled() || sender != EXTENDED_BANK_SENDER)
            return false;

        if (action > EXTENDED_BANK_ACTION_OPEN_BASE && action < EXTENDED_BANK_ACTION_RENAME_BASE)
        {
            uint8 const vault = static_cast<uint8>(action - EXTENDED_BANK_ACTION_OPEN_BASE);

            // No need to close the menu on success. The client closes it by itself when the
            // bank window opens, which is exactly what the stock banker option relies on.
            if (!sExtendedBankMgr->OpenVault(player, vault, creature->GetGUID()))
                CloseGossipMenuFor(player);

            return true;
        }

        if (action > EXTENDED_BANK_ACTION_MOVE_BASE && action < EXTENDED_BANK_ACTION_BUY)
        {
            uint8 const vault = static_cast<uint8>(action - EXTENDED_BANK_ACTION_MOVE_BASE);

            // The result is ignored on purpose. This menu only offers the option where it
            // does something, so a refusal means somebody sent a crafted packet, and the call
            // has already declined to write anything. Redrawing the menu is the right answer
            // either way, and shows the new order when there is one.
            sExtendedBankMgr->MoveVaultUp(player, vault);
            SendManageMenu(player, creature);
            return true;
        }

        switch (action)
        {
            // Getting here means the player stopped qualifying for the banker option while
            // the menu was already open, which in the stock game means unlearning Engineering
            // while standing in front of Jeeves. The menu already belongs to this module by
            // now and the core will not send a replacement, so close it rather than leave a
            // stale one on screen.
            case EXTENDED_BANK_ACTION_BUY:
                sExtendedBankMgr->BuyNextVault(player);
                if (!SendMainMenu(player, creature))
                    CloseGossipMenuFor(player);
                break;
            case EXTENDED_BANK_ACTION_MANAGE_MENU:
                SendManageMenu(player, creature);
                break;
            case EXTENDED_BANK_ACTION_MAIN_MENU:
                if (!SendMainMenu(player, creature))
                    CloseGossipMenuFor(player);
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

        if (action > EXTENDED_BANK_ACTION_RENAME_BASE && action < EXTENDED_BANK_ACTION_MOVE_BASE)
        {
            uint8 const vault = static_cast<uint8>(action - EXTENDED_BANK_ACTION_RENAME_BASE);

            // The result is ignored on purpose. This menu only ever lists vaults the character
            // owns, so a refusal means a crafted packet, and the rename has already declined
            // to write anything. Redrawing the menu is the right answer.
            sExtendedBankMgr->RenameVault(player, vault, code ? code : "");
            SendManageMenu(player, creature);
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }
};

// Two unrelated jobs share this hook, because both have to happen before any packet is
// handled and this is the only place that runs there.
//
// The first is getting the menu in front of the player at all. Most bankers are flagged as
// bankers without being flagged as talkers, and for those the client never asks for a
// conversation. Right-clicking sends "open my bank" straight to the server, the core answers
// by opening the bank window, and no gossip code runs anywhere along the way. Catching that
// one packet and answering with a menu instead is the only way in.
//
// The alternative would be adding the talk flag to every banker at runtime, which was
// rejected: the core writes that flag back to the world database when a GM saves a creature,
// so it would quietly become permanent on somebody's realm.
//
// The second job is the drain described at the top of ExtendedBank.h, and it is the reason
// every other packet passes through here too.
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

        // Exactly the check the core makes before opening a bank: the creature exists, is a
        // banker, and is close enough to talk to. Repeating it rather than trusting the packet
        // is what stops a crafted one opening a bank from across the map.
        ObjectGuid const bankerGuid(packet.read<uint64>(0));
        Creature* creature = player->GetNPCIfCanInteractWith(bankerGuid, UNIT_NPC_FLAG_BANKER);
        if (!creature || !ClaimsBanker(creature))
            return true;

        // The two things the core does before showing any menu, which matter here because
        // this path bypasses the code that would normally do them: break stealth-style auras
        // that end when you talk to someone, and stop the NPC wandering off mid-conversation.
        player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TALK);
        creature->PauseMovementForInteraction();

        // Any creature arriving here has no menu of its own, so its banker option comes from
        // a default that carries no conditions, and the access check inside will pass for
        // every ordinary banker.
        //
        // If it ever does not, letting the packet continue to the core is the right answer
        // rather than refusing. The core's own handler checks nothing except whether the
        // player may use a bank, so it would open this one regardless, and refusing here would
        // make the module stricter than the game itself.
        if (!SendMainMenu(player, creature))
            return true;

        // Swallow the packet so the core never opens the bank. From here on the bank window
        // is opened by whichever vault the player picks out of the menu.
        return false;
    }
};

// ScriptMgr takes ownership in the ScriptObject constructor and deletes every
// registered script at shutdown (ScriptMgr.cpp:161). The analyser sees only the
// bare `new`, which is how every AzerothCore script is registered.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void AddExtendedBankGossipScripts()
{
    new ExtendedBankGossipScript();
    new ExtendedBankPacketScript();
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
