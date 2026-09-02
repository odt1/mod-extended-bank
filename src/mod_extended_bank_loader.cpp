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

// The loader name is derived from the module directory by modules/CMakeLists.txt:
// every '-' becomes '_', so mod-extended-bank must export Addmod_extended_bankScripts().
void Addmod_extended_bankScripts()
{
    AddExtendedBankConfigScripts();
    AddExtendedBankGossipScripts();
    AddExtendedBankCommandScripts();
    AddExtendedBankPlayerScripts();
}
