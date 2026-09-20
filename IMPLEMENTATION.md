# mod-extended-bank implementation notes

This document is about *why*. If you want to know what the module does, read
[README.md](README.md). If you want to know whether it works, read
[tools/TESTING.md](tools/TESTING.md). This is for somebody who has opened the source and wants
to know why it is shaped the way it is, or who is about to change something and would like to
know what they are about to break.

| File | For |
|---|---|
| [README.md](README.md) | installing, configuring and using the module |
| [CLAUDE.md](CLAUDE.md) | the short working guide, and the mistakes already made once |
| [tools/TESTING.md](tools/TESTING.md) | the test plan, with `[x]` marking what has actually been observed passing |
| [tools/check_invariants.sql](tools/check_invariants.sql) | read-only database checks, zero rows means healthy |

Core line references are against the [mod-playerbots branch of
AzerothCore](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot) and were last
verified on 2026-09-03.

---

## The problem

A character in WotLK has one bank. The goal is to give them several, choosing between them at
any banker, without asking anybody to install an addon or patch their client.

That last constraint is the whole difficulty. The client has no concept of a second bank and
cannot be taught one. Its bank window reads two fixed arrays of slots on the player,
`PLAYER_FIELD_BANK_SLOT_1` (slots 39-66) and `PLAYER_FIELD_BANKBAG_SLOT_1` (slots 67-73), and
the packet that opens the window, `SMSG_SHOW_BANK`, carries nothing but the banker's GUID. There
is no field in it for which bank to show.

So "open vault 5" can only mean one thing. Take vault 1's items out of those slots in server
memory, put vault 5's items in, push the changed slots to the client, then send the very same
`SMSG_SHOW_BANK` the ordinary banker sends. The client draws what it finds and never learns that
anything unusual happened.

The trick is cheap. Everything below is about making it safe.

## The rule everything serves

> **Vault 1 is `character_inventory`. Vaults 2..N are `mod_extended_bank_vault_items`.
> Nothing ever moves between those two tables.**

Vault 1's rows sit in the core's own inventory table for the entire life of the character. The
module reads them and never inserts, updates or repositions them. Vaults 2 and up exist only in
the module's table, and never acquire an inventory row.

This matters so much because both tables key a row on (character, bag, slot):
`UNIQUE KEY (guid,bag,slot)` on the core's table, `UNIQUE KEY (owner_guid, vault, bag, slot)` on
the module's. If a vault item is ever written through the core's inventory save, it lands on the
row describing vault 1's slot 45, and vault 1's item in that slot is gone. Not misplaced. Gone.
That is the single failure this design exists to make impossible, and it is why the module never
composes an inventory row itself. It touches that table in exactly two ways, both of them the
core's own calls:

- `Item::DeleteFromInventoryDB`, which forgets where an item was, for an item being taken *into*
  a vault or posted back to its owner. This is the same call mail, the auction house and the
  guild bank each make when an item leaves a player's hands. It cannot reach vault 1's rows,
  because vault 1's items are not loaded while another vault is open.
- `Player::SaveInventoryAndGoldToDB`, called so that the **core** writes the rows for items the
  player has moved *out* of a vault, or rearranged inside vault 1, before the item objects are
  destroyed.

### Vault 1 is the resting state

A vault other than the first is loaded only while the player stands at a banker. The moment that
ends, vault 1 goes back into the slots. Walk away, change map, log out, or pick a different
vault, and the swap is undone.

This is what makes the design safe rather than merely careful. At every quiet instant, logged
out or walking around or in the middle of a crash, the character's bank slots hold vault 1 and
the database matches exactly what it would hold if this module had never been installed. A crash
leaves both tables individually valid with no item in both. The worst case is the data loss the
game already has: whatever changed since the last save.

## The variable that cannot be removed

The module keeps one `uint8` per player in memory saying which vault is open, plus the banker's
`ObjectGuid`. Not a database column, never persisted, never read at login. At login the answer
is always vault 1, by definition, because the core has just loaded `character_inventory` and
that *is* vault 1.

It looks removable and is not, because **the 3.3.5a client sends nothing when the bank window
closes**. The entire player-facing bank protocol is `CMSG_BANKER_ACTIVATE`, `SMSG_SHOW_BANK`,
`CMSG_BUY_BANK_SLOT` and `SMSG_BUY_BANK_SLOT_RESULT`. There is no close in either direction, and
the core has no notion of one either: `WorldSession::m_currentBankerGUID` is set in
`SendShowBank` (`BankHandler.cpp:188`) and never cleared.

"Swap only while the player is using the bank" therefore has a clear beginning and no end the
client will tell you about. The server has to work out for itself that the visit is over, and
that byte is what it works it out against.

### Why walking away is the right signal

`GetNPCIfCanInteractWith` tests `INTERACTION_DISTANCE` (5.5f, `ObjectDefines.h:24`), and that is
the same distance at which **the client closes the bank window by itself**. Losing the banker is
not a proxy for the window closing. For every close except pressing Escape while standing still,
it is the same event.

