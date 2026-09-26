-- Rune Tablet episode target groups (membership is still pending).
-- Apply only to installations using SQL monster databases.
-- These nullable fields do not assign any monster to a group.

ALTER TABLE `mob_db`
  ADD COLUMN `racegroup_rune_ep18` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_encroached_gephenia`,
  ADD COLUMN `racegroup_rune_ep19` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep18`,
  ADD COLUMN `racegroup_rune_ep20` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep19`,
  ADD COLUMN `racegroup_rune_ep21` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep20`;

ALTER TABLE `mob_db2`
  ADD COLUMN `racegroup_rune_ep18` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_encroached_gephenia`,
  ADD COLUMN `racegroup_rune_ep19` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep18`,
  ADD COLUMN `racegroup_rune_ep20` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep19`,
  ADD COLUMN `racegroup_rune_ep21` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep20`;

ALTER TABLE `mob_db_re`
  ADD COLUMN `racegroup_rune_ep18` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_encroached_gephenia`,
  ADD COLUMN `racegroup_rune_ep19` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep18`,
  ADD COLUMN `racegroup_rune_ep20` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep19`,
  ADD COLUMN `racegroup_rune_ep21` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep20`;

ALTER TABLE `mob_db2_re`
  ADD COLUMN `racegroup_rune_ep18` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_encroached_gephenia`,
  ADD COLUMN `racegroup_rune_ep19` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep18`,
  ADD COLUMN `racegroup_rune_ep20` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep19`,
  ADD COLUMN `racegroup_rune_ep21` tinyint(1) unsigned DEFAULT NULL AFTER `racegroup_rune_ep20`;
