# Rune Tablet: installation on a clean rAthena (Renewal)

Base: upstream `rathena/rathena` master `e98500617` (2026-08-21).

Status: 64 of 65 sets resolved (EP21 set 1260048 is missing: no published data
for its monsters). Decomposition is active with a uniform local policy.
The map-server compiles; in-game testing is still required.

## 1. Code (requires compiling the map-server)

Apply the patch from the rAthena root:

```
git apply /path/to/rathena-rune-tablet/patch/rune_tablet.diff
```

The patch already contains every new file. The `server/` folder of this
repository holds the same new files, only for browsing and review.

Only one new source file: `src/map/rune_tablet.cpp`. In order, it contains the
pure rules (`rune_tablet_logic`), the packet formats (`rune_tablet_wire`) and
the implementation. There is no `rune_tablet.hpp`: the 9 public functions are
declared in `src/map/pc.hpp`, next to `pc_delitem`, because every caller already
includes that file. CMake picks it up automatically (`GLOB_RECURSE`); in Visual
Studio it is added to `map-server.vcxproj` and `map-server-generator.vcxproj`.

| File | Change |
| --- | --- |
| `src/map/pc.hpp` | Window state, upgrade anti-spam, `pc_cant_act2`, optional parameters for `pc_additem`/`pc_delitem`, Rune Tablet prototypes |
| `src/map/pc.cpp` | `pc_additem(..., update_observers)`, `pc_delitem(..., update_quest)`, window close on `pc_setpos` |
| `src/map/battle.cpp` | Special effects when getting hit / killing with targeted magic (`battle_damage`) |
| `src/map/status.cpp`, `status.hpp`, `script_constants.hpp` | `SC_RUNE_TABLET_REGEN` and bonus application in `status_calc_pc_sub` |
| `src/map/clif.cpp`, `clif_packetdb.hpp` | Sync on map load and packets 0x0bcb..0x0c17 |
| `src/map/map.cpp` | `rune_tablet_init` / `rune_tablet_final` |
| `src/map/map.hpp`, `script_constants.hpp`, `mob.cpp` | `RC2_RUNE_EP18..21` (enum, constants, SQL columns) |
| `src/custom/atcommand.inc`, `atcommand_def.inc` | `@runetablet [close]` (GM 99) |
| `src/custom/script.inc`, `script_def.inc` | Script command `runetablet;` (Rune Stone NPC) |
| `src/custom/battle_config_struct.inc`, `battle_config_init.inc` | `rune_tablet_min_group_level`, `rune_tablet_decomposition_uniform` |

**PACKETVER (not included in the diff):** upstream ships 20211103, and with that
value the Rune Tablet packets are not registered (they need `>= 20230802`;
rewards need `>= 20241016`). Set it in `src/custom/defines_pre.hpp`:

```
#define PACKETVER 20260219
```

20260219 is the value that works with the Rune Tablet client. If you use a
different client, set it to that client's date.

## 2. Data (no compile needed; restart the map-server)

Everything lives in `db/re/`, linked with `Mode: Renewal` from `db/mob_db.yml`
and `db/item_db.yml`, after the Renewal DB and before `db/import` (your imports
can still override it).

| File | Contents |
| --- | --- |
| `db/re/rune_tablet_db.yml` | 198 records: activation, effects, upgrades, rewards |
| `db/re/rune_tablet_decomposition.yml` | 1163 decomposable cards |
| `db/re/mob_db_rune_ch1.yml` + `npc/custom/rune_tablet/ch1_mobs.txt` | 19 Chapter 1 monsters and 33 spawns |
| `db/re/mob_db_rune_ep19_20.yml` + `npc/custom/rune_tablet/ep19_20_mobs.txt` | 45 EP19/EP20 monsters with `Rune_Ep19`/`Rune_Ep20`, 74 spawns |
| `db/re/mob_db_rune_episodes.yml` | `Rune_Ep18` on 31 EP18 monsters from `db/re/mob_db.yml` |
| `db/re/item_db_rune_tablet.yml` | 7 reward boxes (106408..106414); their groups already exist in `db/re/item_group_db.yml` |
| `db/re/item_db_rune_cards.yml` | Cards 27266 and 300483, missing upstream (Divine Pride data) |
| `db/re/status.yml` (+ `db/pre-re/status.yml`) | `Rune_Tablet_Regen` entry |

**Monster stats:** taken from Divine Pride as-is (Renewal), e.g. Grotesque
Skogul DEF 287 / MDEF 1126. Names longer than 23 characters are abbreviated,
and 21974..21978 are named "Fake Iwin Soldier" (Korean-only name on Divine
Pride); the original name is kept in a comment.

