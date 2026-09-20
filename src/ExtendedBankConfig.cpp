/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

#include "ExtendedBank.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include <algorithm>

// A constructor running at namespace scope can throw where nothing can catch it, and this one
// technically can, because it sizes a vector. Four elements, before the world exists, on a
// path where running out of memory would end the process anyway. This is also the shape every
// config singleton in AzerothCore has. Left alone on purpose.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
ExtendedBankConfig sExtendedBankConfig;

void ExtendedBankConfig::BuildConfigCache()
{
    SetConfigValue<bool>(ExtendedBankSetting::ENABLE, "ExtendedBank.Enable", true);
    SetConfigValue<uint32>(ExtendedBankSetting::MAX_VAULTS, "ExtendedBank.MaxVaults", 8);
    // These defaults must stay identical to the ones in the shipped config file. An
    // administrator who never copies that file gets these instead, so if the two drift apart
    // the feature quietly costs a different amount depending on whether somebody copied a
    // file.
    SetConfigValue<std::string>(ExtendedBankSetting::VAULT_COST, "ExtendedBank.VaultCost",
        "100,1000,2500,6000,18000,38000,90000");
    SetConfigValue<bool>(ExtendedBankSetting::ALLOW_RESTRICTED_ITEMS, "ExtendedBank.AllowRestrictedItems", false);

    // Announced on every startup and every reload, because this is the one setting here that
    // changes what the realm treats as an exploit rather than what the feature costs or how
    // big it is. A realm running with it on by accident has no other symptom to notice.
    if (GetConfigValue<bool>(ExtendedBankSetting::ALLOW_RESTRICTED_ITEMS))
    {
        LOG_WARN("module.extendedbank",
            "ExtendedBank.AllowRestrictedItems is enabled: capped and duration items may be stored in any vault. "
            "A stowed vault is counted by nothing and ticks no clocks, so both caps and durations stop applying "
            "to what is in one. See conf/mod_extended_bank.conf.dist.");
    }

    uint32 const configuredMax = GetConfigValue<uint32>(ExtendedBankSetting::MAX_VAULTS);
    uint8 const maxVaults = static_cast<uint8>(std::clamp<uint32>(configuredMax, 1, EXTENDED_BANK_VAULT_LIMIT));

    if (configuredMax != maxVaults)
    {
        LOG_WARN("module.extendedbank", "ExtendedBank.MaxVaults ({}) is out of range 1..{}, clamped to {}.",
            configuredMax, EXTENDED_BANK_VAULT_LIMIT, maxVaults);
    }

    std::vector<uint32> vaultCost;

    for (std::string_view token : Acore::Tokenize(GetConfigValue(ExtendedBankSetting::VAULT_COST), ',', false))
    {
        if (Optional<uint32> gold = Acore::StringTo<uint32>(token))
        {
            // Converting gold to copper overflows above about 429,496 gold, and an overflow
            // here turns an absurd price into a cheap one silently. A typo in the config would
            // then hand out vaults for pocket change instead of refusing to sell them.
            if (*gold > EXTENDED_BANK_MAX_VAULT_COST_GOLD)
            {
                LOG_ERROR("module.extendedbank",
                    "ExtendedBank.VaultCost entry '{}' exceeds the {} gold a character can hold, clamped.",
                    token, EXTENDED_BANK_MAX_VAULT_COST_GOLD);

                vaultCost.push_back(EXTENDED_BANK_MAX_VAULT_COST_GOLD * EXTENDED_BANK_COPPER_PER_GOLD);
            }
            else
            {
                vaultCost.push_back(*gold * EXTENDED_BANK_COPPER_PER_GOLD);
            }
        }
        else
        {
            LOG_ERROR("module.extendedbank",
                "ExtendedBank.VaultCost contains a non-numeric entry '{}', ignored.", token);
        }
    }

    if (vaultCost.empty())
    {
        LOG_ERROR("module.extendedbank",
            "ExtendedBank.VaultCost yielded no usable prices, falling back to 100 gold per vault.");
        vaultCost.push_back(100 * EXTENDED_BANK_COPPER_PER_GOLD);
    }

    std::lock_guard<std::mutex> guard(_configMutex);
    _maxVaults = maxVaults;
    _vaultCost.swap(vaultCost);
}

uint32 ExtendedBankConfig::GetVaultCost(uint8 vault) const
{
    std::lock_guard<std::mutex> guard(_configMutex);

    if (vault <= EXTENDED_BANK_DEFAULT_VAULT || _vaultCost.empty())
        return 0;

    // The list is indexed from vault 2. Shorter lists reuse their last price.
    std::size_t const index = std::min<std::size_t>(vault - EXTENDED_BANK_DEFAULT_VAULT - 1, _vaultCost.size() - 1);
    return _vaultCost[index];
}

class ExtendedBankWorldScript : public WorldScript
{
public:
    ExtendedBankWorldScript() : WorldScript("ExtendedBankWorldScript", {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD
    }) { }

    void OnBeforeConfigLoad(bool reload) override
    {
        sExtendedBankConfig.Initialize(reload);
    }
};

// ScriptMgr takes ownership in the ScriptObject constructor and deletes every
// registered script at shutdown (ScriptMgr.cpp:161). The analyser sees only the
// bare `new`, which is how every AzerothCore script is registered.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void AddExtendedBankConfigScripts()
{
    new ExtendedBankWorldScript();
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