That one exception leaves a vault open while the player stands there. It costs nothing in
storage terms, since `character_inventory` is untouched and a crash is no worse than vanilla,
but core code that counts the bank (`GetItemCount(..., inBankAlso)`, uniqueness checks, quest
turn-ins) sees the open vault rather than vault 1 for as long as it lasts.

Every way of narrowing that gap is worse than the gap. There is no packet to close the bank
window either, so any speculative revert, an idle timeout being the obvious one, can fire while
the window is genuinely open. The player then watches vault 1's items appear inside a window
they believe is vault 5. Nothing is lost, because the next drag acts on real slots, but it is an
inexplicable event of exactly the shape that produces "the module ate my items" reports. An idle
timeout is also at its worst for a player who went away from the keyboard with the window open,
which is the commonest way to reach the case it was meant to fix.

## What happens when a vault is opened

In order:

1. Refuse if the player is in combat or has a trade window open.
2. Save the outgoing vault. For a vault the module owns that means `PersistVaultLayout` writing
   its rows, taking its items off the core's pending-write list, and then calling the core's
   `Player::SaveInventoryAndGoldToDB` **inside the same transaction**, so anything still
   pending, most importantly an item the player has just dragged *out* of the vault, gets its
   inventory row written atomically with the vault rows it is leaving. For vault 1 it is the
   core's save alone, because vault 1's positions *are* `character_inventory`.

   The core's full character save is deliberately avoided here. It returns without doing
   anything, and without running the module's hook, while a long-distance teleport is pending
   (`PlayerStorage.cpp:7236`). Trusting it would mean destroying items whose changes, or in the
   case of an `ITEM_NEW` looted seconds ago whose entire existence, had never been written. The
   narrower inventory save has no such escape hatch.
3. Empty the bank slots with `Player::RemoveItem`, contents of each bag before the bag itself,
   then destroy the `Item` objects. It is called with `update = false`, because with
   `update = true` it ends in `pItem->SendUpdateToPlayer` (`PlayerStorage.cpp:3073`), a full
   create-object block for an item destroyed on the next line. The client still learns the slots
   are empty, in one batch, at step 6.
4. `Player::SetBankBagSlotCount` to the incoming vault's count. This has to happen *before*
   anything is stored, or `Player::CanBankItem` refuses that vault's own bank bags for having no
   slots to go in.
5. Load the incoming vault: `Item::LoadFromDB`, then `CanBankItem`/`BankItem` for top-level
   slots and `CanStoreItem`/`StoreItem` for bag contents, each followed immediately by
   `RemoveFromUpdateQueueOf` and `SetState(ITEM_UNCHANGED)`, exactly as `Player::_LoadInventory`
   does. Afterwards the whole bank is swept and marked again, because `Player::_StoreItem` marks
   the *containing bag* `ITEM_CHANGED` too, so the first item into each bank bag dirties it
   again behind us. The core avoids this by holding `m_itemUpdateQueueBlocked` across its whole
   load, and that member is private.
6. `player->SendUpdateToPlayer(player)`. Without this the changed slots would ship at the end of
   the world tick, which is after the bank window has been drawn, and the player would see the
   previous vault's contents for an instant.
7. `WorldSession::SendShowBank(bankerGuid)`.

The gossip window is not closed explicitly, because the stock banker option does not close it
either (`Player::OnGossipSelect`, `case GOSSIP_OPTION_BANKER`). The client closes it by itself
when the bank opens. Every vault sends an identical packet, so vault 5 behaves exactly as vault
1 does.

## Keeping vault items away from the core's save

This is the part that took longest to get right, and the part most likely to be broken by a
well-meaning change.

While a vault other than the first is open, its items sit in the slots the core believes are the
character's real bank. Ordinary play re-marks them as needing a write, and the core's inventory
save would then write `REPLACE INTO character_inventory (guid, 0, 45, item)`, which through the
unique key on (character, bag, slot) deletes vault 1's row at slot 45.

The module cannot stop the core saving. What it does instead is take its items off the list the
core saves from, `Player::m_itemUpdateQueue`, before anything can reach that save. The core
writes nothing about items it cannot see there, because `Player::_SaveInventory` returns early
on an empty queue. That removal is called the **drain** throughout the code, and
`DrainUpdateQueue` is the function.

Two smaller mechanisms make the drain affordable:

- **Detaching is free.** `Player::RemoveItem` only touches memory and the client's view. It
  never changes `Item::uState`, so taking a whole vault out of the bank slots costs no database
  write at all. `Player::MoveItemFromInventory` is deliberately avoided, because it calls
  `SetNotRefundable()`, which would burn an item's refund window every time its owner switched
  vaults.
- **Attaching undoes its own dirt.** `Player::StoreItem` ends in `SetState(ITEM_CHANGED)`, which
  queues a `REPLACE INTO character_inventory`. The module cancels it immediately, the same way
  `Player::_LoadInventory` does, then sweeps the finished bank again for the bags `_StoreItem`
  re-dirtied behind it.

### Where the drain has to run, and why that is not obvious

