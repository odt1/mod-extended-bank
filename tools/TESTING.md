# mod-extended-bank — test plan

**Verified 2026-09-02** against a live realm (`Admin`, guid 3002) driven over SOAP plus direct
read-only SQL. `[x]` means observed passing, not reasoned about. Unticked entries are genuinely
untested — several are marked with why.

Every entry says what it *proves*, not just what to click. The happy path is marked as such; the
value is in the rest.

Run `tools/check_invariants.sql` after anything that moves an item. Zero rows is a pass:

```bash
mysql -h127.0.0.1 -uacore -pacore acore_characters < tools/check_invariants.sql
```

## 0. Instrumentation first

- [x] Set `Logger.module=3` in `worldserver.conf`. At the shipped `4` (WARN and above) the
      module's `LOG_INFO` lines are suppressed and its recovery paths are silent.
- [ ] **Also lower the appender.** `Appender.Server=2,5,...` filters the *file* at level 5
      (ERROR), so even with `Logger.module=3` the module's INFO and WARN lines reach only the
      console window and never `Server.log`. Until that is changed, "no module lines in
      Server.log" proves only that no module *errors* occurred — a far weaker statement, and an
      easy one to over-read. This caught us out for most of the 2026-09-02 session.
- [x] Baseline the realm's pre-existing orphans (check 11 with its last line removed) so a later
      hit can be attributed to this module rather than to years of accumulated data.
- [x] Snapshot the test character's bank rows. Several tests below diff against it:
      `SELECT bag, slot, item FROM character_inventory WHERE guid = ? ORDER BY bag, slot`

## 1. No regression when disabled

- [x] `ExtendedBank.Enable = 0`, `.reload config`. Right-click a banker: the stock bank frame
      opens directly, no gossip window. *Proves the packet hook passes `CMSG_BANKER_ACTIVATE`
      straight through.*
- [ ] While disabled, log out and in twice. Bank rows byte-identical to the snapshot. *Proves
      the now-ungated login hook writes nothing.*
- [ ] Re-enable on a character that has never used a vault. Menu is exactly `Vault 1` and
      `Buy Next Vault`; picking `Vault 1` opens the bank with identical contents. (Happy path.)

## 2. Gossip surface

- [ ] A plain banker without the gossip flag — most of them. *Covers the
      `CMSG_BANKER_ACTIVATE` interception.*
- [ ] A banker that also carries `UNIT_NPC_FLAG_GOSSIP`. *Covers `CanCreatureGossipHello`.*
      Both routes must produce the same menu.
- [ ] A banker that is also an innkeeper or vendor: its own options survive below the vault list
      and any sub-menu they open still works. *Proves the snapshot-and-rebuild carries
      `GossipMenuItemData` across.*
- [ ] A banker that is also a questgiver: its quests are still listed. *Proves `ClearMenu` was
      used rather than `ClearGossipMenuFor`, which also wipes the quest menu.*
- [ ] A banker with a `ScriptName`: the module stays out entirely and the NPC's own script runs.
      *Proves the `GetScriptId()` guard.*
- [ ] Own the maximum vaults on a banker with many DB gossip options, so the menu would exceed
      32 entries. **This is a crash test** — `GossipMenu::AddMenuItem` asserts past
      `GOSSIP_MAX_MENU_ITEMS`. Expect a clean warning naming the number actually dropped.
- [ ] Selecting a vault closes the gossip window with nothing left behind.

## 3. The storage invariant

The section that matters most.

- [x] Distinctive items in vault 1, different ones in vault 2. Switch back and forth five times,
      running the invariant checks after each. *Check 1 failing means an item exists in both
      tables and can be duplicated.*
- [x] After any number of switches, the bank half of `character_inventory` equals the §0
      snapshot exactly. *This one diff is the entire design claim.*
- [x] Drag an item **out** of vault 2 into the bags and run the invariants **before** saving.
      Check 11 must not fire. *Regression test: the drain used to drop the vault row without
      writing the new `character_inventory` row, leaving the item in neither table until the
      next periodic save.* Then `.save` and confirm it has a `character_inventory` row and no
      vault row.
- [x] Drag an item **into** vault 2, `.save`, relog. It stays in the vault and does not snap back
      to the bag. *Proves `DeleteFromInventoryDB` runs for items taken in.*
