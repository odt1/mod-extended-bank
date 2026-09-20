/*
 * Copyright (C) 2026 odt1
 *
 * Released under the GNU General Public License v2, the same licence as
 * AzerothCore. See LICENSE in the root of this repository.
 */

void AddExtendedBankConfigScripts();
void AddExtendedBankGossipScripts();
void AddExtendedBankCommandScripts();
void AddExtendedBankPlayerScripts();

// The one function AzerothCore calls to start this module, and the only symbol it looks for.
//
// The name is not a choice. The build system derives it from the directory name by replacing
// every hyphen with an underscore, so a module in mod-extended-bank must export exactly
// Addmod_extended_bankScripts(). Rename the directory and this has to change with it, or the
// build fails at the very last step with an unresolved symbol.
void Addmod_extended_bankScripts()
{
    AddExtendedBankConfigScripts();
    AddExtendedBankGossipScripts();
    AddExtendedBankCommandScripts();
    AddExtendedBankPlayerScripts();
}