Hooking the character-save event alone is not enough, and believing it was is the original bug
in this module's history. `Player::SaveToDB` is not the only route into `_SaveInventory`.
`Player::SaveInventoryAndGoldToDB` (`PlayerStorage.cpp:7304`) calls it directly and fires no
hook whatsoever. Every one of its callers:

| Site | What | Whose inventory |
|---|---|---|
| `AuctionHouseHandler.cpp:327,401,589,662` | sell, bid, buyout, cancel | the sender |
| `Guild.cpp:791,805` | guild bank deposit/withdraw | the sender |
| `MailHandler.cpp:374,621` | send mail, take attached item | the sender |
| `ItemHandler.cpp:1205` | gift wrapping | the sender |
| `SpellHandler.cpp:318` | open item | the sender |
| `Player.cpp:16146` | `Player::RefundItem` | the sender |
| `PlayerUpdates.cpp:2431` | partial-save timer | itself |
| `TradeHandler.cpp:656,667` | trade accept | the sender |
| **`TradeHandler.cpp:660,668`** | **trade accept** | **the trade partner** |
| `TC9GrpcHandler.cpp:140,199` | cluster sidecar | any player, by GUID |

Vendor buyback is absent from that list on purpose. `HandleBuybackItem` does
`RemoveItemFromBuyBackSlot` plus `StoreItem` and goes through the ordinary pending-write list,
so it never reaches this function.

So the drain runs at four points, which between them get in front of every entry above:

| Where | Covers |
|---|---|
| `ServerScript::CanPacketReceive`, before every packet handler runs | every handler that reaches `SaveInventoryAndGoldToDB`, including one that fires in the same batch as the packet that dirtied the item |
| the same hook, on `CMSG_ACCEPT_TRADE`, draining the **trade partner** as well | the one save that is not the packet sender's |
| `OnPlayerUpdate` (`PlayerUpdates.cpp:315`) | `UpdateAdditionalSaves` at `:340` in the same `Player::Update` |
| `OnPlayerSave` | full character saves |

The trade partner is the exception that shows why draining per packet is not sufficient on its
own. `HandleAcceptTradeOpcode` saves *both* sides, and the partner's own packets never pass
through this hook during somebody else's accept. Their per-tick drain still runs, so the
exposure was never more than one world tick, but most realms run several map threads and one
tick is all it takes. Reaching across to a second player from inside a packet hook is safe here
specifically because `CMSG_ACCEPT_TRADE` is `PROCESS_THREADUNSAFE`, which means it runs only in
`World::UpdateSessions` and never overlaps the map worker pool.

How reachable that case actually is turned out to be a separate question, answered on 2026-09-03
by trying it. The bank window and the trade window are both UI panels and cannot be shown at
once, and `TradeFrame_OnHide` calls `CloseTrade()` (`FrameXML/TradeFrame.lua:156`), which sends
`CMSG_CANCEL_TRADE`. So talking to a banker cancels an in-progress trade outright, which is
stock behaviour at every NPC and not something this module introduces.

Hiding the *bank* window, by contrast, sends nothing at all, because no close packet exists. A
vault therefore does stay open while its owner trades. They simply cannot see or reach it, so no
ordinary action can dirty a bank item during a trade, and the drain fires and finds nothing.
Reaching the original fault needs a scripted client, or one of the exotic routes that dirties a
bank item without the player touching it, such as an enchantment or a countdown ticking. The
guard stays because it costs one comparison. The hole itself is hard to fall into.

`TC9GrpcHandler` cannot be covered at all, since it saves an arbitrary player by GUID with no
packet to precede it. It sits behind `Cluster.Enabled`, which defaults to false, so it is inert
unless the realm runs in cluster mode.

Everything else in that table is reached only through a packet handler, and no packet handler
runs without the drain. That is checkable rather than hopeful: `opHandle->Call` appears in
exactly four places in the entire server, `WorldSession.cpp:470,491,504,523`, which are the
`STATUS_LOGGEDIN`, `STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT`, `STATUS_TRANSFER` and `STATUS_AUTHED`
arms of one switch, and each is immediately preceded by `CanPacketReceive`.
`WorldSocket::ProcessIncoming` only queues. Nor is any of those functions reachable off a
packet: `Player::RefundItem` has a single caller (`ItemHandler.cpp:1453`), and
`Guild::PlayerMoveItemData` exists only to serve `CMSG_GUILD_BANK_SWAP_ITEMS`.

### Handlers that can reach into an open vault

A different problem from saving, and worth stating because it is not obvious. Some parts of the
core find items by GUID alone without caring which container they are in, and the bank slots are
within range of that search. `Player::GetItemByGuid` scans bank slots and bank bags
(`PlayerStorage.cpp:423,435`), and both mail (`MailHandler.cpp:210,240`) and auctions
(`AuctionHouseHandler.cpp:205`) resolve their items with it. So `CMSG_SEND_MAIL` and
`CMSG_AUCTION_SELL_ITEM` can take an item straight out of an open vault. The stock client cannot
do it, because its mail and auction windows only accept items dragged from the bags, but a
crafted packet can, exactly as one can put a vault item into a trade.

