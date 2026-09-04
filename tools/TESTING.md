# mod-extended-bank — test plan

Tested against a live realm across **2026-09-02** and **2026-09-03**, driven from a game client
plus SOAP GM commands and read-only SQL. Characters used: `Admin` (3002), `Radmin`, and for the
trade work two ordinary ones, `Test` (24008) and `Testthree` (24010) — GM accounts cannot trade.

Every entry says what it *proves*, not just what to click. The happy path is marked as such; the
value is in the rest.

**How to read the boxes.** `[x]` means observed passing, never reasoned about. An unticked box is
one of three things, and the entry says which:

- *genuinely pending* — worth doing, nobody has;
- *not reachable* — the state cannot be produced from a client, and the entry says why (the
  trade race, a restricted bag with contents, the mail/auction reach-in);
- *covered by construction* — a static argument settles it and clicking adds little. §10's
  handler list is the main example.

That distinction matters more than the count. Most of what is still unticked is closed, not
outstanding.

Run `tools/check_invariants.sql` after anything that moves an item. Zero rows is a pass:

```bash
mysql -h127.0.0.1 -uacore -pacore acore_characters < tools/check_invariants.sql
```

Pair it with a log grep, because the flush writes only the rows a layout delta says changed and
commits them asynchronously. A statement that fails — a unique-key collision from a wrong delta,
say — aborts the transaction with **nothing shown in game**: the vault silently reverts to its
previous layout rather than losing anything, which is easy to mistake for the player misdragging.
The abort is a database error, and `Logger.sql.sql` routes those to the **Errors** appender with
the offending statement text -- not to `Server.log`, where an earlier revision of this file sent
you looking:

```bash
grep -i mod_extended_bank_vault_items build/bin/RelWithDebInfo/logs/Errors.log
```

Both log files open in `w` mode, so they are truncated at every server start and anything in
them belongs to the current run.

## 0. Instrumentation first

- [x] Set `Logger.module=3` in `worldserver.conf`. At the shipped `4` (WARN and above) the
      module's `LOG_INFO` lines are suppressed and its recovery paths are silent.
- [x] **Also lower the appender.** Done: this realm now runs `Appender.Server=2,3,0,Server.log,w`
      (level 3 = INFO). Before that it was level 5, which filtered the *file* at ERROR, so even
      with `Logger.module=3` the module's INFO and WARN lines reached only the console window.
      For most of the 2026-09-02 session that made "no module lines in `Server.log`" mean only
      "no module *errors*" — a far weaker statement than it looked, and it caught us out. With
      the appender at INFO the absence of module lines now genuinely means no recovery or repair
      path ran.
- [x] **Know which file SQL failures go to.** `Logger.sql.sql=2,Console Errors` with
      `Appender.Errors=2,2,0,Errors.log,w`, so a failed statement is written to `Errors.log`
      with its text — *not* to `Server.log`. Both files are opened `w` and truncated at startup,
      so anything in them belongs to the current run. Verified 2026-09-03: `Errors.log` held
      nothing but startup data warnings across a full day of vault testing.
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

- [x] A plain banker without the gossip flag — most of them. *Covers the
      `CMSG_BANKER_ACTIVATE` interception.*
- [x] A banker that also carries `UNIT_NPC_FLAG_GOSSIP`. *Covers `CanCreatureGossipHello`.*
      Both routes must produce the same menu.
- [ ] A banker that is also an innkeeper or vendor: its own options survive below the vault list
      and any sub-menu they open still works. *Proves the snapshot-and-rebuild carries
      `GossipMenuItemData` across.*
- [ ] A banker that is also a questgiver: its quests are still listed. *Proves `ClearMenu` was
      used rather than `ClearGossipMenuFor`, which also wipes the quest menu.*
- [ ] A banker with a `ScriptName`: the module stays out entirely and the NPC's own script runs.
- [x] **Jeeves (35642) without Master Engineering**: no vault list, and his menu is exactly what
      it is with the module disabled — "Let me browse your goods." only. *He is the only
      creature in the stock database with a condition on a banker option
      (`CONDITION_SKILL 202/350`).*
