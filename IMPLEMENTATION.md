# mod-extended-bank — implementation notes

This is the design document: *why* each mechanism exists, not just what it does. It assumes you
have read [README.md](README.md) and are here because you are changing the code, reviewing it,
or trying to work out whether some behaviour is deliberate.

The other documents:

| File | For |
|---|---|
| [README.md](README.md) | installing, configuring and using the module |
| [CLAUDE.md](CLAUDE.md) | the short working guide — the invariant, and the mistakes that are easy to make |
| [tools/TESTING.md](tools/TESTING.md) | the test plan, with `[x]` marking what has actually been observed passing |
| [tools/check_invariants.sql](tools/check_invariants.sql) | read-only database checks; zero rows means healthy |

Core line references are against the [mod-playerbots branch of
AzerothCore](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot) and were last
verified on 2026-09-03.

---

## How it works

The WotLK client has no concept of a bank page. Its bank frame reads two private update-field
arrays on the player, `PLAYER_FIELD_BANK_SLOT_1` (slots 39-66) and `PLAYER_FIELD_BANKBAG_SLOT_1`
(slots 67-73), and `SMSG_SHOW_BANK` carries nothing but the banker's GUID. Opening vault 5 can
therefore only mean one thing: put vault 5's items into the player's real bank slots, push the
update fields, then send the very same `SMSG_SHOW_BANK` the vanilla banker option sends.

### The storage invariant

> **Vault 1 is `character_inventory`. Vaults 2..N are `mod_extended_bank_vault_items`.
> Nothing ever moves between those two tables.**

- Vault 1's bank rows live in `character_inventory` for the entire life of the character. The
  module reads them; it never inserts, updates or repositions them.
- Vaults 2..N exist only as rows in `mod_extended_bank_vault_items` plus their ordinary
  `item_instance` rows. They never acquire a `character_inventory` row.
- **Vault 1 is the resting state.** A different vault is loaded only while the player is
  actually standing at the banker. As soon as that ends the module puts vault 1 back. Logged
  out, walking around, or crashed, the character's bank slots hold vault 1 and match
  `character_inventory` exactly, as if the module were not installed.

A crash at any moment leaves both tables individually valid and mutually disjoint. The worst
case is the data loss vanilla already has: changes since the last `Player::SaveToDB`.

The module never composes a `character_inventory` row of its own. It touches that table in
exactly two ways, both of them the core's:

- `Item::DeleteFromInventoryDB` — delete-by-item-GUID — for an item it takes *into* a vault,
  and for an item it has to mail back because the slot it recorded is no longer usable. That
  is the same call mail, the auction house and the guild bank make whenever an item leaves a
  player's inventory. In the first case it cannot reach vault 1's rows, because vault 1's
  items are not live while another vault is open; in the second, deleting the row is the whole
  point, since the item is going to the mailbox.
- `Player::SaveInventoryAndGoldToDB`, called so the **core** writes the rows for items the
  player moved out of a vault, or rearranged inside vault 1, before their `Item` objects are
  freed. Every row's contents are decided by `Player::_SaveInventory`, exactly as on a normal
  save.

### Why an "active vault" variable exists

It is one `uint8` in session memory plus the banker's GUID. It is not a database column, is
never persisted, and is never read at login — on login the bank is vault 1 by definition,
because `Player::_LoadInventory` just read `character_inventory`.

It cannot be dropped, because **3.3.5a sends no packet when the bank frame closes**. The entire
player-bank opcode surface is `CMSG_BANKER_ACTIVATE`, `SMSG_SHOW_BANK`, `CMSG_BUY_BANK_SLOT` and
`SMSG_BUY_BANK_SLOT_RESULT` — there is no close in either direction, and the core has no notion
of one either: `WorldSession::m_currentBankerGUID` is set in `SendShowBank`
(`BankHandler.cpp:188`) and never cleared. "Swap only while the player is using the bank" has a
clear start but no client-supplied end, so the server needs its own way to notice the
interaction is over. Reverting to vault 1 happens on the first of: picking another vault,
walking out of the banker's interaction range (checked once a second), changing map, or logging
out.

#### Why the range check is the right substitute, and not merely the available one

`GetNPCIfCanInteractWith` tests `INTERACTION_DISTANCE` (5.5f, `ObjectDefines.h:24`), which is
the distance at which **the client closes the bank frame by itself**. So losing the banker is
not a proxy for "the frame closed" — it is the same event, for every close except pressing
Escape while standing still.

That one remaining case leaves a vault live while the player stands there. It costs nothing in
storage terms — `character_inventory` is untouched and a crash is no worse than vanilla — but
core code counting the bank (`GetItemCount(..., inBankAlso)`, uniqueness checks, quest
turn-ins) sees the open vault rather than vault 1 for as long as it lasts.