The outcome is correct either way. The item leaves the bank, the next drain notices it missing,
drops its vault row and writes the core's inventory in one transaction. What is left is the tick
between the handler's commit and that drain, during which the item is in `mail_items` or
`auctionhouse` *and* still listed in a vault. A crash there leaves a stale vault row whose item
still exists, and opening that vault afterwards would load a second copy. That is why the login
repair checks all four tables and not just `character_inventory`.

### What the drain costs

Almost nothing, which it has to, because it runs before every packet and on every tick for as
long as a vault is open. It begins with a lock-free check that no vault is open at all, which is
the answer for almost every player on the realm. Beyond that it compares the bank against the
layout it last wrote and returns without touching the database when nothing has moved.

That comparison is a single pass producing a delta of which rows entered, moved and left, and
the save writes exactly those. Dragging one item inside a full vault costs one `DELETE` and one
multi-row `INSERT` rather than a rewrite of all 287 possible rows.

The two are ordered deliberately, and `PersistVaultLayout` is where it is enforced. Every row
about to move is deleted before any is written back, because two items swapping places would
otherwise collide on `UNIQUE KEY (owner_guid, vault, bag, slot)`: the first insert lands on a
slot its previous occupant has not vacated, and the whole transaction is rolled back. Since
commits happen in the background, that abort is invisible in game and the vault merely appears
to forget the player's last drag. A row nobody touched cannot cause it, because a slot only
frees up when whatever held it is itself in the delete list.

What the drain deliberately does *not* do is call `Player::SaveInventoryAndGoldToDB`. Taking the
vault's items off the pending-write list is the entire requirement. Pulling the core's inventory
save into the hot path as well would run its buyback purge, its position cheat-detection (which
can mark an item destroyed) and its queue clear at arbitrary packet boundaries, hundreds of
times a minute. It is called from only two places, both of them moments before the item objects
are freed: a vault switch, and logout.

Durability is therefore identical to vanilla. Both flush on the same save cadence.

### Thread safety

`Player::Update`, and so the per-tick hook, runs on the `MapUpdater` worker pool, so with
`MapUpdate.Threads > 1` two players in different zones reach the module's containers in the same
instant. Every entry point takes a `std::recursive_mutex`, recursive because the public entry
points legitimately call one another: opening a vault checks ownership and then switches, a save
drains first, and buying a vault reloads the list. The two hooks that fire for every player on
every tick check an `std::atomic` counter before taking the lock at all, so a realm with nobody
at a banker pays one atomic load.

## Bank bag slots

Each vault has its own count of purchased bank bag slots, and the stock purchase path is
untouched. `WorldSession::HandleBuyBankSlotOpcode` reads the live count, prices the next slot
from `BankBagSlotPrices.dbc`, and increments it. The shipped prices are 10s, 1g, 10g, 25g, 25g,
25g, 25g, which is 111g 10s for all seven.

Because the live count is set to the open vault's when that vault is opened, the in-bank
purchase button prices and scopes itself to that vault with no handler override at all. This is
the one piece of the feature that came for free.

The price table also carries rows 8 to 12, at 999,999,999 copper each. That is under
`MAX_MONEY_AMOUNT` (2,147,483,646), so the core's handler, which checks nothing but the row
existing and `HasEnoughMoney`, would happily sell them. It never gets the chance from a stock
client: `PurchaseSlot()` only builds `CMSG_BUY_BANK_SLOT` while the slot count is `< 7`, and
`GetNumBankSlots()` reports the bank full from the same byte, so the UI hides the button and an
eighth attempt is refused before any packet exists. Seven is a client constant rather than a
consequence of the price table, and the module inherits that bound.

The count itself lives in `PLAYER_BYTES_2`, which is saved into the `characters` row. That
creates the one piece of crash insurance in the module. If the server dies while a vault is
open, the saved count is that vault's, not vault 1's. At the next login the core would find
vault 1 owning more bags than it has slots and post the surplus to the player, contents
included. So the module keeps vault 1's count current in `mod_extended_bank_vaults.bag_slots`
and restores it from `OnPlayerLoadFromDB`, which runs before `Player::_LoadInventory`. That
ordering is the entire point of using that hook.

## Gossip menu

With nothing bought:

```
Main Vault
Buy Next Vault (100 gold)
Rename and Reorder Vaults
```

After buying one:

```
Main Vault
Vault 2
Buy Next Vault (1000 gold)
Rename and Reorder Vaults
```

Because vault 1 is listed as its own entry, the core-generated `GOSSIP_OPTION_BANKER` would be a
duplicate of it. So the module asks `Player::PrepareGossipMenu` to build the NPC's normal menu,
copies every option except that one along with its submenu link, clears the menu, adds the vault
entries, and puts the copied options back underneath. A banker who is also an innkeeper or a
vendor keeps those options and their submenus, and the quest list is left alone.

Entries the module adds carry a private marker, so anything else a player clicks falls through
to the core untouched.

