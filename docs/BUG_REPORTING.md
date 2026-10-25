# Reporting bugs (Dark Souls Remastered)

Open a report on this fork using the [bug template](https://github.com/kj-318/mariosouls/issues/new?template=bug_report.md). Remastered problems belong here, not on the original PTDE project. Reporting is manual; the mod uploads nothing.

## Include

- MarioSouls build: the commit, or the zip's date.
- Dark Souls Remastered version (Steam), and the resolution and graphics settings if it's a visual problem.
- Area, starting position and a short list of what you did.
- What you expected, what happened, and whether it repeats.
- Controller or keyboard/mouse.
- Whether it also happens in knight mode (L3 + R3 / F6).
- Other mods, especially anything in the game folder.
- `MarioSouls\settings.txt` if you made one.

For collision and attack problems, include the direction you approached from. A short video helps more than a screenshot.

## Log and privacy

Attach the relevant part of `%LOCALAPPDATA%\MarioSoulsProbe\remastered_host.log`. It is replaced on the next launch, so copy it first. Remove personal paths or names before posting. Never attach ROMs, saves, extracted game files or memory dumps. For a security issue, follow [SECURITY.md](../SECURITY.md) instead of posting details.

## Proposing a fix

Open a PR against this fork's `main`. Describe the problem, what changed, and what you actually checked in game. Update [PORT_STATUS](../framework/hosts/ds1remastered/PORT_STATUS.md) and [CHANGELOG.md](../CHANGELOG.md). See [contributing](../CONTRIBUTING.md).