Narrowing it is tempting and every available lever is worse than the gap. There is no `SMSG` to
close the bank frame either, so any speculative revert — an idle timeout being the obvious
one — can fire while the frame is genuinely open, and the player then watches vault 1's items
appear inside a frame they believe is vault 5. Nothing is lost, since the next drag acts on
real slots, but it is an unexplainable event of exactly the shape that produces "the module ate
my items" reports. An idle timeout is in fact at its worst for a player who went AFK with the
frame open, which is the commonest way to reach the case it is meant to fix.

### Exactly where the swap happens

Selecting a vault runs, in this order:

1. Refuse the switch if the player is in combat or has a trade window open.
2. Persist the outgoing vault. For a module-owned vault that is `DrainUpdateQueue`: it writes
   the vault's rows, un-queues its items, and then calls the core's
   `Player::SaveInventoryAndGoldToDB` **in the same transaction**, so anything still queued —
   most importantly an item the player dragged *out* of the vault — gets its
   `character_inventory` row written atomically with the vault rows it is leaving. For the
   default vault it is `Player::SaveInventoryAndGoldToDB` alone, because the default vault's
   positions *are* `character_inventory` and only the core writes them.

   `Player::SaveToDB` is deliberately **not** used. It returns immediately — before
   `OnPlayerSave`, so before this module gets to persist anything — while a far teleport is
   pending (`PlayerStorage.cpp:7236`). Detaching after that would free `Item` objects whose
   changes, or in the `ITEM_NEW` case whose entire existence, had never been written.
   `SaveInventoryAndGoldToDB` carries no such guard and is exactly the half that matters.
3. Detach the live bank with `Player::RemoveItem`, contents of a bank bag before the bag
   itself, then free the `Item` objects. `RemoveItem` is called with `update = false`, because
   with `update = true` it ends in `pItem->SendUpdateToPlayer` (`PlayerStorage.cpp:3073`) — a
   create-object block for an item destroyed on the very next line. The slot GUIDs it clears
   are still delivered, as one batch, by step 6.
4. `Player::SetBankBagSlotCount` to the incoming vault's count — this has to happen *before*
   anything is stored, or `Player::CanBankItem` refuses that vault's bank bags.
5. Attach the incoming vault: `Item::LoadFromDB`, then `CanBankItem`/`BankItem` for top-level
   slots and `CanStoreItem`/`StoreItem` for bag contents, each followed by
   `RemoveFromUpdateQueueOf` + `SetState(ITEM_UNCHANGED)`, and finally one sweep over the
   finished bank repeating that for every item — `Player::_StoreItem` marks the *containing
   bag* `ITEM_CHANGED` as well, so each bank bag is pushed back into the queue by the first
   item stored into it. (The core avoids this by holding `m_itemUpdateQueueBlocked` across the
   whole of `_LoadInventory`; that member is private, so the module sweeps instead.)
6. `player->SendUpdateToPlayer(player)` — the bank slot GUID fields were only marked dirty and
   would otherwise ship at the end of the world tick, after the client had already drawn the
   frame with the previous vault's contents.
7. `WorldSession::SendShowBank(bankerGuid)`.

The gossip window is **not** closed explicitly. The stock banker option does not close it
either (`Player::OnGossipSelect`, `case GOSSIP_OPTION_BANKER`): the client closes the gossip
frame itself when `SMSG_SHOW_BANK` arrives. Every vault sends the identical packet, so the
transition looks the same for vault 1 and vault 5.

### Three mechanisms that keep the invariant true

1. **Detaching is free.** `Player::RemoveItem` only touches memory and the client's update
   fields; it never changes `Item::uState`, so removing an unchanged item writes nothing.
   `Player::MoveItemFromInventory` is deliberately *not* used — it calls `SetNotRefundable()`,
   which would burn an item's refund window on every vault switch.
2. **Attaching un-dirties itself.** `Player::StoreItem` ends with `SetState(ITEM_CHANGED)`,
   which queues a `REPLACE INTO character_inventory`. The module cancels it exactly the way
   `Player::_LoadInventory` does, then sweeps the whole bank once more to catch the bags
   `_StoreItem` re-dirtied behind it.
3. **The drain.** While a non-default vault is open, ordinary player actions re-dirty its
   items, and `Player::_SaveInventory` would write
   `REPLACE INTO character_inventory (guid, 0, 45, item)` — which, through the
   `UNIQUE KEY (guid,bag,slot)`, would delete vault 1's row at slot 45. Keeping those items
   out of `Player::m_itemUpdateQueue` is what prevents it: `_SaveInventory` returns early when
   the queue is empty and writes nothing about items that are not in it.