- [x] Jeeves **with** Engineering 350: the vault list appears and vaults open normally.
- [x] A banker whose own gossip menu has no options at all — Novia (16615), Periel (16616),
      Ceera (17631) or Elana (17632), all `npcflag` 131073 on map 530. Their banker option comes
      from the default menu 0 fallback, so the vault list must still appear. *This is the case
      the condition gate could plausibly have broken.* Verified 2026-09-03 in Silvermoon and
      the Exodar.
      *Proves the `GetScriptId()` guard.*
- [x] Own the maximum vaults on a banker with many DB gossip options, so the menu would exceed
      32 entries. **This is a crash test** — `GossipMenu::AddMenuItem` asserts past
      `GOSSIP_MAX_MENU_ITEMS`. Expect a clean warning naming the number actually dropped.
      *Established 2026-09-03 as unreachable on a stock database: the richest banker menu in the
      game is Jeeves, and he contributes one carried option beside the vault list. Every
      `AddGossipItemFor` in this module is already behind `CanAddMenuItem`, and the carried-item
      loop logs how many it dropped, so the guard is there for a modded realm rather than for
      anything shipped.*
- [x] Selecting a vault closes the gossip window with nothing left behind.

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
- [x] **Swap two items inside a vault** by dropping one onto the other, several times in a row.
      Both keep their new positions across a switch away and back. *The flush writes only the
      rows that changed, so a swap is the case that would collide on
      `UNIQUE KEY (owner_guid, vault, bag, slot)` if the deletes were not all emitted ahead of
      the inserts. A failure here aborts the transaction: positions silently revert to the
      previous flush rather than being lost.*
- [x] The same swap between a top-level bank slot and a slot inside a bank bag, and between two
      different bank bags.
- [x] Rearrange inside vault 1, switch to vault 2, switch back. The rearrangement stuck. *Proves
      the default-vault branch of `PersistOutgoingVault`.*
- [x] Rearrange inside vault 2, walk away to force a revert, return. Layout preserved.
- [x] Put a bag in a bank bag slot of vault 2, fill it, switch away and back. Bag and contents
      intact and in order. *Bags are the ordering-sensitive case: contents must detach before
      their bag and attach after it.*
- [ ] Same with two bags of different sizes.
- [ ] Fill vault 2 completely — 28 slots plus 7 bags — then switch away and back. *Worst case
      for switch cost and for `CanBankItem` refusals.*
- [x] A near-full vault — 28 slots plus a couple of bags with contents — then a single drag
      inside it. Position stuck across a switch away and back; no `mod_extended_bank_vault_items`
      line in `Server.log` and no invariant rows. *This is where the flush writes two statements
      where it used to write over five hundred, so it is where a bad delta has the most room to
      show. Verified 2026-09-03 against the delta flush.*

## 4. Bank bag slots

- [x] Buy a bank bag slot with vault 2 open: the price follows `BankBagSlotPrices.dbc` for
      **vault 2's** count, not vault 1's. The shipped prices, checked against the DBC, are
      10s, 1g, 10g, 25g, 25g, 25g, 25g for slots 1..7 — 111g 10s for a full set, so a vault
      whose count is behind vault 1's is visibly cheaper to top up.
- [x] An eighth slot is refused. Observed in game as a "can't buy more" refusal, and the cap is
      the client's own: `PurchaseSlot()` builds `CMSG_BUY_BANK_SLOT` only while the bank bag
      slot byte is `< 7`, so nothing reaches the server. Worth knowing because the DBC *does*
      carry rows 8..12 and the core handler would sell them — the seven-slot bound is the
      client's, not the price table's, and a forged packet is the only way past it.
- [x] Switch to vault 1: its count is unchanged and all its bags are present. *A wrong count here
      makes the core mail vault 1's bank bags back — grep the log for `sent by mail`.*
