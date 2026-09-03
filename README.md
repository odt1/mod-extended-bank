# mod-extended-bank

Blanced, vanilla-like bank space extension module that adds new bank pages or **vaults**. No client addon, no custom frame, no client patch: pick a vault page from the banker's new gossip/dialogue window and the ordinary bank UI opens showing that vault's contents.

- Vaults 2 to 8 (by default) are the new _extra_ banks, each with its own usual 28 item slots and its own 7 purchasable bank bag slots using the original vanilla system and UI.
- Vault 1 is the character's normal bank and its storage is never modified by this module.
- New vaults/pages are purchased with the same dialogue menu, and by default are at extremely steep prices for the gold sink mechanic and balance reasons. 
- Vaults can be renamed, colour codes and inline icons supported.

## Disclamer:

Vibe-coded using Claude Opus 5 against [AzerothCore mod-playerbots branch](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot), but rigorously tested on solo server as much as possible (see tools\TESTING.md). 

Please feel free to report any issues, PRs and provide any reports and additional testing, thanks and enjoy! 

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

It cannot be dropped, because **3.3.5a sends no packet when the bank frame closes**. There is
no `CMSG_BANK_CLOSE`; the client simply hides the frame. "Swap only while the player is using
the bank" has a clear start (`SMSG_SHOW_BANK`) but no client-supplied end, so the server needs
its own way to notice that the interaction is over. Reverting to vault 1 happens on the first
of: picking another vault, walking out of the banker's interaction range (checked once a
second), changing map, or logging out.

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
hook at all. Its callers include mail (`ItemHandler.cpp:1205`), the auction house
(`AuctionHouseHandler.cpp:327,401,589,662`), the guild bank (`Guild.cpp:791,805`), item refunds
(`Player.cpp:16146`) and the partial-save timer (`PlayerUpdates.cpp:2431`). So the module
drains at three points, which between them precede every one of those:

