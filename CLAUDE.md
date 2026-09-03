# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

An AzerothCore module. The core repo two levels up carries `AGENTS.md` with project-wide rules
(formatting, SQL placement, "do not configure or build unless explicitly asked") — those apply
here and are not repeated. This file covers only what is specific to mod-extended-bank.

`README.md` is the design document and explains *why* each mechanism exists.
`tools/TESTING.md` is the test plan, with `[x]` marking what has actually been observed passing.

## What it does

Multiple bank "vaults" per character, chosen from any banker's gossip menu, on a stock 3.3.5a
client. The client has no concept of a vault: the bank frame reads two private update-field
arrays and `SMSG_SHOW_BANK` carries only the banker GUID. So "opening vault 5" can only mean
swapping vault 5's items into the player's real bank slots (39–73) and then sending the same
packet the stock banker sends.

## The invariant everything else serves

> **Vault 1 *is* `character_inventory`. Vaults 2..N are `mod_extended_bank_vault_items`.
> Nothing ever moves between those two tables.**

Vault 1 is also the resting state: a different vault is loaded only while the player is at the
banker, and reverted on switch, walk-away, map change or logout. So at every quiet instant —
including a crash — the bank slots hold vault 1 and match `character_inventory` exactly, as if
the module were not installed.

The module never composes a `character_inventory` row itself. It reaches that table only
through core helpers: `Item::DeleteFromInventoryDB` for an item taken *into* a vault, and
`Player::SaveInventoryAndGoldToDB` so the *core* writes rows for items that left one.

`SelfHealVaultRows` is the insurance: at login it drops any vault row whose item has turned up
somewhere the core owns — `character_inventory`, `mail_items`, `auctionhouse` or
`guild_bank_item`. The last three matter because `Player::GetItemByGuid` scans bank slots and
bank bags (`PlayerStorage.cpp:423,435`), so `CMSG_SEND_MAIL` and `CMSG_AUCTION_SELL_ITEM` can
take an item straight out of an open vault; the drain handles that correctly, and this covers a
crash inside the tick before it runs. Login is the only safe moment for the check — reads take a
synchronous connection while writes commit on the async pool, so running it mid-session could
see a row whose deletion is queued but not executed. It is skipped entirely for characters who
own no vault beyond the default, which is most of a realm.

## The part that is easy to get wrong

**Keeping vault items out of `Player::m_itemUpdateQueue` is the whole design.**
`_SaveInventory` returns early on an empty queue and writes nothing about items not in it. If a
live vault item is queued when it runs, it writes `REPLACE INTO character_inventory (guid, 0,
45, item)`, which through `UNIQUE KEY (guid,bag,slot)` **deletes vault 1's row at slot 45**.

`Player::SaveToDB` is *not* the only route to `_SaveInventory`.
`Player::SaveInventoryAndGoldToDB` (`PlayerStorage.cpp:7304`) calls it directly and fires **no
hook at all** — mail, auction house, guild bank, item refund, gift wrapping, open-item, trade
and the partial-save timer all reach it. (Vendor buyback does *not*: `HandleBuybackItem` goes
through the ordinary update queue. An earlier revision of this file claimed otherwise.) Hooking
`OnPlayerSave` alone was this module's original bug. The drain runs at four points:

| Where | Covers |
|---|---|
| `ServerScript::CanPacketReceive`, before every packet handler | every handler reaching `SaveInventoryAndGoldToDB`, including one in the same batch as the packet that dirtied the item |
| the same hook, on `CMSG_ACCEPT_TRADE`, draining the **partner** too | `HandleAcceptTradeOpcode` saves both sides (`TradeHandler.cpp:660,668`); the partner's own packets never pass through this hook during someone else's accept |
| `OnPlayerUpdate` (`PlayerUpdates.cpp:315`) | `UpdateAdditionalSaves` at `:340` in the same `Player::Update` |
| `OnPlayerSave` | full character saves |

That covers everything, and the reason is checkable rather than hopeful: `opHandle->Call`
appears in exactly four places in the whole server (`WorldSession.cpp:470,491,504,523`, the four
dispatching arms of one switch), each immediately preceded by `CanPacketReceive`, and
`WorldSocket::ProcessIncoming` only queues. The single exception is `TC9GrpcHandler`, which
saves an arbitrary player over gRPC with no packet to precede; it is gated behind
`Cluster.Enabled`, default false.

Other landmines, each already paid for once:

- **`Player::_StoreItem` marks the containing bag `ITEM_CHANGED` too**, so every bank bag is
  pushed back into the queue by the first item stored into it. The core avoids this with
  `m_itemUpdateQueueBlocked`, which is private — so `AttachVault` sweeps the finished bank.
- **`Player::SaveToDB` returns without doing anything, and without firing `OnPlayerSave`, while
  a far teleport is pending** (`PlayerStorage.cpp:7236`). Never depend on it to persist before
  freeing `Item` objects; use `SaveInventoryAndGoldToDB`, which has no such guard.
- **`Item::SaveToDB` deletes the object itself on `ITEM_REMOVED`** (`Item.cpp:407`). No
  `delete` of your own after it.
- **The core's inventory save is expensive and has side effects** — buyback purge, position
  cheat-detection that can mark an item `ITEM_REMOVED`, full queue clear. It belongs only where
  `Item` objects are about to be freed, or where an item has just left a vault.
- **`PersistVaultLayout` writes a delta, and its deletes must all precede its inserts.** Two
  items swapping places inside a vault collide on `UNIQUE KEY (owner_guid, vault, bag, slot)`
  otherwise — the first `INSERT` lands on a slot its previous occupant has not vacated — and the
  whole transaction aborts. Because commits are asynchronous, that abort is invisible in game:
  the vault silently reverts to its last flush. `Errors.log` is the only place it shows.
