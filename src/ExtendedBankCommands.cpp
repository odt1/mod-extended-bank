/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * The `.vault` commands, which exist for testing rather than for players.
 *
 * Every one of them is available from the server console as well as in game, and that is the
 * point. The normal way to open a vault is to click a menu on an NPC, which no automated test
 * can do, so without these there would be no way to exercise a vault switch from a script.
 * With them, most of the test plan can be driven over a remote console with no game client
 * running at all.
 *
 * `.vault check` is the interesting one. It deliberately only tests things that cannot be
 * seen from SQL: whether the server's memory agrees with the database. Anything checkable by
 * querying the database alone lives in tools/check_invariants.sql instead, which has the
 * advantage of not needing a running server.
 */

#include "ExtendedBank.h"
#include "Bag.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    char const* StateName(ItemUpdateState state)
    {
        switch (state)
        {
            case ITEM_UNCHANGED: return "UNCHANGED";
            case ITEM_CHANGED:   return "CHANGED";
            case ITEM_NEW:       return "NEW";
            case ITEM_REMOVED:   return "REMOVED";
            default:             return "?";
        }
    }

    // Resolves the argument, or the selected player, or the invoker. Over SOAP there is no
    // session and no selection, so a name is required there.
    //
    // By value: every caller has its own Optional to give away, so taking a reference only to
    // copy it inside bought nothing.
    Player* ResolveTarget(ChatHandler* handler, Optional<PlayerIdentifier> resolved)
    {
        if (!resolved)
            resolved = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!resolved)
        {
            handler->SendSysMessage("No target. From the console, name a character: .vault info <name>");
            return nullptr;
        }

        if (!resolved->IsConnected())
        {
            handler->PSendSysMessage("{} is not online; this command needs a live Player.", resolved->GetName());
            return nullptr;
        }

        return resolved->GetConnectedPlayer();
    }

    uint32 CountVaultRows(ObjectGuid::LowType lowGuid, uint8 vault)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM mod_extended_bank_vault_items WHERE owner_guid = {} AND vault = {}",
            lowGuid, vault);

        return result ? (*result)[0].Get<uint32>() : 0;
    }

    bool HandleVaultInfoCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        ObjectGuid const guid = player->GetGUID();
        ExtendedBankDebugState const state = sExtendedBankMgr->GetDebugState(guid);

        handler->PSendSysMessage("--- extended bank: {} ({}) ---", player->GetName(), guid.ToString());
        handler->PSendSysMessage("Active vault: {}{}", sExtendedBankMgr->GetActiveVault(guid),
            state.HasSession
                ? Acore::StringFormat(" (session, banker {})", state.BankerGuid.ToString())
                : " (no session)");

        if (state.HasSession)
        {
            handler->PSendSysMessage("Last persisted: {} items, {} bag slots",
                state.PersistedItems, state.PersistedBagSlots);
        }

        handler->PSendSysMessage("Bank bag slots live: {}", player->GetBankBagSlotCount());

        for (uint8 vault : sExtendedBankMgr->GetOwnedVaults(guid))
        {
            handler->PSendSysMessage("  vault {}: bag_slots={} rows={} name='{}'",
                vault, sExtendedBankMgr->GetVaultBagSlots(guid, vault),
                CountVaultRows(guid.GetCounter(), vault), sExtendedBankMgr->GetVaultName(guid, vault));
        }

        std::vector<ExtendedBankItemPos> live;
        sExtendedBankMgr->CollectLiveBankItems(player, live);
        handler->PSendSysMessage("Live bank: {} items", live.size());

        for (ExtendedBankItemPos const& pos : live)
        {
            handler->PSendSysMessage("  bag={} slot={} item={} entry={} state={}{}",
                pos.Bag, pos.Slot, pos.ItemPtr->GetGUID().GetCounter(), pos.ItemPtr->GetEntry(),
                StateName(pos.ItemPtr->GetState()),
                pos.ItemPtr->IsInUpdateQueue() ? " QUEUED" : "");
        }

        return true;
    }

    bool HandleVaultCheckCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        ObjectGuid const guid = player->GetGUID();
        ObjectGuid::LowType const lowGuid = guid.GetCounter();

        // Read once and reused below. Asking twice would take the manager's lock twice, and
        // the answer could change in between, so the check would be comparing two different
        // moments and could report a fault that never existed.
        ExtendedBankDebugState const state = sExtendedBankMgr->GetDebugState(guid);
        uint8 const active = state.ActiveVault;
        uint32 failures = 0;

        std::vector<ExtendedBankItemPos> live;
        sExtendedBankMgr->CollectLiveBankItems(player, live);

        // 1. While a vault other than the first is open, none of its items may be waiting to
        //    be written by the core. If one is, the next inventory save writes it into the
        //    character's real bank and destroys whatever vault 1 had in that slot. This is
        //    the fault the entire module is built to prevent, so it is checked first.
        if (active != EXTENDED_BANK_DEFAULT_VAULT)
        {
            for (ExtendedBankItemPos const& pos : live)
            {
                if (pos.ItemPtr->IsInUpdateQueue() || pos.ItemPtr->GetState() != ITEM_UNCHANGED)
                {
                    handler->PSendSysMessage("FAIL queued-vault-item: item {} slot {} state {}",
                        pos.ItemPtr->GetGUID().GetCounter(), pos.Slot, StateName(pos.ItemPtr->GetState()));
                    ++failures;
                }
            }
        }

        // 2. What is actually in the bank slots must match what the database says is in this
        //    vault, item for item and slot for slot. A mismatch means a write was lost.
        for (ExtendedBankItemPos const& pos : live)
        {
            ObjectGuid::LowType const item = pos.ItemPtr->GetGUID().GetCounter();
            QueryResult result;

            if (active == EXTENDED_BANK_DEFAULT_VAULT)
            {
                result = CharacterDatabase.Query(
                    "SELECT 1 FROM character_inventory WHERE guid = {} AND item = {} AND bag = {} AND slot = {}",
                    lowGuid, item, pos.Bag, pos.Slot);
            }
            else
            {
                result = CharacterDatabase.Query(
                    "SELECT 1 FROM mod_extended_bank_vault_items "
                    "WHERE owner_guid = {} AND vault = {} AND item = {} AND bag = {} AND slot = {}",
                    lowGuid, active, item, pos.Bag, pos.Slot);
            }

            if (!result)
            {
                handler->PSendSysMessage("FAIL live-item-not-persisted: item {} bag {} slot {}",
                    item, pos.Bag, pos.Slot);
                ++failures;
            }
        }

        // 3. The same comparison in the other direction, catching rows the database holds
        //    that are not in the bank. Only meaningful for a vault the module owns, since
        //    vault 1's rows belong to the core and are none of this module's business.
        if (active != EXTENDED_BANK_DEFAULT_VAULT)
        {
            uint32 const rows = CountVaultRows(lowGuid, active);
            if (rows != live.size())
            {
                handler->PSendSysMessage("FAIL row-count-mismatch: vault {} has {} rows, live bank has {}",
                    active, rows, live.size());
                ++failures;
            }
        }

        // 4. No item may be listed in the character's inventory and in a vault at the same
        //    time. That is how an item becomes two items, and preventing it is what the whole
        //    design is for.
        if (QueryResult result = CharacterDatabase.Query(
            "SELECT v.vault, v.item FROM mod_extended_bank_vault_items v "
            "JOIN character_inventory ci ON ci.item = v.item WHERE v.owner_guid = {}", lowGuid))
        {
            do
            {
                Field* fields = result->Fetch();
                handler->PSendSysMessage("FAIL item-in-both-tables: vault {} item {}",
                    fields[0].Get<uint8>(), fields[1].Get<uint32>());
                ++failures;
            } while (result->NextRow());
        }

        // 5. The number of bank bag slots the character currently has must match what is
        //    recorded for the open vault. If it does not, a crash at this moment would leave
        //    the character owning fewer slots than they have bags, and the core would post the
        //    surplus bags to them at their next login.
        //
        //    A missing record is harmless in exactly one case: vault 1, on a character who has
        //    never bought or opened a vault, because nothing has had reason to write one yet.
        //    For any other vault a missing record is itself the fault, since it is what stops
        //    the slot count being restored at login.
        if (!state.HasVaultRow && active != EXTENDED_BANK_DEFAULT_VAULT)
        {
            handler->PSendSysMessage("FAIL missing-vault-row: vault {} is open with no metadata row.",
                active);
            ++failures;
        }
        else if (!state.HasVaultRow)
        {
            handler->PSendSysMessage("SKIP bag-slot-check: the Main Vault has no metadata row yet, "
                "which is expected until this character buys or opens a vault.");
        }
        else
        {
            uint8 const storedSlots = sExtendedBankMgr->GetVaultBagSlots(guid, active);
            if (storedSlots != player->GetBankBagSlotCount())
            {
                handler->PSendSysMessage("FAIL bag-slot-mismatch: vault {} stores {}, live is {}",
                    active, storedSlots, player->GetBankBagSlotCount());
                ++failures;
            }
        }

        if (!failures)
            handler->PSendSysMessage("OK: vault {} consistent, {} live items.", active, live.size());
        else
            handler->PSendSysMessage("{} failure(s).", failures);

        return true;
    }

    bool HandleVaultOpenCommand(ChatHandler* handler, uint8 vault, Optional<PlayerIdentifier> target)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        // Naming the player themselves as the banker is the convention the core already uses
        // for a GM opening their own bank with no NPC present. The range check honours the
        // same exception, so a vault opened this way stays open until the character changes
        // map or logs out, which is exactly what a test harness wants.
        if (!sExtendedBankMgr->OpenVault(player, vault, player->GetGUID()))
        {
            handler->PSendSysMessage("Refused: {} does not own vault {}, or the switch was blocked.",
                player->GetName(), vault);
            return true;
        }

        handler->PSendSysMessage("{} now has vault {} loaded.", player->GetName(), vault);
        return true;
    }

    // Triggers by hand the revert that normally happens when a player walks away from the
    // banker, so that path can be tested without scripting a character's movement.
    bool HandleVaultRevertCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        uint8 const before = sExtendedBankMgr->GetActiveVault(player->GetGUID());
        sExtendedBankMgr->RevertToDefaultVault(player);

        handler->PSendSysMessage("{}: vault {} -> {}.", player->GetName(), before,
            sExtendedBankMgr->GetActiveVault(player->GetGUID()));

        return true;
    }

    // Triggers by hand the drain that normally runs before every packet, so its effect can be
    // measured on its own rather than mixed in with whatever else the client was sending.
    bool HandleVaultFlushCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        sExtendedBankMgr->DrainUpdateQueue(player);
        handler->PSendSysMessage("{}: drained.", player->GetName());
        return true;
    }

    // Exactly the purchase the menu performs, refusals included, so the price table and the
    // out-of-money and at-the-limit cases can be tested without a client.
    bool HandleVaultBuyCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        uint32 const before = player->GetMoney();

        if (!sExtendedBankMgr->BuyNextVault(player))
        {
            handler->PSendSysMessage("Refused. Money unchanged: {} copper.", player->GetMoney());
            return true;
        }

        handler->PSendSysMessage("Bought. Money {} -> {} (spent {}).",
            before, player->GetMoney(), before - player->GetMoney());

        return true;
    }

    // Takes the rest of the line exactly as typed, so colour codes and inline icons can be
    // tested the way a player would enter them, and so the length limit can be pushed past its
    // boundary on purpose.
    //
    // The character name is required here and comes first, unlike the other commands where it
    // is optional. A parameter that swallows the rest of the line has to be last, which leaves
    // nowhere unambiguous for an optional name to go: it would eat the first word of the new
    // vault name instead.
    // Reordering is a menu click and nothing else, so a harness has no way to reach it. This
    // command exists purely so the menu order can be tested alongside everything else.
    bool HandleVaultMoveCommand(ChatHandler* handler, uint8 vault, Optional<PlayerIdentifier> target)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        if (!sExtendedBankMgr->MoveVaultUp(player, vault))
        {
            handler->PSendSysMessage(
                "Vault {} was not moved: {} does not own it, or it is already as high as it can go.",
                vault, player->GetName());
            return true;
        }

        std::string order;

        for (uint8 owned : sExtendedBankMgr->GetOwnedVaults(player->GetGUID()))
        {
            if (!order.empty())
                order += ", ";

            order += std::to_string(owned);
        }

        handler->PSendSysMessage("Vault {} moved up. Menu order is now: {}", vault, order);
        return true;
    }

    bool HandleVaultRenameCommand(ChatHandler* handler, PlayerIdentifier target, uint8 vault, Tail name)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        // Report what actually happened rather than what was asked for. This used to print
        // the vault's name unconditionally, and since that function invents "Vault N" for any
        // number you give it, renaming a vault the character did not own printed
        // `Vault 9 name is now 7 bytes: 'Vault 9'` while having written nothing at all.
        if (!sExtendedBankMgr->RenameVault(player, vault, std::string(name)))
        {
            handler->PSendSysMessage("{} does not own vault {}. Nothing was written.",
                player->GetName(), vault);
            return true;
        }

        std::string const stored = sExtendedBankMgr->GetVaultName(player->GetGUID(), vault);
        handler->PSendSysMessage("Vault {} name is now {} bytes: '{}'", vault, stored.size(), stored);

        return true;
    }
}

