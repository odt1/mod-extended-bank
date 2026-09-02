-- mod-extended-bank: read-only invariant checks.
--
-- Every query below must return zero rows on a healthy database. Anything that comes back is
-- a violation, labelled by the `check_name` column. Nothing here writes.
--
--   mysql -h127.0.0.1 -uacore -pacore acore_characters < tools/check_invariants.sql
--
-- Run it after any test that moves items, and again after a hard kill and restart.

-- 1. THE CORE INVARIANT. Vault 1 is character_inventory, vaults 2..N are the module's table,
--    and nothing is ever in both. A hit here means an item exists in two places at once and
--    can be duplicated.
SELECT 'item_in_both_tables' AS check_name, v.owner_guid, v.vault, v.item, ci.bag, ci.slot
FROM mod_extended_bank_vault_items v
JOIN character_inventory ci ON ci.item = v.item;

-- 2. A vault row whose item no longer exists. The item is gone; the position row is litter
--    that will log an error on every open of that vault.
SELECT 'vault_item_without_instance' AS check_name, v.owner_guid, v.vault, v.item
FROM mod_extended_bank_vault_items v
LEFT JOIN item_instance ii ON ii.guid = v.item
WHERE ii.guid IS NULL;

-- 3. A vault row whose item is owned by someone else. Should be impossible; would mean an
--    item was moved between characters without its vault row following.
SELECT 'vault_item_wrong_owner' AS check_name, v.owner_guid, v.vault, v.item, ii.owner_guid AS real_owner
FROM mod_extended_bank_vault_items v
JOIN item_instance ii ON ii.guid = v.item
WHERE ii.owner_guid <> v.owner_guid;

-- 4. Items sitting in a vault the character does not own. Reachable only through a partly
--    failed purchase or a manual edit; the menu will never show them, so they are invisible.
SELECT 'orphan_vault_number' AS check_name, v.owner_guid, v.vault, COUNT(*) AS items
FROM mod_extended_bank_vault_items v
LEFT JOIN mod_extended_bank_vaults m ON m.owner_guid = v.owner_guid AND m.vault = v.vault
WHERE m.owner_guid IS NULL
GROUP BY v.owner_guid, v.vault;

-- 5. Vault rows belonging to a character that no longer exists.
SELECT 'vault_of_deleted_character' AS check_name, m.owner_guid, m.vault
FROM mod_extended_bank_vaults m
LEFT JOIN characters c ON c.guid = m.owner_guid
WHERE c.guid IS NULL
UNION ALL
SELECT 'vault_items_of_deleted_character', v.owner_guid, v.vault
FROM mod_extended_bank_vault_items v
LEFT JOIN characters c ON c.guid = v.owner_guid
WHERE c.guid IS NULL;

-- 6. Position sanity. bag = 0 means a global bank slot (39..73); otherwise slot is an index
--    inside a bank bag and cannot exceed the largest container in the game.
SELECT 'slot_out_of_range' AS check_name, owner_guid, vault, item, bag, slot
FROM mod_extended_bank_vault_items
WHERE (bag = 0 AND (slot < 39 OR slot > 73))
   OR (bag <> 0 AND slot > 35);

-- 7. A vault item claiming to live inside a bag that is not itself in the same vault. On the
--    next open its container will not be found and it has to be mailed back.
SELECT 'contents_without_bag' AS check_name, v.owner_guid, v.vault, v.item, v.bag
FROM mod_extended_bank_vault_items v
LEFT JOIN mod_extended_bank_vault_items b
       ON b.item = v.bag AND b.owner_guid = v.owner_guid AND b.vault = v.vault AND b.bag = 0
WHERE v.bag <> 0 AND b.item IS NULL;

-- 8. Two items in the same place. The UNIQUE key should make this impossible; if it ever
--    fires, the key is missing.
SELECT 'duplicate_position' AS check_name, owner_guid, vault, bag, slot, COUNT(*) AS n
FROM mod_extended_bank_vault_items
GROUP BY owner_guid, vault, bag, slot
HAVING n > 1;

-- 9. Bank bag slot counts outside what the client can hold (0..7).
SELECT 'bag_slots_out_of_range' AS check_name, owner_guid, vault, bag_slots
FROM mod_extended_bank_vaults
WHERE bag_slots > 7;

-- 10. The default vault must never have position rows of its own.
SELECT 'default_vault_has_rows' AS check_name, owner_guid, COUNT(*) AS items
FROM mod_extended_bank_vault_items
WHERE vault = 1
GROUP BY owner_guid;

-- 11. Fully unreachable items: owned by a character but in no inventory, no vault, no mail,
--     no auction and no guild bank. This is the shape an orphan takes if a crash lands in the
--     gap between a vault flush and the core's inventory write.
--
--     Scoped to characters that actually use the module, because an established realm carries
--     unrelated orphans of its own (buyback leftovers, old bot data, modules that delete rows
--     without clearing item_instance). Drop the last line to audit the whole realm, but
--     baseline it first or the result is noise.
SELECT 'orphan_item_instance' AS check_name, ii.owner_guid, ii.guid, ii.itemEntry
FROM item_instance ii
JOIN characters c ON c.guid = ii.owner_guid
LEFT JOIN character_inventory ci ON ci.item = ii.guid
LEFT JOIN mod_extended_bank_vault_items v ON v.item = ii.guid
LEFT JOIN mail_items mi ON mi.item_guid = ii.guid
LEFT JOIN auctionhouse ah ON ah.itemguid = ii.guid
LEFT JOIN guild_bank_item gbi ON gbi.item_guid = ii.guid
WHERE ci.item IS NULL AND v.item IS NULL AND mi.item_guid IS NULL
  AND ah.itemguid IS NULL AND gbi.item_guid IS NULL
  AND ii.owner_guid IN (SELECT owner_guid FROM mod_extended_bank_vaults);