- [ ] Rearrange inside vault 1, switch to vault 2, switch back. The rearrangement stuck. *Proves
      the default-vault branch of `PersistOutgoingVault`.*
- [x] Rearrange inside vault 2, walk away to force a revert, return. Layout preserved.
- [x] Put a bag in a bank bag slot of vault 2, fill it, switch away and back. Bag and contents
      intact and in order. *Bags are the ordering-sensitive case: contents must detach before
      their bag and attach after it.*
- [ ] Same with two bags of different sizes.
- [ ] Fill vault 2 completely — 28 slots plus 7 bags — then switch away and back. *Worst case
      for switch cost and for `CanBankItem` refusals.*

## 4. Bank bag slots

- [x] Buy a bank bag slot with vault 2 open: the price follows `BankBagSlotPrices.dbc` for
      **vault 2's** count, not vault 1's.
- [x] Switch to vault 1: its count is unchanged and all its bags are present. *A wrong count here
      makes the core mail vault 1's bank bags back — grep the log for `sent by mail`.*
- [x] Relog: both counts restored.
- [ ] Buy several slots in a row and watch the frame after each. A stale count was observed
      here more than once (display one purchase behind, correcting on reopen) but a controlled
      A/B could not reproduce it, and the server count was correct every time. Investigated
      2026-09-02 and closed unreproduced; `OpenVault` now closes the gossip frame explicitly as
      a precaution. If it reappears, capture whether the frame was opened via gossip or via a
      direct `SMSG_SHOW_BANK` -- that was the variable under suspicion.
- [ ] Buy up to 7 slots in vault 2, then switch to a vault with fewer. Nothing is mailed.

## 5. Buying

- [x] Buy vault 2: gold deducted matches `VaultCost[0]`. (Happy path.)
- [ ] Buy up to `MaxVaults`; the next attempt is refused and takes no money.
- [ ] Attempt with insufficient gold: refused, no money taken, no row created.
- [ ] Lower `MaxVaults` below what you own, `.reload config`: owned vaults still open, only
      buying is blocked.
- [ ] `ExtendedBank.VaultCost = "100,500000"` and reload: the log clamps it and the menu shows
      214748, not a small wrapped number. *Guards the `uint32` overflow.*
- [ ] Delete a `mod_extended_bank_vaults` row by hand while the character is offline, log in, and
      buy. Refused with "your vaults were out of date", **no gold taken**, list reloads. *Proves
      the desync guard that previously ate the player's money.*

## 6. Renaming

- [ ] `|cff00ff00Herbs|r` renders coloured in both menus and survives a relog.
- [ ] An inline icon: `|TInterface\Icons\INV_Misc_Bag_08:16|t Alts`.
- [x] Paste a very long name. Truncated to 240 characters, the gossip window still renders, and
      **no MySQL 1406** in `Errors.log`. *This is the bug that broke the UI outright.*
- [ ] A name of multi-byte characters (Cyrillic, CJK) at the limit: truncation lands on a
      character boundary, never mid-sequence. **Client only** — the console/SOAP layer mangles
      non-ASCII to `?` before the module sees it, so this cannot be driven from a harness.
- [ ] Rename vault 1 — the label changes, its storage does not.
- [x] Rename to an empty string: falls back to `Vault N`.

## 7. Lifecycle

- [ ] Open vault 2, walk out of range. Within ~1s the live bank is vault 1 again — verify from a
      *different* banker and by a `character_inventory` diff after `.save`.
- [x] Open vault 2, `.tele` away. Reverted on arrival.
- [ ] Open vault 2 and hearthstone out **mid-cast**, then again while the teleport is in flight.
      *`Player::SaveToDB` no-ops during a far teleport; the module deliberately does not depend
      on it, and this is what proves that.*
- [x] Log out from inside the bank frame with vault 2 open. Relog: vault 1 with its original
      contents, vault 2 still listed and intact.
- [x] Get disconnected rather than logging out cleanly (pull the network).
- [ ] Delete the character: both module tables lose its rows and no `item_instance` rows survive
      (checks 5 and 11).

## 8. Crash and recovery

Hard-kill means Task Manager → End Task, **not** `.server shutdown`.

