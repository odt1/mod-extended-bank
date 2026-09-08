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

ExtendedBankConfig sExtendedBankConfig;

void ExtendedBankConfig::BuildConfigCache()
{
    SetConfigValue<bool>(ExtendedBankSetting::ENABLE, "ExtendedBank.Enable", true);
    SetConfigValue<uint32>(ExtendedBankSetting::MAX_VAULTS, "ExtendedBank.MaxVaults", 8);
    // Must stay identical to conf/mod_extended_bank.conf.dist: an administrator who never
    // copies the .conf.dist gets this list, and a mismatch silently reprices the feature.
    SetConfigValue<std::string>(ExtendedBankSetting::VAULT_COST, "ExtendedBank.VaultCost",
        "100,1000,2500,6000,18000,38000,90000");
    SetConfigValue<bool>(ExtendedBankSetting::ALLOW_RESTRICTED_ITEMS, "ExtendedBank.AllowRestrictedItems", false);

    // Said out loud on every startup and every reload, because it is the one setting here that
    // changes what the realm's rules are rather than how the feature is priced or sized, and
    // because a realm that has it on by accident has no other symptom to notice.
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
            // *gold * EXTENDED_BANK_COPPER_PER_GOLD wraps a uint32 above 429,496 gold, which
            // would turn an absurd price into a cheap one with no warning at all.
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

void AddExtendedBankConfigScripts()
{
    new ExtendedBankWorldScript();
}