- **The vault row and the `item_instance` row must stay in one transaction.** An item dragged
  into a vault within two seconds of being looted is still `ITEM_NEW` and has no `item_instance`
  row yet (`m_additionalSaveTimer = 2000`, `PlayerStorage.cpp:7299`). `PersistVaultLayout`
  writes the vault row *before* `Item::SaveToDB` creates that row, which is safe only because
  both are in the same transaction. Splitting them would strand items looted seconds earlier.
- **`Player::Update` runs on the `MapUpdater` pool.** With `MapUpdate.Threads > 1`, two players
  on different maps reach the manager's containers in the same tick. Every entry point takes a
  `std::recursive_mutex`; the two hot hooks check an `std::atomic` count first.

## Layout

| File | Responsibility |
|---|---|
| `src/ExtendedBank.h` | constants, config cache, `ExtendedBankMgr` |
| `src/ExtendedBankStorage.cpp` | attach/detach/flush, every DB access — the whole invariant lives here |
| `src/ExtendedBankGossip.cpp` | `AllCreatureScript` menu + the `ServerScript` packet hook |
| `src/ExtendedBankPlayer.cpp` | `PlayerScript` lifecycle hooks |
| `src/ExtendedBankConfig.cpp` | `ConfigValueCache` + `WorldScript` |
| `src/ExtendedBankCommands.cpp` | `.vault` debug commands, all `Console::Yes` |
| `tools/check_invariants.sql` | 11 read-only DB checks; zero rows = healthy |

Two entry points reach the gossip menu, and both must keep working: bankers *with*
`UNIT_NPC_FLAG_GOSSIP` arrive via `CanCreatureGossipHello`; the majority, which lack it, send
`CMSG_BANKER_ACTIVATE` and are intercepted in `CanPacketReceive`. Creatures with a `ScriptName`
are deliberately left alone — `ScriptMgr::OnGossipHello` stops at the first `AllCreatureScript`
returning true, so claiming one would suppress its own script.

The vault list stands in for the stock `GOSSIP_OPTION_BANKER` entry, so it is offered **only
where that entry is**. `PrepareGossipMenu` omits an option whose `conditions` row fails
(`PlayerGossip.cpp:58`) and applies no further check to a banker option, so one surviving into
the built menu is the core's own statement that this player may bank here — no condition
evaluation is duplicated. On refusal `SendMainMenu` returns false having changed nothing, and
the core re-runs `PrepareGossipMenu` (idempotent; it opens with `ClearMenus`) and sends through
`SendPreparedGossip`, whose quest-menu fallback and menu-aware text id this module does not
reproduce. Jeeves (35642) is the only creature in the stock database that gates a banker option.
Four bankers get theirs from the default menu 0 fallback rather than their own menu, which fires
when a creature's menu has no options at all — do not break that path.

## Commands

```bash
# Lint (run both; the core one reports the whole tree, so filter)
# ci-codestyle.sh greps a RELATIVE "src" -- run it from the module root or it silently
# scans the core's sources instead and reports their tabs as yours.
bash apps/ci/ci-codestyle.sh
python ../../apps/codestyle/codestyle-cpp.py 2>&1 | grep -i mod-extended-bank

# Adding a NEW .cpp requires a CMake reconfigure before it will build.
# modules/CMakeLists.txt collects sources with CollectSourceFiles(), a configure-time glob;
# without this the build fails at link time with an unresolved symbol.
cd ../../build && cmake .
```

The loader symbol is derived from the directory name by `modules/CMakeLists.txt` — every `-`
becomes `_`, so this module must export `Addmod_extended_bankScripts()`.

## Testing

There are no unit tests; verification is against a running realm. `tools/TESTING.md` has the
full plan and current state.

```bash
# Read-only invariant check — zero rows means healthy
Requirements/mysqlbin/mysql -h127.0.0.1 -uacore -pacore acore_characters < tools/check_invariants.sql
```

With SOAP enabled (`SOAP.Enabled = 1`, 127.0.0.1:7878), GM commands can be driven from a
script: POST a `<ns1:executeCommand><command>…</command></ns1:executeCommand>` envelope with
HTTP basic auth. That plus the `.vault` subcommands — `info`, `check`, `open`, `revert`,
`flush`, `buy`, `rename` — makes most of the plan runnable without a game client. `.vault open`
uses the player's own GUID as the banker, the GM `.bank` convention that
`WorldSession::CanUseBank` special-cases, so a vault opened that way stays loaded.

Item *movement* still needs a real client; SOAP cannot drag things between slots.

**Logging, and where to actually look.** `Logger.module` defaults to `4` (WARN+), which silences
the module's `LOG_INFO` recovery lines, and the shipped `Appender.Server` filters the *file* at
ERROR — with both at their defaults, "no module lines in `Server.log`" proves only that no
module *errors* occurred. Set `Logger.module=3` **and** `Appender.Server=2,3,...` before reading
anything into an empty log.

A failed statement never appears there at all: `Logger.sql.sql` routes to the **Errors**
appender, so `Errors.log` is where an aborted flush shows up, with its SQL. Both files open in
`w` mode and truncate at startup, so their contents always belong to the current run. This
matters more than it sounds — commits are asynchronous, so a failed flush is completely silent
in game and the vault merely appears to revert an edit.

Non-ASCII text is mangled to `?` by the console/SOAP layer before the module sees it, so
multi-byte name handling can only be tested from a real client.