- [x] Hard-kill with vault 2 open and items in it. Restart, log in: vault 1 intact, vault 2 at
      its last saved state, nothing duplicated, no bags mailed. Run every invariant check.
- [x] Hard-kill immediately after dragging an item **out** of a vault. It ends up in exactly one
      place. *This is the window the single-transaction flush closes.*
- [ ] Hard-kill immediately after dragging an item **into** a vault.
- [ ] Hard-kill during a switch, while the swap is in flight.
- [x] Hard-kill while a vault with a *different* bank bag slot count is open, with a bag
      actually placed in the default vault's bank bag slot. **The one crash case with teeth** —
      failure mails the player's bank bag back, contents included.
      Verified 2026-09-02: bag placed in vault 1 slot 67, vault 3 (`bag_slots=0`) opened, then
      saved so `characters.bankSlots=0` reached disk, then `worldserver.exe` killed. On login
      the live count read 1 while disk still read 0 — proving the repair ran, since nothing
      else sets that value at login — the bag loaded into slot 67, and nothing was mailed.
      Two traps: the wrong count must be forced to disk *before* the kill or the DB still holds
      the right value and the test proves nothing; and the `Restoring bank bag slot count` line
      goes to the console only, not `Server.log`, unless the appender level is lowered too.

## 9. Concurrency

`MapUpdate.Threads = 4` on this realm, so these are live risks, not theory.

- [ ] Two characters on **different maps**, both with a vault open, both walking away from their
      bankers at the same moment. *Reaches `_vaults`/`_sessions` from two `MapUpdater` threads.*
- [ ] The same two, logging out simultaneously.
- [ ] Several characters switching vaults repeatedly during a full `.save` sweep.
- [ ] A vault open while the autosave interval elapses.

## 10. Interaction with the rest of the core

Each of these reaches `_SaveInventory` **without** firing `OnPlayerSave` — which is exactly what
the drain exists for. With a vault open, do the action, then check invariant 1.

- [x] Take a mail attachment.
- [ ] Post an auction; cancel an auction.
- [ ] Deposit into and withdraw from the guild bank.
- [ ] Refund a recently bought item.
- [ ] Buy an item back from a vendor.
- [ ] Open a trade window with a vault open — switching must be refused.
- [ ] Enter combat with a vault open — switching must be refused.
- [ ] Loot a brand-new item straight into a vault's bank slot, then switch away immediately.
      *`ITEM_NEW` items have no `item_instance` row yet; this is the item-loss case.*
- [ ] A stack that merges on attach because the target slot already holds the same item.
- [x] Put a unique or quest item (any with `item_template.maxcount > 0` or a non-zero
      `ItemLimitCategory`) into a non-default vault. It must bounce back to the bags with a
      message within a tick, and never reach `mod_extended_bank_vault_items`.
- [x] The same, dropped into a bank bag *inside* a vault rather than a top-level slot. Removal
      has to resolve through the container; siblings must keep their rows and positions.
- [x] The bags-full mail path. Reached 2026-09-02 by **dropping a restricted item onto an
      occupied slot in a vault** rather than an empty one: the swap immediately refills the bag
      slot the item came from, so CanStoreItem has nowhere to return it and it is mailed. This
      is the only known route -- a drag to an *empty* slot always leaves the source slot free,
      so the item just bounces back. Verified: item in mail exactly once, no character_inventory
      row, no vault row, displaced items intact in the bags, all invariants pass.
- [ ] Same item into the **Main Vault**: must be allowed, since that is `character_inventory`
      and the core already counts it.
- [ ] A vault holding such an item from before the rule existed: opening it evicts the item.
- [ ] `.pdump write` a character with vaults and load it back: confirm the documented loss of
      vaults 2..N is what actually happens, and that nothing is corrupted by it.

## 11. Adversarial

- [x] A gossip select for a vault number you do not own. Refused. *`OpenVault` checks
      `OwnsVault`.*
- [x] A vault number above 255, so the `uint8` cast wraps to 0.
- [ ] A rename for a vault you do not own.
- [ ] `CMSG_BANKER_ACTIVATE` for a creature that is not a banker, and for one out of range.
- [x] Vault switches spammed as fast as the client will send them.