class ExtendedBankCommandScript : public CommandScript
{
public:
    ExtendedBankCommandScript() : CommandScript("ExtendedBankCommandScript") { }

    std::vector<ChatCommandBuilder> GetCommands() const override
    {
        static std::vector<ChatCommandBuilder> vaultCommandTable =
        {
            { "info",   HandleVaultInfoCommand,   SEC_ADMINISTRATOR, Console::Yes },
            { "check",  HandleVaultCheckCommand,  SEC_ADMINISTRATOR, Console::Yes },
            { "open",   HandleVaultOpenCommand,   SEC_ADMINISTRATOR, Console::Yes },
            { "revert", HandleVaultRevertCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "flush",  HandleVaultFlushCommand,  SEC_ADMINISTRATOR, Console::Yes },
            { "buy",    HandleVaultBuyCommand,    SEC_ADMINISTRATOR, Console::Yes },
            { "rename", HandleVaultRenameCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "move",   HandleVaultMoveCommand,   SEC_ADMINISTRATOR, Console::Yes },
        };

        static std::vector<ChatCommandBuilder> commandTable =
        {
            { "vault", vaultCommandTable },
        };

        return commandTable;
    }
};

// ScriptMgr takes ownership in the ScriptObject constructor and deletes every
// registered script at shutdown (ScriptMgr.cpp:161). The analyser sees only the
// bare `new`, which is how every AzerothCore script is registered.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void AddExtendedBankCommandScripts()
{
    new ExtendedBankCommandScript();
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