#### Why the drain runs where it does

`OnPlayerSave` alone is **not** enough, and relying on it was this module's original mistake.
`Player::SaveToDB` is not the only route to `_SaveInventory`:
`Player::SaveInventoryAndGoldToDB` (`PlayerStorage.cpp:7304`) calls it directly and fires no
hook at all. Every one of its call sites:

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

Vendor buyback is deliberately absent: `HandleBuybackItem` does `RemoveItemFromBuyBackSlot` +
`StoreItem` and goes through the ordinary update queue, so it never reaches this function.

So the module drains at four points, which between them precede every one of those:

| Where | Covers |
|---|---|
| `ServerScript::CanPacketReceive`, before every packet handler runs | every handler that reaches `SaveInventoryAndGoldToDB`, including one that fires in the same batch as the packet that dirtied the item |
| the same hook, on `CMSG_ACCEPT_TRADE`, draining the **trade partner** as well | the one save that is not the packet sender's |
| `OnPlayerUpdate` (`PlayerUpdates.cpp:315`) | `UpdateAdditionalSaves` at `:340` in the same `Player::Update` |
| `OnPlayerSave` | full character saves |

The trade partner is the exception that proves why per-packet draining is not enough on its
own. `HandleAcceptTradeOpcode` saves *both* sides, and the partner's own packets never pass
through this hook during the accept. Their `OnPlayerUpdate` drain still runs every world tick,
so the exposure was never more than one tick — but `MapUpdate.Threads` is greater than one on
most realms, and one tick is all it takes. Reaching across to another player from a packet hook
is safe here specifically because `CMSG_ACCEPT_TRADE` is `PROCESS_THREADUNSAFE`: it runs only in
`World::UpdateSessions`, which does not overlap the `MapUpdater` pool.

How reachable that partner case is turned out to be a separate question, answered on
2026-09-03 by trying it. The bank frame and the trade frame are both UI panels and cannot be
open at once, and `TradeFrame_OnHide` calls `CloseTrade()`
(`FrameXML/TradeFrame.lua:156`), which sends `CMSG_CANCEL_TRADE` — so anything that hides the
trade frame cancels the trade outright. Talking to the banker does exactly that, which is stock
behaviour for every NPC and not something this module introduces.

Hiding the *bank* frame, by contrast, sends nothing at all, because no close opcode exists. So
a vault stays live while its owner trades — but its owner cannot see or touch it, and a bank
item can therefore not be dirtied by any ordinary action during a trade. The drain fires and
finds nothing queued. Reaching the original fault needs a scripted client, or one of the
exotic routes that dirties a bank item without the player touching it, such as an enchantment
or item duration ticking down. The guard stays because it costs one comparison, not because
the hole is easy to fall into.

`TC9GrpcHandler` cannot be covered at all — it saves an arbitrary player by GUID over gRPC with
no packet to precede. It is gated behind `Cluster.Enabled`, which defaults to `false`, so it is
inert unless the realm runs in cluster mode.

Everything else in that table is reached only through a packet handler, and no packet handler
can run without the drain. `opHandle->Call` appears in exactly four places in the whole server —
`WorldSession.cpp:470,491,504,523`, the `STATUS_LOGGEDIN`, `STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT`,
`STATUS_TRANSFER` and `STATUS_AUTHED` arms of one switch — and each is immediately preceded by
`CanPacketReceive`. `WorldSocket::ProcessIncoming` only queues. Nor is any of those functions
reachable off a packet: `Player::RefundItem` has one caller (`ItemHandler.cpp:1453`), and
`Guild::PlayerMoveItemData` exists only to serve `CMSG_GUILD_BANK_SWAP_ITEMS`.

#### Handlers that can reach *into* a live vault

A separate matter from saving, and worth stating because it is not obvious.
`Player::GetItemByGuid` scans bank slots and bank bags (`PlayerStorage.cpp:423,435`), and mail
(`MailHandler.cpp:210,240`) and auction (`AuctionHouseHandler.cpp:205`) resolve their items with
it. So `CMSG_SEND_MAIL` and `CMSG_AUCTION_SELL_ITEM` can take an item straight out of an open
vault. The stock client cannot do it — its mail and auction frames accept only items dragged
from the bags — but a forged packet can, exactly as `CMSG_SET_TRADE_ITEM` can.

The outcome is correct either way: the item leaves the bank, and the next drain sees it missing,
drops its vault row and writes the core's inventory in one transaction. The residue is the tick
between the handler's commit and that drain, during which the item is in `mail_items` or
`auctionhouse` *and* still listed in a vault. A crash there would leave a stale vault row whose
`item_instance` row still exists, and opening that vault afterwards would load a second copy.
`SelfHealVaultRows` therefore checks all four tables at login, not just `character_inventory`.

