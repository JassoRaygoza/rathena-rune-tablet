# rAthena Rune Tablet

Server-side implementation of the kRO **Rune Tablet** system for
[rAthena](https://github.com/rathena/rathena) (Renewal), plus the client files
it needs. This repository contains only the Rune Tablet work, not the emulator.

- Activate rune fragments and sets (materials are validated and consumed server-side)
- Set upgrades +1..+15 with failure counter (a failed upgrade keeps the level)
- Equip/unequip one set; stat bonuses and special effects applied on status rebuild
- Collection rewards per set
- Card decomposition into Imperfect/Perfect Runes
- Chapter 1 and Episode 18-20 monsters, spawns and race groups for set bonuses
- Rune Stone NPCs (official access point) and a GM `@runetablet` command

**Status:** 64 of 65 sets implemented (EP21 set 1260048 has no published monster
data). The map-server compiles against rAthena master `e98500617`. In-game testing
and feedback are welcome.

## Requirements

- rAthena master (tested base: `e98500617`, 2026-08-21), Renewal mode
- A client dated 2023-08-02 or newer with the Rune Tablet UI
  (reward packets need 2024-10-16 or newer). Developed with a 2026-02-19 client.
- `PACKETVER` set to your client's date (see [INSTALL.md](INSTALL.md))

## Repository layout

| Path | Contents |
| --- | --- |
| `patch/rune_tablet.diff` | Complete patch for rAthena (modified and new files). **This is what you apply.** |
| `server/` | The new server files from the patch, in their rAthena paths, for browsing and review |
| `client/System/Rune/` | The 10 Rune Tablet tables the client EXE loads (`System\Rune\` is hardcoded) |
| `client/SystemEN/itemInfo_RuneTablet.lua` | Optional. English names for the reward boxes and new cards. It goes in `SystemEN` only because this EXE loads item info from `SystemEN\iteminfo.lua`; the Rune Tablet works without it |
| `INSTALL.md` | Full installation and testing guide |

## Quick install

```sh
cd /path/to/rathena
git apply /path/to/rathena-rune-tablet/patch/rune_tablet.diff
# set PACKETVER in src/custom/defines_pre.hpp, e.g. #define PACKETVER 20260219
# rebuild the map-server and restart
```

Then copy `client/System/Rune/*` into the client's `System/Rune/` folder (required).
Optionally add `itemInfo_RuneTablet.lua` to your `SystemEN/itemInfo.lua` loader
so the reward boxes and new cards show their names. By default only
GM level 99 can open the tablet (`rune_tablet_min_group_level` in
`conf/battle/feature.conf`). See [INSTALL.md](INSTALL.md) for details.

## Contributing

Fork the repository, make your change and open a pull request.

- Server code changes: update `patch/rune_tablet.diff` (and the matching file in
  `server/` if it is a new file). The simplest way is to work in a patched rAthena
  checkout and regenerate the diff with `git diff --binary <base> > rune_tablet.diff`.
- Database changes: edit the YAML files in `db/re/` and include them in the patch.
- Keep all player-facing text in English.
- Make sure the map-server compiles and starts without Rune Tablet warnings.
- Bug reports: open an issue with your rAthena commit, client date and PACKETVER.

## Credits and license

The server code is released under the **GNU GPL v3** (see `LICENSE`),
the same license as rAthena, since the patch is derived from rAthena.

The client files in `client/System/Rune/` come from the kRO client (Gravity Co.)
and the English translations of [ROenglishRE](https://github.com/llchrisll/ROenglishRE)
(zackdreaver, llchrisll). Item descriptions are based on
[Divine Pride](https://www.divine-pride.net/) data. Those files remain the
property of their respective owners and are included only for interoperability.
Ragnarok Online is a trademark of Gravity Co., Ltd.