| Where | Covers |
|---|---|
| `ServerScript::CanPacketReceive`, before every packet handler runs | every handler that reaches `SaveInventoryAndGoldToDB`, including one that fires in the same batch as the packet that dirtied the item |
| `OnPlayerUpdate` (`PlayerUpdates.cpp:315`) | `UpdateAdditionalSaves` at `:340` in the same `Player::Update` |
| `OnPlayerSave` | full character saves |

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
from `BankBagSlotPrices.dbc` (10g, 100g, 250g, 600g, 1800g, 3800g, 9000g) and increments it.
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
Rename Vaults
```

After buying one:

```
Main Vault
Vault 2
Buy Next Vault (1000 gold)
Rename Vaults
```

Because vault 1 is listed as its own entry, the core-generated banker option would be a
duplicate. The module therefore builds the NPC's normal menu with `Player::PrepareGossipMenu`,
copies every option except `GOSSIP_OPTION_BANKER` (together with its sub-menu link), clears
the gossip menu, adds the vault entries, and re-adds the copied options underneath. A banker
that is also an innkeeper or a vendor keeps those options and their sub-menus. The quest menu
is left untouched.

Vault entries carry a module-private gossip `sender`, so anything else falls through to the
core handler unchanged.

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

`Rename Vaults` lists every owned vault, including vault 1 (whose name is only a label — it
changes nothing about its storage). Clicking one opens the client's gossip text box.

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

Nothing between the text box and the packet enforces a limit on its own: the client's gossip
input box will send more than 255 characters, and `PlayerMenu::SendGossipMenu` writes the
string straight into `SMSG_GOSSIP_MESSAGE` with no cap. The practical ceiling is what Blizzard
shipped — across 3.3.5a's own `gossip_menu_option` data the longest `OptionText` is 397
characters and the longest `BoxText` is 102, so a single gossip line of that order is known to
render. 240 sits under that with room for the rename prompt, which embeds the name in
`Enter a new name for {}:`, and under the `VARCHAR(255)` `name` column.

Truncation uses the core's `utf8truncate`, the same helper `Guild::BankTab::SetText` uses for
guild bank tab text. It counts characters rather than bytes, so a multi-byte name is never
left with a split sequence, and it clears the string outright when the input is not valid
UTF-8. An empty name falls back to the default `Vault N` label.

The truncated string is what goes into both the database and the in-memory cache. Assigning
the untruncated name to the cache is what previously turned an over-length rename into a
permanently broken gossip window: the `UPDATE` failed with MySQL error 1406 (`Data too long
for column 'name'`) while the menu kept re-sending the name the database had rejected.

## Installation

```bash
cd azerothcore-wotlk/modules
git clone <this repo> mod-extended-bank
cd ../build
cmake .. -DMODULES=static -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc) && make install
```

No `CMakeLists.txt` is needed — `modules/CMakeLists.txt` picks up `src/`, `conf/*.conf.dist`
and `data/sql/` automatically. The characters-database tables in
`data/sql/characters/base/` are applied by the core's own updater on the next worldserver
start (requires `Updates.EnableDatabases` to include the characters DB, which is the default).

Copy `conf/mod_extended_bank.conf.dist` to `mod_extended_bank.conf` in the server's
`configs/modules` directory if `make install` did not place it there.

### Adding a source file

`modules/CMakeLists.txt` collects module sources with `CollectSourceFiles()`, a glob evaluated
when CMake **configures**, not when it builds. A newly added `.cpp` is therefore not in the
generated project and the build fails at link time with an unresolved symbol for whatever it
defined. Re-run the configure step once after adding a file:

```bash
cd build && cmake .
```

## Configuration

All three options are read through `ConfigValueCache` and are **hot-reloadable with
`.reload config`** — no restart required.

| Option | Default | Meaning |
|---|---|---|
| `ExtendedBank.Enable` | `1` | Master switch. When `0`, banker NPCs behave exactly like stock AzerothCore. |
| `ExtendedBank.MaxVaults` | `8` | Total vaults per character, vault 1 included, so 7 purchasable. Clamped to 1..20. |
| `ExtendedBank.VaultCost` | `"100,1000,2500,6000,18000,38000,90000"` | Price in gold for vault 2, 3, … A shorter list reuses its last value. Entries above 214748 gold — more than a character can hold — are clamped and logged, because the conversion to copper would otherwise wrap a `uint32` and make the vault cheap. |

`Enable` is checked when a banker is used, not at login: the vault list is loaded for every
character regardless. Gating the login query on it would leave a character who logged in while
the module was off with no vault list in memory, and the menu would then offer to sell them a
vault they already own.

Lowering `MaxVaults` below a character's owned vault count never hides or deletes anything; it
only blocks further purchases.

Switching vaults is refused in combat or with an open trade window. That is deliberate and not
configurable.

## Database

Two tables in the characters database. `mod_extended_bank_vault_items` mirrors the shape of
`character_inventory` plus a `vault` column, so `bag = 0` means `slot` is a global player slot
(39-73) and `bag = <bag item GUID>` means `slot` is an index inside that bank bag.

| Table | Purpose |
|---|---|
| `mod_extended_bank_vaults` | one row per owned vault: name, purchased bag slots, creation time |
| `mod_extended_bank_vault_items` | item positions for vaults 2..N only |

Character deletion is handled by the `OnPlayerDeleteFromDB` hook. `Player::DeleteFromDB`
already removes every `item_instance` row owned by the character, so only the module's own
rows need clearing.

## Known consequences

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
- **Items the game caps per character are not allowed in vaults 2..N.** `MaxCount` and
  `ItemLimitCategory` are enforced by counting `character_inventory`, and a stowed vault is
  invisible to that count — so parking a unique or quest item in a vault would let its owner
  acquire another. Any such item found in a module-owned vault is moved back to the player's
  bags with a message, or mailed if the bags are full. The check mirrors
  `Player::CanTakeMoreSimilarItems` (`PlayerStorage.cpp:818`), including the `2147483647`
  sentinel that means "no limit" despite being positive; it covers 5802 of the shipped item
  templates. It runs on every drain, so it also cleans out items stored before the rule
  existed, the first time that vault is opened. The default vault is unaffected — it *is*
  `character_inventory`, so the core's own counting already sees it.
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
- **Conditions on an NPC's banker option are not re-applied to the vault list.** The module
  builds the NPC's normal menu, drops the core's `GOSSIP_OPTION_BANKER` entry and substitutes
  its own vault entries — but those carry no conditions of their own. On a banker whose bank
  access is gated (Jeeves, entry 35642, requires Master Engineering) the vault list is offered
  to everyone, and selecting one opens the bank. The fix is to add vault entries only when the
  core actually produced a banker option, since that is what proves the conditions were met —
  but it needs checking against plain bankers first, because it would suppress the menu
  anywhere that option is not generated.
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

## Licence

GPL v2, matching AzerothCore.