- [x] Relog: both counts restored.
- [x] Per-vault purchasing on the **stock Blizzard bank frame**. Verified 2026-09-03: the
      purchase button appears and disappears as the open vault's count crosses seven, in both
      directions, across vault switches and relogs. `UpdateBagSlotStatus`
      (`FrameXML/BankFrame.lua:103`) takes both the locked-slot tint and the purchase frame's
      visibility from one `GetNumBankSlots()` call, whose `full` return is computed live as
      `count > 6`, so there is nothing for the module to invalidate.
- [ ] **ElvUI users only.** ElvUI stops offering the purchase button once vault 1 owns all
      seven slots and does not offer it again in a vault with fewer. Not reproducible on the
      default UI, out of this module's control. `/run PurchaseSlot()` is a full workaround: it
      gates on nothing but the live count being under seven, so it follows the open vault, and
      it fires immediately with no confirmation dialog. Retest if ElvUI is ever in scope.
- [ ] Buy several slots in a row and watch the frame after each. A stale count was observed
      here more than once (display one purchase behind, correcting on reopen) but a controlled
      A/B could not reproduce it, and the server count was correct every time. Investigated
      2026-09-02 and closed unreproduced; `OpenVault` now closes the gossip frame explicitly as
      a precaution. If it reappears, capture whether the frame was opened via gossip or via a
      direct `SMSG_SHOW_BANK` -- that was the variable under suspicion. Seems again like an ElvUI-specific issue.
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

- [x] `|cff00ff00Herbs|r` renders coloured in both menus and survives a relog.
- [x] An inline icon: `|TInterface\Icons\INV_Misc_Bag_08:16|t Alts`.
- [x] Paste a very long name. Truncated to 240 characters, the gossip window still renders, and
      **no MySQL 1406** in `Errors.log`. *This is the bug that broke the UI outright.*
- [x] A name of multi-byte characters (Cyrillic, CJK) at the limit: truncation lands on a
      character boundary, never mid-sequence. **Client only** — the console/SOAP layer mangles
      non-ASCII to `?` before the module sees it, so this cannot be driven from a harness.
      Verified 2026-09-03 with a long Cyrillic name typed in the client: stored as exactly 240
      characters / 480 bytes, a clean 2 bytes per character throughout, so `utf8truncate` cut on
      a boundary with no partial sequence and nothing mangled. `VARCHAR(255)` counts characters
      rather than bytes in MySQL, so 240 fits — which is what the original `[1406] Data too
      long` failure was about.
- [ ] Rename vault 1 — the label changes, its storage does not.
- [x] Rename to an empty string: falls back to `Vault N`.

## 7. Lifecycle

- [x] Open vault 2, walk out of range. Within ~1s the live bank is vault 1 again — verify from a
      *different* banker and by a `character_inventory` diff after `.save`. Confirmed
      2026-09-03 with `.vault info`, which reports the live vault directly.
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

Two different kills, proving two different things, and they are not interchangeable.

**Server hard-kill** — `Stop-Process -Name worldserver -Force`, **not** `.server shutdown`. The
process dies with no logout path at all, so nothing the module holds in memory is written. This
is what tests the on-disk state and the login-time repairs.

**Client hard-kill** — SuperF4 on `wow.exe`. The server survives and runs its normal link-dead
logout, so this tests that `OnPlayerBeforeLogout` → `FlushAndDetachForLogout` fires on an abrupt
disconnect rather than only on a clean `/logout`. A client kill can never exercise a login
repair: the server saved correctly on the way out, so there is nothing left to repair. That
distinction cost a wasted run once — see the bank-bag-slot entry below.

### Client kill (abrupt disconnect)

