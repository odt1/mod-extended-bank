/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

/*
 * Where the module attaches to a character's life: logging in, being updated each tick,
 * changing map, being saved, logging out, and being deleted.
 *
 * Every hook here is one line handing off to the manager. The value is in the choice of hook
 * rather than in the code, so each comment explains why that particular moment and not
 * another one.
 */

#include "ExtendedBank.h"
#include "Player.h"
#include "ScriptMgr.h"

// Deliberately none of these check whether the module is switched on.
//
// The enable setting can be changed without restarting the server, so a character can log in
// while the module is off and still be playing when somebody turns it on. Had the login hook
// checked, that character would have no vault list loaded, and the menu would cheerfully
// offer to sell them a vault they already own.
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

    // This runs before the core loads the character's items, which is the only moment the
    // bank bag slot count can still be put right. Miss it and the core finds more bags than
    // slots and posts the difference to the player. See LoadPlayer for why that number can be
    // wrong in the first place.
    void OnPlayerLoadFromDB(Player* player) override
    {
        sExtendedBankMgr->LoadPlayer(player);
    }

    // The per-tick hook, and it is here for a specific reason rather than for tidiness. A few
    // lines after this fires, the same core function runs a periodic inventory save that does
    // not announce itself through any hook at all. Draining here is what gets in front of it.
    //
    // The range check shares the hook because this is also the only place that runs often
    // enough to notice a player walking away from a banker.
    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        sExtendedBankMgr->DrainUpdateQueue(player);
        sExtendedBankMgr->UpdateRangeCheck(player, p_time);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        sExtendedBankMgr->RevertToDefaultVault(player);
    }

    // Fires during a full character save, and crucially before the part that writes the
    // inventory, so the module gets to take its items off the pending-write list first. The
    // ordering is the whole reason this hook is useful.
    void OnPlayerSave(Player* player) override
    {
        sExtendedBankMgr->FlushActiveVault(player);
    }

    // Fires at the very start of logging out, before the final save, so the character record
    // that reaches disk always describes an empty bank and vault 1's bag slot count. That is
    // what makes a logged-out character indistinguishable from one on a realm without this
    // module.
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

// ScriptMgr takes ownership in the ScriptObject constructor and deletes every
// registered script at shutdown (ScriptMgr.cpp:161). The analyser sees only the
// bare `new`, which is how every AzerothCore script is registered.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void AddExtendedBankPlayerScripts()
{
    new ExtendedBankPlayerScript();
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
