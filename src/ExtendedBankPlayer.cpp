/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

#include "ExtendedBank.h"
#include "Player.h"
#include "ScriptMgr.h"

// No hook here is gated on the enable switch. ExtendedBank.Enable is hot-reloadable, so a
// character can log in while the module is off and be playing when it is switched on; gating
// the login hook would leave that character with no vault list in memory, and the gossip menu
// would then offer to sell them a vault they already own.
class ExtendedBankPlayerScript : public PlayerScript
{
public:
    ExtendedBankPlayerScript() : PlayerScript("ExtendedBankPlayerScript", {
        PLAYERHOOK_ON_LOAD_FROM_DB,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_MAP_CHANGED,
        PLAYERHOOK_ON_SAVE,
        PLAYERHOOK_ON_BEFORE_LOGOUT,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_DELETE_FROM_DB
    }) { }

    // Runs before Player::_LoadInventory, which is the only window in which the bank bag
    // slot count can still be corrected without the core mailing bank bags back.
    void OnPlayerLoadFromDB(Player* player) override
    {
        sExtendedBankMgr->LoadPlayer(player);
    }

    // Player::Update calls this at PlayerUpdates.cpp:315 and UpdateAdditionalSaves -- which
    // reaches _SaveInventory without firing OnPlayerSave -- at :340, so draining here closes
    // the periodic-save path.
    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        sExtendedBankMgr->DrainUpdateQueue(player);
        sExtendedBankMgr->UpdateRangeCheck(player, p_time);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        sExtendedBankMgr->RevertToDefaultVault(player);
    }

    // Fires from Player::SaveToDB before _SaveInventory, which is what lets the module take
    // its items out of the update queue before the core would write them into
    // character_inventory.
    void OnPlayerSave(Player* player) override
    {
        sExtendedBankMgr->FlushActiveVault(player);
    }

    // Fires at the very start of WorldSession::LogoutPlayer, before the final SaveToDB, so
    // the character is always saved with an empty bank and the default vault's bag slot count.
    void OnPlayerBeforeLogout(Player* player) override
    {
        sExtendedBankMgr->FlushAndDetachForLogout(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        sExtendedBankMgr->ForgetPlayer(player->GetGUID());
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        sExtendedBankMgr->DeleteCharacterData(trans, guid);
    }
};

void AddExtendedBankPlayerScripts()
{
    new ExtendedBankPlayerScript();
}
