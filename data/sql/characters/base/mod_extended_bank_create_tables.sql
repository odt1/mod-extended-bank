--
-- mod-extended-bank: characters database schema
--
-- `mod_extended_bank_vault_items` deliberately mirrors the shape of the core
-- `character_inventory` table plus a `vault` column:
--   bag = 0                    -> `slot` is a global player slot (39..73)
--   bag = <bag item low GUID>  -> `slot` is an index inside that bank bag (0..35)
--
-- Vault 1 is never stored here: it lives in `character_inventory` and is owned
-- entirely by the core. Only vaults 2..N have rows in this table.
--

CREATE TABLE IF NOT EXISTS `mod_extended_bank_vaults` (
  `owner_guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `vault` TINYINT UNSIGNED NOT NULL COMMENT '1 = the vanilla bank',
  `name` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'player-chosen label, UI escapes allowed',
  `bag_slots` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'purchased bank bag slots for this vault',
  `sort_order` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT 'menu position; ties break on vault, 255 sorts last',
  `created` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix timestamp',
  PRIMARY KEY (`owner_guid`, `vault`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-extended-bank';

CREATE TABLE IF NOT EXISTS `mod_extended_bank_vault_items` (
  `item` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid',
  `owner_guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `vault` TINYINT UNSIGNED NOT NULL,
  `bag` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 or the containing bag item guid',
  `slot` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`item`),
  UNIQUE KEY `pos` (`owner_guid`, `vault`, `bag`, `slot`),
  KEY `idx_owner_vault` (`owner_guid`, `vault`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-extended-bank';