The drain is a lock-free atomic check unless the player actually has a vault open, and beyond
that it compares the live bank against the layout last written and returns without touching the
database when nothing has moved.

That comparison is a single hash-map pass producing a delta — which rows entered the vault,
which moved, which left — and the flush writes exactly those. Dragging one item inside a full
vault costs one `DELETE` and one multi-row `INSERT`, not a rewrite of all 287 possible rows.
The two are ordered deliberately: every row that is about to move is deleted before any is
written back, because two items swapping places would otherwise collide on
`UNIQUE KEY (owner_guid, vault, bag, slot)` — the first `INSERT` landing on a slot its previous
occupant has not vacated yet — and abort the whole transaction. A row left untouched cannot
collide with one that moved, because a position only becomes free when whatever held it is
itself in that delete.

What the drain deliberately does **not** do is call `Player::SaveInventoryAndGoldToDB`. Taking
the vault's items out of the queue is the entire requirement — `_SaveInventory` returns early on
an empty queue and writes nothing about items that are not in it. Pulling the core's inventory
save into the hot path as well would run its buyback-slot purge, its position cheat-detection
(which can mark an item `ITEM_REMOVED`) and its queue clear at arbitrary packet boundaries,
hundreds of times a minute. It is called only from the two places where the `Item` objects are
about to be freed: a vault switch and logout.

Durability is therefore identical to vanilla: both flush on the same `SaveToDB` cadence.

### Thread safety

`Player::Update` — and therefore `OnPlayerUpdate` — runs on the `MapUpdater` worker pool, so
with `MapUpdate.Threads > 1` two players on different maps reach the module's containers in the
same tick. Every entry point takes a `std::recursive_mutex`; recursive because a switch calls
into the core, which calls back through `OnPlayerSave`. The two hooks that run for every player
every tick and for every packet are gated on an `std::atomic` count first, so a realm with no
vault open pays an atomic load and nothing else.

## Bank bag slots

Each vault has its own count of purchased bank bag slots. The stock purchase path is
untouched: `WorldSession::HandleBuyBankSlotOpcode` reads the live count, prices the next slot
from `BankBagSlotPrices.dbc` and increments it. The shipped 3.3.5a prices are 10s, 1g, 10g,
25g, 25g, 25g, 25g — 111g 10s for all seven.

The DBC also carries rows 8..12, at 999,999,999 copper each. That is under `MAX_MONEY_AMOUNT`
(2,147,483,646), so `HandleBuyBankSlotOpcode` — which checks nothing but the row existing and
`HasEnoughMoney` — would sell them. It never gets the chance from a stock client: `PurchaseSlot()`
gates on the bank bag slot byte being `< 7` and only then sends `CMSG_BUY_BANK_SLOT`, and
`GetNumBankSlots()` reports `full` from the same byte with `count > 6`, so the UI hides the
purchase frame and an eighth attempt is refused before any packet is built. Seven is a client
constant, not a consequence of the price table. The module inherits that bound: `bag_slots` can
only ever reach 7 through play.
Because the live count is set to the open vault's count when that vault is opened, the in-bank
"Purchase" button prices and scopes itself to that vault with no handler override at all.

`PLAYER_BYTES_2` — where the count lives — is saved into `characters`, so the module keeps
vault 1's count current in `mod_extended_bank_vaults` and restores it from
`OnPlayerLoadFromDB`, which runs before `Player::_LoadInventory`. Without that, a server crash
while another vault was open would leave the character with too few bank bag slots and the
core would **mail vault 1's bank bags back to the owner**.

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

Because vault 1 is listed as its own entry, the core-generated banker option would be a
duplicate. The module therefore builds the NPC's normal menu with `Player::PrepareGossipMenu`,
copies every option except `GOSSIP_OPTION_BANKER` (together with its sub-menu link), clears
the gossip menu, adds the vault entries, and re-adds the copied options underneath. A banker
that is also an innkeeper or a vendor keeps those options and their sub-menus. The quest menu
is left untouched.

Vault entries carry a module-private gossip `sender`, so anything else falls through to the
core handler unchanged.