## 3. Scripts, configuration and SQL

- `npc/custom/rune_tablet/rune_stone.txt`: 7 Rune Stone NPCs (official access point).
- `npc/scripts_custom.conf`: loads `rune_stone.txt`, `ch1_mobs.txt` and `ep19_20_mobs.txt`.
- `npc/custom/warper.txt`: "Chapter 1 Dungeons" (D54) and "Episode 18-20 Areas"
  (D55) entries. **The warper is disabled upstream**
  (`//npc: npc/custom/warper.txt` in `npc/scripts_custom.conf`); enable it if you use it.
- `conf/battle/feature.conf`: `rune_tablet_min_group_level: 99` (GM only) and
  `rune_tablet_decomposition_uniform: yes`.
- `conf/atcommands.yml`: `@runetablet` help.
- SQL: only if you use `use_sql_db: yes` in `conf/inter_athena.conf`. The
  `sql-files/mob_db*.sql` schemas already include the `racegroup_rune_ep18..21`
  columns; an existing database needs
  `sql-files/upgrades/upgrade_20260923_rune_episode_groups.sql`.
  With the default YAML path nothing is needed.

Progress is stored in character variables (`RuneTabletFragment[]`,
`RuneTabletSet[]`, `RuneTabletEquipped`, `RuneTabletReward[]`); there are no new tables.

## 4. Client

Copy the contents of this repository's `client` folder into the client root:

| Path | Purpose |
| --- | --- |
| `System/Rune/*.lub` | **Required.** The 10 files the EXE loads from its hardcoded `System\Rune\` path: `runeSystem_table`, `runesystemid`, `runesystemInfo`, `runesystem_f`, `rune_info`, `rune_desc`, `runeset_info`, `runeset_desc`, `runeset_reward`, `itemDecom`. `rune_desc.lub` and `runeset_desc.lub` hold the English names |
| `SystemEN/itemInfo_RuneTablet.lua` | Optional. Names of the reward boxes and cards 27266/300483; without it the Rune Tablet still works but those items show no name |

The item file is loaded through the ROenglishRE multi-itemInfo loader
(`SystemEN/itemInfo.lua`). Add it to your loader:

```lua
ImportFiles = {
	-- ...your other files
	"itemInfo_RuneTablet.lua",
}
ImportTables = {
	-- ...your other tables
	"runetablet",
}
```

## 5. Startup and testing

1. Compile the map-server (and the generator if you use it) and restart.
2. The log must not show `Rune Tablet` warnings, `Unknown monster race group
   Rune_Ep1x`, `Invalid monster ID` or name truncation in `db/re/mob_db_rune_*.yml`.
3. Tests with GM 99 (`@item <id> <amount>`):
   - EP18 1260014: `@item 1000405 1750`, `@item 1001282 170`, `@item 1001283 1`
     and one unit each of 1001539..1001548; activate 1263035/1263036 and the set;
     equip it; in `gw_fild01` damage against Grey Wolf (21304) increases by 10%.
   - EP19/EP20: enter `jor_back1`/`jor_back5`; the monsters (21525, 21965) show
     their sprites; equip 1260023: +10% against 21525.
   - CH1: `@mobinfo 22471` shows DEF 287 / MDEF 1126 (Renewal values).
   - EP18 instances (Wolves, Villa, Thor): they are loaded in Renewal, so the
     16 instance monsters also receive the bonus.
   - Decomposition: `@item 4001 31`: x1 gives 1..2 Imperfect Rune (1001282); x30
     gives 33..66 plus a 15% chance of 1 Perfect Rune (1001283). `@item 300483 1`
     and `@item 27266 1` show their names and can be decomposed. Full inventory:
     rejected without consuming anything.
   - Rewards, upgrades (a failure keeps the level), death/map change/relog and
     server restart: progress persists.
4. Open to players: `rune_tablet_min_group_level: 0` and `@reloadbattleconf`.
   `@runetablet` stays GM 99 only; players use the Rune Stone NPC.

## 6. Known behavior

The material count shown in the Rune Tablet window is calculated by the client
when the window opens. If you take a material out of storage while the window
is open, close and reopen the Rune Tablet to refresh the count. The server always
validates the real inventory when you press Activate, so storage items can never
be used.

## 7. Reverting

`git apply -R /path/to/rathena-rune-tablet/patch/rune_tablet.diff` from the rAthena
root, then recompile.