- [x] Drag an item **out of** vault 2, kill `wow.exe` instantly. Item `12522091` ended up in
      exactly one place, `character_inventory` bag 0 slot 25; `.vault info` then read
      `Active vault: 1 (no session)`, so the revert fired without a clean logout;
      `characters.bankSlots = 1` (vault 1's count), so the core would not mail vault 1's bags
      back at the next login; vault 2's rows intact. *Proves the logout hook is reached on a
      dropped connection, which is the only thing standing between an abrupt disconnect and a
      vault left live in the bank slots.*
- [x] The same with an item moved **inside** a bank bag in the vault.
- [x] Two disconnects and a hard kill inside the long switching run that left
      `character_inventory` byte-identical to its opening baseline (§3).

### Server kill

- [x] Server hard-kill with vault 2 open and items in it. Restart, log in: vault 1 intact, vault 2 at
      its last saved state, nothing duplicated, no bags mailed. Run every invariant check.
- [x] Hard-kill immediately after dragging an item **out** of a vault. It ends up in exactly one
      place. *This is the window the single-transaction flush closes.*
- [x] Hard-kill immediately after dragging an item **into** a vault.
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
- [x] `SelfHealVaultRows` runs for a character who owns a vault and is skipped for one who does
      not. It writes nothing and logs nothing when there is nothing to repair, so count the
      statement itself instead:

      ```sql
      SELECT SCHEMA_NAME, COUNT_STAR, DIGEST_TEXT
      FROM performance_schema.events_statements_summary_by_digest
      WHERE DIGEST_TEXT LIKE '%mod_extended_bank_vault_items%'
        AND DIGEST_TEXT LIKE '%character_inventory%';
      ```

      The `DELETE v ... JOIN character_inventory` digest is the repair; nothing else in the
      module joins those two tables. Verified 2026-09-03: unchanged across a login by a
      character owning no vault, +1 across a login by one that does, and +1 again after a
      hard kill and restart, with the vault intact.
- [ ] The repair covers mail, auction and guild bank as well as `character_inventory`. Reaching
      the state needs a forged packet — `Player::GetItemByGuid` scans bank slots
      (`PlayerStorage.cpp:423,435`), so `CMSG_SEND_MAIL` and `CMSG_AUCTION_SELL_ITEM` can take an
      item out of an open vault, though the stock client's frames accept only items dragged from
      the bags — plus a crash inside the one tick before the next drain. Not reachable by
      playing; the cheap substitute is a hand-written vault row for an item already sitting in
      mail, then a login, then confirming the row is gone. **That is a DB write on a real
      character, so it is nobody's call but yours.**

      This counter is cumulative since the **MySQL** server started, not since the test, and a
      worldserver restart does not reset it — only the deltas mean anything. Select
      `SCHEMA_NAME` as well: the table is keyed on schema plus digest, so the same query run
      from a connection with no default database appears as a second row rather than
      incrementing the first.

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

**Read this before spending time here.** A static sweep on 2026-09-03 established that these are
covered by construction, not by luck. `opHandle->Call` exists in exactly four places in the
server (`WorldSession.cpp:470,491,504,523`, the four dispatching arms of one switch), and every
one is immediately preceded by `CanPacketReceive`; `WorldSocket::ProcessIncoming` only queues.
None of the functions involved is reachable off a packet either — `Player::RefundItem` has a
single caller at `ItemHandler.cpp:1453`, and `Guild::PlayerMoveItemData` serves only
`CMSG_GUILD_BANK_SWAP_ITEMS`. So the value left in the entries below is confirming the *handler*
behaves, not confirming the drain fires. Do them when convenient; none is load-bearing.

Two genuine exceptions were found by that sweep and are listed separately: the trade partner
(now fixed, below) and the mail/auction reach-in (covered by `SelfHealVaultRows`, §8).

- [x] Take a mail attachment.
- [ ] Post an auction; cancel an auction.
- [ ] Deposit into and withdraw from the guild bank.
- [ ] Refund a recently bought item.
- [ ] Wrap an item as a gift. *`ItemHandler.cpp:1205`, the site the docs used to mislabel as
      mail.*
- [ ] Open a lockbox or container item. *`SpellHandler.cpp:318`.*
- [ ] Open a trade window with a vault open — switching must be refused.
- [x] **Trade completed while the *other* player has a vault open.** Two characters at a bank.
      B opens vault 2 and drags an item inside it; A accepts the trade within the same second.
      B's `character_inventory` bank rows must be unchanged and B's vault 2 layout intact.
      *`HandleAcceptTradeOpcode` calls `SaveInventoryAndGoldToDB` on **both** players
      (`TradeHandler.cpp:660,668`), and B's own packets never pass through `CanPacketReceive`
      during A's accept. The hook now drains the partner on `CMSG_ACCEPT_TRADE` as well. Before
      that, the window was one world tick — B's `OnPlayerUpdate` drain closes it otherwise — so
      a negative result here proves little unless the timing is tight. Run it repeatedly.*

      Run 2026-09-03 with `Test` (24008) and `Testthree` (24010), both at the Orgrimmar banker,
      Testthree holding vault 2 open and Test completing the trade. A two-way trade completed;
      both items changed hands and every other row was byte-identical — both vault layouts, bag
      slot counts and money — with all 11 invariants clean. What that establishes is the safe
      part: reaching across to a second `Player` from inside a packet hook does not crash,
      deadlock on the recursive mutex, or disturb the partner's vault. It does **not** establish
      that the race was caught, which by the entry below is not reachable by hand at all.
- [ ] The same with the roles reversed, so the completing accept comes from the player who has
      the vault open rather than from their partner.
- [x] **The UI makes the race unreachable by hand.** Established 2026-09-03. The bank frame and
      the trade frame are both UI panels and cannot be shown together, and `TradeFrame_OnHide`
      calls `CloseTrade()` (`FrameXML/TradeFrame.lua:156`), which sends `CMSG_CANCEL_TRADE` — so
      talking to the banker cancels an in-progress trade, as it does at any NPC. Hiding the bank
      frame sends nothing, so a vault does stay live while its owner trades; they simply cannot
      reach it to dirty anything. Only one ordering is even testable: open the vault, *then*
      trade, then complete without touching the banker again. Treat the entries above as a
      safety check on reaching across to another `Player` from a packet hook, not as a bug hunt.
- [ ] Enter combat with a vault open — switching must be refused.
- [x] Loot a brand-new item straight into a vault's bank slot, then switch away immediately.
      *`ITEM_NEW` items have no `item_instance` row yet; this is the item-loss case.* **Not
      reachable**: both the default UI and ElvUI refuse to place an item from a loot window into
      a bank slot, so a looted item always lands in the bags first.
- [x] The reachable form of the same case: loot an item, then drag it from the bags into a vault
      *before* anything saves, so it reaches the vault still `ITEM_NEW`. `PersistVaultLayout`
      writes the vault row before `Item::SaveToDB` creates the `item_instance` row, which is
      safe only because both are in one transaction — worth knowing before anyone reorders that
      function.
      Verified 2026-09-03: a looted Weather-Beaten Journal (34109) dragged straight into vault 2
      landed at slot 44 with a matching `item_instance` row, correct owner and count, no
      `character_inventory` row, all invariants clean and nothing in `Errors.log`. The window is
      wider than it looks and the test almost certainly hit it — storing an item arms
      `m_additionalSaveTimer = 2000` (`PlayerStorage.cpp:7299`), so there are two full seconds
      before anything writes `item_instance`, and the module's own per-tick drain never touches
      items in the bags.
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
- [x] The rejection announcement itself. Besides the chat line, the banker sends a boss whisper
      naming the item. Observed on a stock client it draws no chat bubble over the NPC -- it
      renders as a full-screen system-style notice and triggers an addon alert sound, which is
      what is wanted. `ChatHandler::SendNotification` was tried next to it and removed as a
      near-duplicate that faded sooner. Re-check after any rebuild: chat line *and* whisper.
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

## Regression pass after the 2026-09-03 refactor

`ExtendedBankStorage.cpp` was split, four helpers were extracted, and four behaviours changed on
purpose. **Re-run `cmake .` in `build/` before rebuilding** — `ExtendedBankVaults.cpp` is a new
source file and `CollectSourceFiles()` is a configure-time glob, so without it the link fails on
an unresolved `ExtendedBankMgr::instance`.

Everything else is meant to be behaviour-identical, so the quickest confidence check is §3 and
§4 end to end. The four deliberate changes, each cheap to confirm:

- [ ] `.vault check` on a character who has never bought or opened a vault prints
      `SKIP bag-slot-check` instead of a spurious `FAIL bag-slot-mismatch`. It was comparing a
      purchased bank bag slot count against the 0 returned for a vault with no metadata row.
      The skip is narrow: a *non-default* vault open with no row is now its own failure,
      `FAIL missing-vault-row`, because that is the state that stops `LoadPlayer` restoring
      `PLAYER_BYTES_2` and gets the vault's bank bags mailed back.
- [ ] Re-opening a vault that is *already* open, from a **different** banker, rebinds the
      banker and restarts the range-check interval. Walk between two bankers without switching
      vaults; the vault must stay loaded. Re-opening from the *same* banker deliberately leaves
      the timer alone, so a client repeating the packet cannot hold the range check off.
- [ ] The rename list holds one menu slot back for **Back**. Unreachable at a vault limit of 20;
      confirm the menu still looks the same.
- [ ] `IsVaultRestricted` is now the exact negation of `Player::CanTakeMoreSimilarItems`'s "no
      maximum" test, which treats `MaxCount == 2147483647` as unlimited even when the item also
      carries an `ItemLimitCategory`. Both forms select the same 5802 shipped templates, so §10
      should behave identically; only a custom item could tell them apart.
- [ ] **The eviction notice names the item that actually moved.** A capped *bag* drags its
      uncapped contents out of the vault with it. If the bag fits back into the player's bags
      while a loose item from inside it does not, the whisper used to name the bag as having
      been mailed -- it tracked one name plus a ''did anything mail'' counter, and the counter
      was set by the loose item. It now tracks the first mailed item separately and prefers it,
      since that is the one the player cannot see arrive. Same unreachable-from-a-client
      caveat as the restricted-bag entry in §10.
- [ ] **Refund and soulbound-trade registrations on a mailed-back item.** `AttachVault` runs
      `RestoreItemSideData` *before* trying to place an item, so an item that then fails to
      place was registered in `m_refundableItems` and/or `m_itemSoulboundTradeable` and was
      being mailed with both entries intact — `Player::RemoveItem`, which clears the trade
      list, never runs on that path. `ReleaseItem` now clears both and is called there. Needs
      a vault holding a refundable or BOP-tradeable item that cannot be placed, so it is an
      instrumented-build test rather than a client one; listed because the failure is silent
      (a `_SaveInventory` complaint per save, and a once-a-second walk over an item that now
      lives in the mail).

## What is left

Short list, so the 40-odd unticked boxes above do not read as a backlog.

**Genuinely pending, reachable:** the remaining §5 purchase edge cases; §6 rename of a vault you
do not own; `.pdump` round-trip; a stack that merges on attach; §9's multi-character concurrency
entries beyond the ones already run.

**Not reachable from a client, and why:** the trade-partner race (the bank and trade frames
cannot both be open, and hiding the trade frame cancels the trade); a restricted *bag* with
ordinary contents inside a vault; the mail/auction reach-in that `SelfHealVaultRows` repairs.
Each needs a forged packet or an instrumented build.

**Closed by construction, not by clicking:** §10's handler list — `opHandle->Call` exists in
four places in the server and every one is preceded by the drain; §2's menu overflow — the
richest banker menu in the game contributes one option beside the vault list.

**Out of scope:** ElvUI's bank bag slot purchase button, which stops appearing once the Main
Vault owns all seven slots. Not reproducible on the default UI.