Because the vault list stands in for the banker option, it is offered **only where that option
is**. If the menu the core built contains no `GOSSIP_OPTION_BANKER` entry — which is how the
core says this player may not bank here — the module changes nothing and hands the NPC back, so
the result is identical to the module not being installed. See [Design consequences](#design-consequences-and-why-each-one-is-accepted)
for why that is the whole of the condition handling.

### How the menu is reached on a banker with no gossip flag

Most banker NPCs have `UNIT_NPC_FLAG_BANKER` but not `UNIT_NPC_FLAG_GOSSIP`. For those the
client does not send `CMSG_GOSSIP_HELLO` on right-click at all — it sends
`CMSG_BANKER_ACTIVATE`, and `WorldSession::HandleBankerActivateOpcode` answers with
`SMSG_SHOW_BANK` directly. There is no script hook in that path, so
`AllCreatureScript::CanCreatureGossipHello` never fires and the bank frame opens with no menu.

The module therefore also registers a `ServerScript` on `SERVERHOOK_CAN_PACKET_RECEIVE`. When
the opcode is `CMSG_BANKER_ACTIVATE` and the module is enabled, it reads the banker GUID from
the packet, repeats the stock `GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BANKER)` guard,
sends the vault menu, and returns `false` so the core handler never runs. Anything that fails
a guard is passed through untouched, and with `ExtendedBank.Enable = 0` the hook returns
immediately on the opcode check.

No creature's `npcflags` are modified. Adding `UNIT_NPC_FLAG_GOSSIP` at runtime would work
too, but AzerothCore's `creature` table has an `npcflag` column that `Creature::SaveToDB`
writes, so a GM saving a banker would persist the added flag into the world DB.

Bankers that *do* have the gossip flag still arrive through `CanCreatureGossipHello`; both
paths call the same menu builder.

## Renaming

`Rename and Reorder Vaults` lists every owned vault, including vault 1 (whose name is only a
label — it changes nothing about its storage), as `Rename "<name>"`. Clicking one opens the
client's gossip text box. The same submenu carries the `Move "<name>" Up` entries described
under [Reordering](#reordering); the two share a menu but are otherwise unrelated.

Content is not filtered. UI escape sequences are preserved on purpose, so colour and inline
icons work:

```
|cff00ff00Herbs|r
|TInterface\Icons\INV_Misc_Bag_08:16|t Alts
```

An unterminated `|c` will bleed colour into the next menu line and a malformed `|T` renders as
literal text — cosmetic, self-inflicted, and only ever visible to the vault's owner. The value
is written with an escaped statement, so its content cannot affect the query.

**The client doubles every `|` it sends from an edit box.** A player typing `|cffff0000Herbs|r`
arrives at the server as `||cffff0000Herbs||r`, and `||` renders as a literal pipe — which is
why escape sequences appeared as plain text until this was handled. `RenameVault` collapses the
pairs back before storing. A literal `|` in a vault name is therefore not reachable, which is
the same trade the game itself makes everywhere else. Names stored before this was fixed still
contain the doubled form and need renaming once.

### Length limit

A name is truncated to **240 characters** (`EXTENDED_BANK_VAULT_NAME_MAX_CHARS`).

Nothing between the text box and the packet enforces a limit on its own, in either direction.
The client's rename box is `StaticPopupDialogs["GOSSIP_ENTER_CODE"]` in `StaticPopup.lua`, which
declares no `maxLetters` at all — and `StaticPopup_Show` calls `editBox:SetMaxLetters` only when
the dialog supplies one, so the box keeps whatever limit the last dialog to set one left behind.
On the way in, `WorldSession::HandleGossipSelectOptionOpcode` reads the code string with no cap,
and on the way out `PlayerMenu::SendGossipMenu` writes the option text straight into
`SMSG_GOSSIP_MESSAGE` with no cap either. The module's own truncation is the only bound there
is. The practical ceiling is what Blizzard shipped — across 3.3.5a's own `gossip_menu_option`
data the longest `OptionText` is 397 characters and the longest `BoxText` is 102, so a single
gossip line of that order is known to render. 240 sits under that with room for the rename
prompt, which embeds the name in `Enter a new name for {}:`, and under the `VARCHAR(255)` `name`
column.

Truncation happens **before** escaping, and `TruncateAndQuote` exists to keep the two together.
The order is a security property: the `UPDATE` is the module's only statement that interpolates
a player-supplied string, and escaping first would let truncation bisect an escape pair and
leave a trailing backslash that escapes the statement's own closing quote. Prepared statements
are not available to a module — they live in the core's `CharacterDatabaseStatements` enum —
so the escape is `mysql_real_escape_string` through the live connection handle, on a connection
set to `utf8mb4`, which is what rules out the multi-byte lead-byte bypass. The format string is
a literal, so the name is only ever an fmt *argument* and braces in it are inert.

Truncation uses the core's `utf8truncate`, the same helper `Guild::BankTab::SetText` uses for
guild bank tab text. It counts characters rather than bytes, so a multi-byte name is never
left with a split sequence, and it clears the string outright when the input is not valid
UTF-8. An empty name falls back to the default `Vault N` label.

The truncated string is what goes into both the database and the in-memory cache. Assigning
the untruncated name to the cache is what previously turned an over-length rename into a
permanently broken gossip window: the `UPDATE` failed with MySQL error 1406 (`Data too long
for column 'name'`) while the menu kept re-sending the name the database had rejected.

Confirmed against a long Cyrillic name typed into the client: it stored as exactly 240
characters and 480 bytes, a clean two bytes per character, so the cut fell on a boundary and
nothing was mangled. Note that `VARCHAR(255)` counts characters rather than bytes in MySQL, so
the 480-byte name fits comfortably — the character count is the only thing 240 has to respect.

## Reordering

Vaults can be moved up and down the menu. The whole feature is one column,
`mod_extended_bank_vaults.sort_order`, and the reason it stays that small is worth stating
plainly, because there is an obvious-looking alternative that is much worse.

**The vault number is never touched.** `vault` is the identity column: it is half the primary
key of the metadata table, part of `UNIQUE KEY (owner_guid, vault, bag, slot)` on the item
table, and the thing that decides which of the two storage tables an item belongs to.
Renumbering vaults to reorder them would mean rewriting `mod_extended_bank_vault_items.vault`
for every affected row, and a swap collides on that unique key exactly the way two items
swapping slots inside one vault do — the same trap `PersistVaultLayout` pays for with
delete-before-insert. The difference is the blast radius. A botched layout flush loses an edit;
a botched renumber files items under the wrong vault. And the player would see nothing for it,
because the numbers are internal.

**The gossip action already carried the vault number**, as
`EXTENDED_BANK_ACTION_OPEN_BASE + vault`, so menu position and vault identity were independent
before any of this existed. Reordering the list changes the order of a loop, and nothing else.

That makes the whole feature inert with respect to the invariant. No item row is read or
written, so reordering needs none of the guards a vault switch does: it works with a vault
open, in combat, mid-trade, and it cannot be made to duplicate or strand an item by any
sequence of clicks.

**Positions are renumbered from zero on every move**, in `PersistVaultOrder`, rather than
swapping the two affected rows. That costs one `UPDATE` per vault on an action a player takes
by hand — at most twenty, on a click — and buys a property worth more than the saving: the
result never depends on what the previous values were. A realm that has never reordered has
255 in every row; a half-applied write from an earlier abort leaves gaps or duplicates. Both
resolve themselves the first time anything moves. The column deliberately carries no unique
key, and `GetOwnedVaults` sorts on `(sort_order, vault)`, so even a partially applied
renumbering is still a total order rather than an error.

**A newly bought vault sorts last** because `BuyNextVault` writes `EXTENDED_BANK_SORT_LAST`
(255) rather than a position. On a list nobody has reordered every row holds 255, they all
tie, and the tiebreak on `vault` reproduces exactly the order the menu had before the column
existed. Once anything is moved the list is renumbered 0..N-1, and 255 keeps meaning "after
everything placed so far" for whatever is bought next.

**The Main Vault is pinned to the front** rather than sorted there, so its own `sort_order` can
never matter. It *is* `character_inventory` and the resting state of the bank; a fixed anchor
is worth more than the freedom to bury it.

**Moving is offered only where it does something.** The menu lists `Move "<name>" Up` from the
third entry down — the first is the pinned default vault and the second has only that above
it — so no line in the menu silently fails. `MoveVaultUp` refuses the same two cases anyway,
which is what lets the debug command distinguish "already at the top" from "does not own it".

Renaming and reordering share one submenu because they are the two things a player does to a
vault rather than to its contents, and because a move entry has to sit beside the name it
moves to be readable. They are otherwise entirely untangled: a move never touches a name, a
rename never touches a position, and the default `Vault N` label keeps following the vault
number rather than the menu position — so moving one vault can never appear to rename another.

## Design consequences, and why each one is accepted

- **Stowed vaults are invisible to item lookups.** Items in a vault that is not currently open
  do not count toward `Player::GetItemCount(..., inBankAlso)`, so they are ignored by quest
  requirements, unique-item checks and similar scans. This is inherent to having more than one
  bank, not a bug. A consequence: a unique item stored in a stowed vault will not stop you
  acquiring a second copy, and re-opening that vault will then mail the surplus back to you.
- **Login-time cleanups do not apply to stowed vaults.** `Player::_LoadItem` deletes conjured
  items, items limited to another map or zone, and expired holiday items at login. The module
  deliberately does not repeat those checks when a vault is opened, because doing so would
  destroy items simply for opening a vault in the wrong place. Refund windows and
  soulbound-trade windows *are* restored.
- **Faction and race change skip vaults 2..N.** The core converts only items reachable through
  `character_inventory` (`CHAR_UPD_CHAR_INVENTORY_FACTION_CHANGE`).
- **`.pdump` does not know about the module tables.** A dumped and reloaded character loses
  vaults 2..N.
- **Playerbots** read `BANK_SLOT_*` directly and always see vault 1, which is unaffected.
- **Switching is refused in combat and during a trade; the automatic reverts are not.** That
  asymmetry is deliberate. The guard is UX — it stops a player *choosing* to swap at a silly
  moment — and it is not a safety property, so the reverts triggered by walking away, changing
  map or logging out do the identical detach with no check. Adding the guard to those would be
  strictly worse: a revert that refuses leaves a vault live after the player has left the
  banker, which breaks the resting-state invariant everything else depends on.

  The trade half is not quite theatre. `HandleSetTradeItemOpcode` resolves its position with
  `_player->GetItemByPos(bag, slot)` (`TradeHandler.cpp:895`) and performs no position-class
  check, and `Player::GetItemByPos` returns `m_items[slot]` for any slot below
  `BANK_SLOT_BAG_END` — the whole bank — while `GetBagByPos` accepts bank bag slots too. So a
  forged `CMSG_SET_TRADE_ITEM` can put a vault item into a trade. The consequence is mild
  because `TradeData` stores `ObjectGuid`, not `Item*`, and resolves through
  `Player::GetItemByGuid`: once the module frees the item during a revert that resolves to
  `nullptr`, and every use site in `HandleAcceptTradeOpcode` is null-checked. The item silently
  drops out of the trade. No dangling pointer, no duplication.
- **Two kinds of item are not allowed in vaults 2..N**, for the same underlying reason: a
  stowed vault is invisible to a rule the game enforces elsewhere, so parking an item there
  would buy its owner something the game does not sell. Either kind, found in a module-owned
  vault, is moved back to the player's bags with a message, or mailed if the bags are full.
  The check runs on every drain, so it also cleans out items stored before the rule existed,
  the first time that vault is opened. The default vault is unaffected in both cases — it *is*
  `character_inventory`, so the core's own accounting already sees it.

  **Items the game caps per character.** `MaxCount` and `ItemLimitCategory` are enforced by
  counting `character_inventory`, which a stowed vault is not part of, so parking a unique or
  quest item in one would let its owner acquire another. The check mirrors
  `Player::CanTakeMoreSimilarItems` (`PlayerStorage.cpp:818`), including the `2147483647`
  sentinel that means "no limit" despite being positive; it covers 5802 shipped templates.

  **Items with a duration.** The clock is driven by `Player::UpdateItemDuration` walking
  `m_itemDuration`, and detaching a vault calls `Player::RemoveItem`, which calls
  `RemoveItemDurations` — so stowing a vault freezes its items' timers outright. The tempting
  objection is that vanilla already pauses these: at login the catch-up is
  `UpdateItemDuration(time_diff, true)` (`PlayerStorage.cpp:5584`), and `realtimeonly` skips
  anything without `ITEM_FLAGS_CU_DURATION_REAL_TIME`, so an ordinary duration item in the
  vanilla bank already stops ticking while its owner is logged out. That misses what the two
  pauses cost. Vanilla's is paid for in playing time — to stop the clock you have to stop
  playing. A vault stops the same clock for free while you carry on, and it does so for an
  item the player can still reach at any banker. Same effect, no price, which is exactly what
  this rule exists to refuse. It covers all 280 templates with a duration, not just the 70
  real-time-flagged ones a narrower rule would have caught, because the free-pause argument
  does not depend on the flag. The template is authoritative rather than the live
  `ITEM_FIELD_DURATION`, since `Item::LoadFromDB` forces the two into agreement
  (`Item.cpp:452`).
- **A newly bought bank bag slot may not appear until the bank is reopened.** Seen several
  times while buying slots with a vault open: the frame kept showing the previous number, so
  after buying two slots it looked like one had been lost, and the final purchase left the last
  slot looking unavailable while the game still said no more could be bought. **Nothing is
  actually wrong** — the server's count is correct throughout, the gold is deducted once, and
  the slot is there. Closing and reopening the bank shows the right number, and a relog is
  never needed. A controlled attempt to reproduce it afterwards failed, so the cause is not
  known and it may not be specific to this module; the frame is now closed explicitly before
  the bank opens, which is the most likely remedy. Worth knowing about because it looks alarming
  and invites a bug report about lost gold, when nothing has been lost.

  Later evidence points at an addon rather than at this module. The stock Blizzard bank frame
  was confirmed on 2026-09-03 to handle per-vault slot counts correctly in every direction:
  the purchase button appears and disappears as the open vault's count crosses seven, across
  vault switches and relogs alike. The client is well behaved here by construction —
  `UpdateBagSlotStatus` (`FrameXML/BankFrame.lua:103`) derives both the locked-slot tint and
  the purchase frame's visibility from the same `GetNumBankSlots()` call, whose second return
  is computed live as `count > 6` and is not cached anywhere. **ElvUI, by contrast, stops
  offering the purchase button once vault 1 has bought all seven slots, and does not offer it
  again in a vault that has fewer.** That is the only UI seen to get this wrong, it is out of
  this module's control, and `/run PurchaseSlot()` is a complete workaround — that function
  gates on nothing but the live count being below seven, so it always follows the open vault.
- **A rejected item is announced twice, because a chat line alone is not enough.** When a
  capped item is refused, it leaves the cursor and vanishes from the bank in the same instant,
  which reads as item loss to the player. The module therefore writes the detail to chat *and*
  has the banker whisper it. The item is never actually lost: it is returned to the bags, or
  — if there is nowhere to put it, which happens when the drop *swapped* with an occupied slot
  and refilled the source — mailed.

  The whisper is sent as a boss whisper (`Unit::Whisper(..., isBossWhisper = true)`). On a
  stock 3.3.5a client that does *not* draw a chat bubble over the NPC — it renders as a
  full-screen system-style notice, which is the desired effect anyway, and it is the form
  addons hook to play an alert sound. `ChatHandler::SendNotification` was tried alongside it
  and dropped: it looks almost the same and stays on screen for less time, so it added nothing.
  A vault opened through the GM `.bank` convention has no banker creature, so there the chat
  line is the only announcement.
- **Conditions on an NPC's banker option are honoured, by inheriting the core's answer.** The
  vault list stands in for the stock `GOSSIP_OPTION_BANKER` entry, so it is offered only where
  that entry is. `Player::PrepareGossipMenu` omits an option whose `conditions` row fails
  (`PlayerGossip.cpp:60`) and applies no further check to a banker option, so one surviving into
  the built menu *is* the core's statement that this player may use this bank. When none does,
  the module returns false and the NPC is handed back untouched — the core re-runs
  `PrepareGossipMenu`, which is idempotent, and sends the menu through `SendPreparedGossip`,
  whose quest-menu fallback and menu-aware text id the module does not try to reproduce.

  Jeeves (entry 35642) is the only creature in the stock database that gates a banker option —
  `CONDITION_SKILL 202/350`, Master Engineering — and until this was added the vault list handed
  his bank to anyone who could reach him. The check costs nothing anywhere else: of 55 bankers,
  14 carry the gossip flag, and every one of them produces a banker option, four of them through
  the default menu 0 fallback that fires when a creature's own menu has no options at all.
- **A vault opened by a scripted NPC is skipped.** A banker whose `creature_template` carries
  a `ScriptName` is left entirely alone, so it keeps its own gossip and gains no vault list.
  `ScriptMgr::OnGossipHello` asks every `AllCreatureScript` first and stops at the first one
  that returns true (`CreatureScript.cpp:34`), so claiming such an NPC would stop its
  `CreatureScript` from ever running — and a menu that script builds in code, rather than in
  `gossip_menu_option`, cannot be carried across the module's menu rebuild. Losing a custom
  NPC's options is worse than that NPC having no vaults.
- **An item leaving a vault is written atomically.** Dropping an item's vault row without
  writing the `character_inventory` row that says where it went would leave it in neither
  table until the next periodic save — an orphan window of up to `PlayerSave.Interval`, and a
  crash inside it strands the item (its `item_instance` row survives, so a GM can recover it,
  but nothing else can). The drain therefore pulls the core's `SaveInventoryAndGoldToDB` into
  the same transaction whenever it notices that something the vault used to hold is no longer
  in the live bank. It does **not** do so otherwise: `_SaveInventory` also purges buyback
  slots, runs position cheat-detection that can mark an item `ITEM_REMOVED`, and clears the
  whole update queue, none of which should happen on an arbitrary packet boundary. This gap
  was found by testing, not by reading — see `tools/TESTING.md` §3.

## Compatibility

Any other module that hooks `AllCreatureScript::CanCreatureGossipHello` on banker NPCs will
conflict — the first script to return `true` wins and the other never runs. This module only
claims creatures that have `UNIT_NPC_FLAG_BANKER` and **no** `ScriptName`.

The same applies to `SERVERHOOK_CAN_PACKET_RECEIVE`: a module that swallows
`CMSG_BANKER_ACTIVATE` before this one does will suppress the vault menu on bankers that have
no gossip flag. This module only swallows that one opcode, and only for a creature the player
can legitimately bank with; for every other packet its hook drains the live vault and returns
`true`.

`MapUpdate.Threads > 1` is supported — see [Thread safety](#thread-safety).