Because the vault list stands in for the banker option, it is offered **only where that option
is**. If the menu the core built contains no `GOSSIP_OPTION_BANKER`, which is how the core says
this player may not bank here, the module changes nothing and hands the NPC back, and the result
is identical to the module not being installed. See [Design
consequences](#design-consequences-and-why-each-one-is-accepted) for why that is the whole of
the access-control story.

### Reaching a banker that has no menu

Most bankers carry `UNIT_NPC_FLAG_BANKER` without `UNIT_NPC_FLAG_GOSSIP`. For those the client
never sends `CMSG_GOSSIP_HELLO` on right-click at all. It sends `CMSG_BANKER_ACTIVATE`,
`WorldSession::HandleBankerActivateOpcode` answers with `SMSG_SHOW_BANK` directly, and no gossip
hook exists anywhere in that path. Left alone, those bankers would open the bank window with no
menu in front of it, which is most of the bankers in the game.

So the module also hooks packet reception. When the packet is `CMSG_BANKER_ACTIVATE` and the
module is enabled, it reads the banker GUID, repeats the stock
`GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BANKER)` guard, sends the vault menu, and returns
false so the core's handler never runs. Anything failing a check is passed through untouched,
and with `ExtendedBank.Enable = 0` the hook returns immediately on the opcode check.

No creature's `npcflags` are modified. Adding `UNIT_NPC_FLAG_GOSSIP` at runtime would work too,
but the `creature` table has an `npcflag` column that `Creature::SaveToDB` writes, so a GM
saving a banker would quietly make the added flag permanent in somebody's world database.

Bankers that *do* have the talk flag still arrive through the ordinary gossip hook. Both paths
call the same menu builder.

## Renaming

`Rename and Reorder Vaults` lists every owned vault, vault 1 included, as `Rename "<name>"`.
Vault 1's name is a label and nothing more: renaming it changes no storage. Clicking one opens
the client's text box. The same submenu carries the `Move "<name>" Up` entries described under
[Reordering](#reordering), which share a menu with renaming and are otherwise unrelated to it.

Content is not filtered, on purpose, so colour codes and inline icons work:

```
|cff00ff00Herbs|r
|TInterface\Icons\INV_Misc_Bag_08:16|t Alts
```

An unterminated `|c` bleeds colour into the next menu line and a malformed `|T` renders as
literal text. Both are cosmetic, self-inflicted, and visible only to the vault's owner. The
value is written with an escaped statement, so nothing a player types can affect the query.

**The client doubles every `|` it sends from a text box.** A player typing `|cffff0000Herbs|r`
arrives at the server as `||cffff0000Herbs||r`, and a doubled pipe renders as one literal pipe,
which is why colour codes and icons appeared as plain text until this was handled. `RenameVault`
collapses the pairs back. A literal pipe in a vault name is unreachable as a result, which is
the same trade the game makes everywhere else. Names stored before the fix still hold the
doubled form and need renaming once.

### Why there is a length limit at all

A name is truncated to **240 characters** (`EXTENDED_BANK_VAULT_NAME_MAX_CHARS`), and this
module is the only thing in the chain imposing a limit.

That is the surprising part. The client's rename box is
`StaticPopupDialogs["GOSSIP_ENTER_CODE"]` in `StaticPopup.lua`, which declares no `maxLetters`
at all, and `StaticPopup_Show` calls `editBox:SetMaxLetters` only when a dialog supplies one, so
the box keeps whatever limit the last dialog to set one left behind. On the way in,
`WorldSession::HandleGossipSelectOptionOpcode` reads the string with no cap, and on the way out
`PlayerMenu::SendGossipMenu` writes it straight into `SMSG_GOSSIP_MESSAGE` with no cap either.
Without this constant a pasted essay reaches the `VARCHAR(255)` column, fails, and leaves that
character's menu broken.

240 is chosen against what Blizzard shipped: across 3.3.5a's own `gossip_menu_option` data the
longest `OptionText` is 397 characters and the longest `BoxText` is 102, so a line of this order
is known to render. It leaves room for the rename prompt, which wraps the name in
`Enter a new name for {}:`, and sits under the column width.

### Why truncation happens before escaping

Because the order is a security property rather than a stylistic one, and `TruncateAndQuote`
exists to hold the two steps together so they cannot be separated by accident.

This `UPDATE` is the only statement in the module that puts player-typed text into SQL. Escape
first and truncate afterwards, and the cut can land inside an escape sequence and leave a
trailing backslash. That backslash escapes the statement's own closing quote, and everything
after it is handed to the database as SQL. Truncate first and it cannot happen, because the
truncation cuts on a character boundary and whatever survives is escaped whole.

A prepared statement would make the question moot and is not available to a module: they are
registered in the core's `CharacterDatabaseStatements` enum, which a module cannot extend
without editing core files. So the escape is `mysql_real_escape_string` through the live
connection handle, on a connection set to `utf8mb4`, which is what rules out the multi-byte
lead-byte bypass that defeats a connection-less escape. The format string stays a literal, so
the name is only ever an argument and braces inside it are inert.

Truncation uses the core's `utf8truncate`, the same helper `Guild::BankTab::SetText` uses. It
counts characters rather than bytes, so a name in any language is never left with a split
sequence, and it empties the string outright when the input is not valid UTF-8. An empty name
falls back to the default `Vault N` label.

The truncated string is what goes into both the database and the in-memory cache. Caching the
untruncated one is exactly what turned an over-long rename into a permanently broken menu: the
`UPDATE` failed with MySQL error 1406 (`Data too long for column 'name'`) while the menu went on
sending a name the database had rejected.

Confirmed against a long Cyrillic name typed into the client: stored as exactly 240 characters
and 480 bytes, a clean two bytes per character, so the cut landed on a boundary and nothing was
mangled. `VARCHAR(255)` counts characters rather than bytes in MySQL, so 480 bytes fits
comfortably and the character count is the only thing 240 has to respect.

## Reordering

Vaults can be moved up the menu. The whole feature is one column,
`mod_extended_bank_vaults.sort_order`, and it is worth saying why it stays that small, because
the obvious alternative is much worse.

**The vault number is never touched.** `vault` is the identity column. It is half the primary
key of the metadata table, part of the unique key on the item table, and the thing that decides
which of the two storage tables an item belongs to. Renumbering vaults to reorder them would
mean rewriting `mod_extended_bank_vault_items.vault` for every affected row, and a swap collides
on that unique key exactly the way two items swapping slots inside one vault do. The difference
is what a failure costs. A botched layout write loses an edit. A botched renumber files items
under the wrong vault. And the player would see nothing for the risk, because the numbers are
internal.

**The menu entry already carried the vault number**, as
`EXTENDED_BANK_ACTION_OPEN_BASE + vault`, so menu position and vault identity were independent
before any of this existed. Reordering changes the order of a loop and nothing else.

That makes the whole feature inert with respect to the rule at the top of this document. No item
row is read or written, so reordering needs none of the guards a vault switch does. It works
with a vault open, in combat, mid-trade, and no sequence of clicks can make it duplicate or
strand an item.

**Positions are renumbered from zero on every move** rather than swapping the two affected rows.
That costs one write per vault on an action a player performs by hand, at most twenty on a
click, and buys a property worth more than the saving: the result never depends on what the
previous values were. A realm that has never reordered has the same default in every row, and a
half-applied write from an earlier abort leaves gaps or duplicates. Both sort themselves out the
first time anything moves. The column deliberately carries no unique key, and `GetOwnedVaults`
sorts on `(sort_order, vault)`, so even a partly applied renumbering is still a total order
rather than an error. `PersistVaultOrder` is the function.

**A newly bought vault sorts last**, because `BuyNextVault` writes `EXTENDED_BANK_SORT_LAST`
(255) rather than a position. On a list nobody has reordered every row holds 255, they all tie,
and the tiebreak on the vault number reproduces exactly the order the menu had before the column
existed. Once anything is moved the list is renumbered 0..N-1, and 255 goes on meaning "after
everything placed so far" for whatever is bought next.

**Vault 1 is pinned to the front** rather than sorted there, so its own position can never
matter. It is the character's real bank and the state everything returns to, and a fixed anchor
is worth more than the freedom to bury it.

**Moving is offered only where it does something.** The menu lists `Move "<name>" Up` from the
third entry down, since the first is the pinned vault 1 and the second has only that above it,
so no line in the menu silently fails when clicked. `MoveVaultUp` refuses those two cases
anyway, which is what lets the debug command tell "already at the top" apart from "does not own
it".

Renaming and reordering share a submenu because they are the two things a player does to a vault
rather than to its contents, and because a move entry only reads properly next to the name it
moves. They are otherwise entirely untangled. A move never touches a name, a rename never
touches a position, and the default `Vault N` label follows the vault number rather than the
menu position, so moving one vault can never appear to rename another.

## Design consequences, and why each one is accepted

- **Stowed vaults are invisible to item lookups.** Items in a vault that is not currently open
  do not count toward `Player::GetItemCount(..., inBankAlso)`, so quest requirements,
  unique-item checks and similar scans ignore them. This is inherent to having more than one
  bank rather than a bug. One consequence: a unique item in a stowed vault will not stop its
  owner acquiring a second copy, and reopening that vault will then post the surplus to them.
- **Login-time cleanups do not apply to stowed vaults.** `Player::_LoadItem` deletes conjured
  items, items limited to another map or zone, and expired holiday items at login. The module
  deliberately does not repeat those checks when a vault opens, because that would destroy items
  merely for opening a vault in the wrong place. Refund windows and soulbound-trade windows
  *are* restored.
- **Faction and race changes skip vaults 2..N.** The core converts only items reachable through
  `character_inventory` (`CHAR_UPD_CHAR_INVENTORY_FACTION_CHANGE`).
- **`.pdump` does not know about the module tables.** A character dumped and reloaded that way
  loses vaults 2 and up.
- **Playerbots** read `BANK_SLOT_*` directly and always see vault 1, which is unaffected.
- **Switching is refused in combat and during a trade. The automatic reverts are not.** That
  asymmetry is deliberate. The guard is a courtesy, stopping a player *choosing* to swap at a
  silly moment, and it is not a safety property, so the reverts triggered by walking away,
  changing map or logging out do the identical detach with no check at all. Adding the guard to
  those would be strictly worse: a revert that refuses leaves a vault open after the player has
  left the banker, which breaks the resting-state rule everything else depends on.

  The trade half is not quite theatre. `HandleSetTradeItemOpcode` resolves its position with
  `_player->GetItemByPos(bag, slot)` (`TradeHandler.cpp:895`) and performs no check on what kind
  of position that is, and `Player::GetItemByPos` returns `m_items[slot]` for anything below
  `BANK_SLOT_BAG_END`, which is the whole bank, while `GetBagByPos` accepts bank bag slots too.
  So a crafted `CMSG_SET_TRADE_ITEM` can put a vault item into a trade. The consequence is mild,
  because `TradeData` stores an `ObjectGuid` rather than an `Item*` and resolves it through
  `Player::GetItemByGuid`. Once the module frees the item during a revert that resolves to
  `nullptr`, and every use of it in `HandleAcceptTradeOpcode` is null-checked. The item silently
  drops out of the trade. No dangling pointer, no duplication.
- **Two kinds of item are refused from vaults 2..N**, for the same underlying reason. A stowed
  vault is invisible to a rule the game enforces elsewhere, so parking an item there would buy
  its owner something the game does not sell. Either kind, found in a module-owned vault, is
  moved back to the player's bags with a message, or posted to them if the bags are full.
  `SelfHealVaultRows` is unrelated to this: the eviction check runs on every drain, so it also
  clears out items stored before the rule existed, the first time that vault is opened. Vault 1
  is unaffected in both cases, since it *is* `character_inventory` and the core's own accounting
  already sees it.

  **Items the game caps per character.** `MaxCount` and `ItemLimitCategory` are enforced by
  counting `character_inventory`, which a stowed vault is not part of, so parking a unique or
  quest item in one lets its owner acquire another. The check mirrors
  `Player::CanTakeMoreSimilarItems` (`PlayerStorage.cpp:818`), sentinel and all: there,
  `MaxCount == 2147483647` means "no limit" even for an item that also carries an
  `ItemLimitCategory`, so a test reading the two conditions independently would throw out items
  the game does not in fact cap. `IsVaultRestricted` is the predicate, and it covers 5802
  shipped templates.

  **Items with a countdown.** The clock is driven by `Player::UpdateItemDuration` walking
  `m_itemDuration`, and detaching a vault calls `Player::RemoveItem`, which calls
  `RemoveItemDurations`, so stowing a vault freezes the timer outright. The tempting objection
  is that the game already pauses these: at login the catch-up is
  `UpdateItemDuration(time_diff, true)` (`PlayerStorage.cpp:5584`), and `realtimeonly` skips
  anything without `ITEM_FLAGS_CU_DURATION_REAL_TIME`, so an ordinary countdown item in a normal
  bank already stops ticking while its owner is logged out. That misses what each pause costs.
  The game's is paid for in playing time, because stopping the clock means stopping playing. A
  vault stops the same clock for free while its owner carries on, and the item is still one
  banker visit away. Same effect, no price, which is exactly what this rule refuses. It covers
  all 280 templates with a countdown rather than only the 70 real-time-flagged ones a narrower
  rule would have caught, because the free-pause argument does not depend on the flag. The
  template is authoritative rather than the live `ITEM_FIELD_DURATION`, since `Item::LoadFromDB`
  forces the two into agreement anyway (`Item.cpp:452`).

  **Both are refusable by the realm.** `ExtendedBank.AllowRestrictedItems`, off by default,
  makes the whole check answer false. Neither rule is a statement about how the storage works.
  Both are opinions about what counts as an exploit, which belongs to whoever runs the realm.
  The config file carries the argument in full, and the module logs a warning naming the setting
  on every startup and every reload while it is on, because a realm running with it on by
  accident has no other symptom. One line covers the whole feature because nothing branches on
  *why* an item was refused. Switching it back off needs no migration either, since the sweep
  that clears out items stored before the rule existed is the same sweep that clears out items
  stored while it was off. Two things no switch can undo. One is a duplicate created in the
  meantime. The other has nothing to do with balance: `Player::CanBankItem` calls
  `CanTakeMoreSimilarItems` unconditionally (`PlayerStorage.cpp:2164`) and knows nothing of this
  setting, so a stowed unique whose owner has since acquired a second copy cannot be placed when
  its vault is next opened, and takes `AttachVault`'s mail-back path instead.
- **A newly bought bank bag slot may not appear until the bank is reopened.** Seen several times
  while buying slots with a vault open: the window kept showing the previous number, so after
  buying two slots it looked like one had been lost, and the final purchase left the last slot
  looking unavailable while the game still said no more could be bought. **Nothing is actually
  wrong.** The server's count is correct throughout, the gold is deducted once, and the slot is
  there. Closing and reopening the bank shows the right number and a relog is never needed. A
  controlled attempt to reproduce it afterwards failed, so the cause is unknown and it may not
  be specific to this module. The gossip window is now closed explicitly before the bank opens,
  which is the most likely remedy. Worth knowing about because it looks alarming and invites a
  bug report about lost gold, when nothing has been lost.

  Later evidence points at an addon rather than at this module. The stock Blizzard bank window
  was confirmed on 2026-09-03 to handle per-vault slot counts correctly in every direction: the
  purchase button appears and disappears as the open vault's count crosses seven, across vault
  switches and relogs alike. The client is well behaved here by construction, since
  `UpdateBagSlotStatus` (`FrameXML/BankFrame.lua:103`) derives both the locked-slot tint and the
  purchase frame's visibility from one `GetNumBankSlots()` call, whose second return is computed
  live as `count > 6` and cached nowhere. **ElvUI, by contrast, stops offering the purchase
  button once vault 1 has bought all seven slots, and does not offer it again in a vault that
  has fewer.** That is the only UI seen to get this wrong, it is out of this module's control,
  and `/run PurchaseSlot()` is a complete workaround, since that function gates on nothing but
  the live count being under seven and so always follows the open vault.
- **A refused item is announced twice, because a chat line alone is not enough.** When an item
  is refused it leaves the cursor and vanishes from the bank in the same instant, which looks
  exactly like losing it, and nobody is reading the chat frame in the middle of a drag. So the
  module writes the detail to chat *and* has the banker whisper it. The item is never actually
  lost. `CanStoreItem` puts it back in the bags, or it goes into the mail if there is nowhere to
  put it, which happens when the drop *swapped* with an occupied slot and refilled the source.

  The whisper is sent in the style bosses use (`Unit::Whisper(..., isBossWhisper = true)`). On a
  stock client that draws no chat bubble over the NPC. It renders as a notice across the middle
  of the screen, which is the desired effect anyway, and it is the form addons already listen
  for to play an alert sound. `ChatHandler::SendNotification` was tried alongside it and
  dropped, because it looks almost the same and stays on screen for less time. A vault opened
  through the GM `.bank` convention has no banker creature, so there the chat line is the only
  notice.
- **Conditions on an NPC's banker option are honoured, by inheriting the core's answer.** The
  vault list stands in for the banker option, so it is offered only where that option is.
  `Player::PrepareGossipMenu` omits an option whose `conditions` row fails
  (`PlayerGossip.cpp:60`) and applies no further check to a banker option, so one surviving into
  the built menu *is* the core stating that this player may use this bank. When none does, the
  module returns false and the NPC is handed back untouched. The core rebuilds the menu, which
  is safe because it starts by clearing, and sends it through `SendPreparedGossip`, whose
  quest-menu fallback and menu-aware greeting text the module does not try to reproduce.

  Jeeves (entry 35642) is the only creature in the stock database that gates a banker option,
  through `CONDITION_SKILL 202/350`, Master Engineering, and until this was added the vault list
  handed his bank to anyone who could reach him. The check costs nothing anywhere else: of 55
  bankers, 14 carry the talk flag, and every one of them produces a banker option, four of them
  through the default menu fallback that fires when a creature's own menu has no options at all.
- **A scripted banker is skipped entirely.** A banker whose `creature_template` carries a
  `ScriptName` is left alone, so it keeps its own gossip and gains no vault list.
  `ScriptMgr::OnGossipHello` asks every module first and stops at the first one that claims the
  NPC (`CreatureScript.cpp:34`), so taking one over would stop its own script from ever running.
  On top of that, a menu that script builds in code rather than in `gossip_menu_option` cannot
  be read back and re-added across the module's rebuild. Losing a custom NPC's options is worse
  than that NPC having no vaults.
- **An item leaving a vault is written atomically.** Dropping an item's vault row without
  writing the inventory row that says where it went would leave it in neither table until the
  next periodic save, an orphan window of up to `PlayerSave.Interval`, and a crash inside it
  strands the item. Its `item_instance` row survives, so a GM can recover it, but nothing else
  can. So the drain pulls the core's `SaveInventoryAndGoldToDB` into the same transaction
  whenever it notices something the vault used to hold is no longer in the bank. It does **not**
  do so otherwise, because `_SaveInventory` also purges buyback slots, runs position
  cheat-detection that can mark an item `ITEM_REMOVED`, and clears the whole pending-write list,
  none of which should happen at an arbitrary packet boundary. This gap was found by testing
  rather than by reading. See `tools/TESTING.md` §3.

## Compatibility

Any other module hooking `AllCreatureScript::CanCreatureGossipHello` on banker NPCs will
conflict, because the first script to claim the NPC wins and the other never runs. This module
only claims creatures that carry `UNIT_NPC_FLAG_BANKER` and have **no** `ScriptName`.

The same applies to `SERVERHOOK_CAN_PACKET_RECEIVE`. A module that swallows
`CMSG_BANKER_ACTIVATE` before this one does will suppress the vault menu on every banker without
a talk flag. This module swallows that one opcode only, and only for a creature the player can
legitimately bank with. For every other packet its hook drains the open vault and returns true.

More than one map thread is supported. See [Thread safety](#thread-safety).
