/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * Debug commands. Every one is Console::Yes, so they can be driven over SOAP or the remote
 * console without a game client attached -- which is the only way to exercise a vault switch
 * from a test harness, since the normal route is a gossip click.
 *
 * `.vault check` deliberately only asserts things that are invisible from SQL: live memory
 * against the database. The purely relational invariants live in tools/check_invariants.sql,
 * which does not need a running server.
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

        // One read, so the vault this function checks and the row-presence flag it checks it
        // against cannot come from two different acquisitions of the manager's lock.
        ExtendedBankDebugState const state = sExtendedBankMgr->GetDebugState(guid);
        uint8 const active = state.ActiveVault;
        uint32 failures = 0;

        std::vector<ExtendedBankItemPos> live;
        sExtendedBankMgr->CollectLiveBankItems(player, live);

        // 1. No live bank item may sit in the update queue while a module-owned vault is
        //    loaded. If one does, Player::_SaveInventory will write it into
        //    character_inventory and collide with the default vault's row for that slot.
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

        // 2. The live bank must match whichever table owns the active vault, exactly.
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

        // 3. The reverse: nothing persisted for the active vault may be missing from the live
        //    bank. Only meaningful for a module-owned vault, where the module owns the table.
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

        // 4. No item may exist in character_inventory and a vault at the same time. This is the
        //    duplication vector, and the one thing the whole design exists to prevent.
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

        // 5. The live bank bag slot count must match what is stored for the active vault, or a
        //    crash here would make the core mail that vault's bank bags back at next login.
        //    A missing metadata row is only benign for the default vault on a character who
        //    has never bought or opened one -- SwitchTo calls EnsureDefaultVaultRow, so anyone
        //    who has switched has a row. For any other vault a missing row is itself the
        //    fault: it is what stops LoadPlayer restoring PLAYER_BYTES_2, after which the core
        //    mails that vault's bank bags back.
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

        // The player's own GUID as the banker is the GM ".bank" convention, which
        // WorldSession::CanUseBank already special-cases. UpdateRangeCheck honours the same
        // exception, so a vault opened this way stays loaded until a map change or logout.
        if (!sExtendedBankMgr->OpenVault(player, vault, player->GetGUID()))
        {
            handler->PSendSysMessage("Refused: {} does not own vault {}, or the switch was blocked.",
                player->GetName(), vault);
            return true;
        }

        handler->PSendSysMessage("{} now has vault {} loaded.", player->GetName(), vault);
        return true;
    }

    // Forces the revert that normally fires on walking out of range, so the auto-revert path
    // can be tested without waiting on a range check.
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

    // Forces the drain that normally runs before every packet, so its effect can be observed
    // in isolation rather than as a side effect of whatever the client happened to send.
    bool HandleVaultFlushCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        Player* player = ResolveTarget(handler, target);
        if (!player)
            return true;

        sExtendedBankMgr->DrainUpdateQueue(player);
        handler->PSendSysMessage("{}: drained.", player->GetName());
        return true;
    }

    // Same purchase the gossip option performs, including every refusal.
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

    // Takes the rest of the line verbatim, so colour codes and inline icons can be tested the
    // way a player would type them, and so the length limit can be driven past its boundary.
    //
    // The target is required and comes first: Tail has to be the last parameter, which leaves
    // nowhere unambiguous to put an optional name -- it would swallow the first word of the
    // new vault name instead.
    // Reordering has no client-driven route a harness can reach -- it is a gossip click -- so
    // this exists to make the menu order testable over SOAP alongside everything else.
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

        // Reporting GetVaultName unconditionally used to invent a success: it synthesises
        // "Vault N" for any number, so renaming a vault the character does not own printed
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
