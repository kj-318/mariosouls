# Documentation

This fork is the **Dark Souls Remastered** port of MarioSouls. Player guides are for Remastered. Developer pages that describe the original PTDE host say so, and are kept because the port follows that host's behavior.

## For players

Start with the [project README](../README.md), then:

| Guide | What it answers |
| --- | --- |
| [Installation](INSTALLING.md) | Copying the mod into the game folder, the ROM, first-launch collision, settings, update/uninstall |
| [Playing](PLAYING.md) | Mario and the knight, controls, combat, Wing Cap, world interactions, known gaps |
| [Compatibility](COMPATIBILITY.md) | Supported game version, what has been checked, other mods |
| [Troubleshooting](TROUBLESHOOTING.md) | Mario missing, collision, stuck states, visuals |
| [Bug reports](BUG_REPORTING.md) | What to include and where to report |
| [Recovery](RECOVERY.md) | Removing the mod, save backups, earlier source |

## For contributors

- [Port status and handoff](../framework/hosts/ds1remastered/PORT_STATUS.md): the current Remastered state, architecture, offsets and backlog. Start here.
- [Remastered findings](REMASTERED_FINDINGS.md) and [port working notes](remastered/): live-run evidence and earlier plans.
- [Development](DEVELOPMENT.md): building and packaging the Remastered host.
- [Verification](TESTING.md): manual checks for Remastered.
- [Contributing](../CONTRIBUTING.md) and [AGENTS.md](../AGENTS.md).
- [Framework architecture](../framework/README.md), [architecture rules](../framework/docs/ARCHITECTURE.md), [SDK contract](../framework/docs/CONTRACT.md) and [adapter guide](../framework/docs/ADDING_A_CHARACTER.md).
- PTDE reference (original host, not built by this fork):
  - [PTDE behavior checklist for Remastered](../framework/docs/REMASTERED_PARITY.md)
  - [Remastered handoff](../framework/docs/REMASTERED_HANDOFF.md)
  - [host porting](../framework/docs/HOST_PORTING.md)
  - [PTDE integration inventory](../framework/hosts/ds1ptde/PORT_CHECKLIST.md)
  - [Features and tweaks](FEATURES_AND_TWEAKS.md) (PTDE behavior inventory)
  - [PTDE release preparation](RELEASING.md)
- [Changelog](../CHANGELOG.md), [license](../LICENSE), [third-party notices](../THIRD_PARTY.md) and [security guidance](../SECURITY.md).
