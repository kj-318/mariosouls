# Contributing

This fork is the **Dark Souls Remastered** port of MarioSouls, built on Ted Cassirer's reusable host/core/adapter framework. The Remastered host lives in `framework/hosts/ds1remastered/`; its current state is in [PORT_STATUS](framework/hosts/ds1remastered/PORT_STATUS.md). The original PTDE host (`framework/hosts/ds1ptde/`) stays in the tree as the reference implementation and is not built or shipped by this fork. Start with [development](docs/DEVELOPMENT.md), [architecture](framework/README.md) and [AGENTS.md](AGENTS.md).

Read the detailed [architecture rules](framework/docs/ARCHITECTURE.md) before changing framework code or contracts. They distinguish intended boundaries from current PTDE debt. These rules apply equally to human contributors and AI-generated changes; compilation alone does not establish that they were followed.

## Make a focused change

Keep Dark Souls integration in the edition host, neutral policies in core/SDK and guest-engine behavior in the character adapter. Prefer an improvement at the correct framework boundary over a Mario-specific branch in host code. Preserve native health, stamina, inventory and rewards.

Explain the owning layer/authority and any changed dependency, data lifetime, identity, capability/failure path, timing/render behavior or ABI in your PR. Hosts must not include adapter internals; core/SDK must not acquire native/graphics dependencies; drawing must not advance gameplay. New editions use their own verified implementation/build/install boundaries. Update the architecture/debt record when a boundary actually changes rather than describing the new design as already verified.

Build the Remastered host with `tools\build_remastered.bat` and package it with `tools\package_remastered.bat` (see [development](docs/DEVELOPMENT.md)). Warnings are errors.

CI compiles; there are no automated gameplay suites. Use the [manual checklist](docs/TESTING.md) when game verification is appropriate, and state which cases remain untested. Never use the owner's level-65 character. Agent-driven controls need a current handoff.

Character/camera assembly and debug geometry/panels now have explicit neutral input builders; PTDE supplies observations and owns native restoration/GPU state. Keep their borrowed-input and output lifetimes intact. Use the current resource/session owners rather than introducing a parallel global instance; do not convert native cleanup into destructor side effects. Preview/cinematic/audio state and include order remain bounded debt, documented in the architecture specification. A port starts from the [presentation-only handoff](framework/docs/REMASTERED_HANDOFF.md).

## Keep the behavior record current

Update [features and tweaks](docs/FEATURES_AND_TWEAKS.md) whenever behavior, tuning, configuration or defaults change. Include values/units, source locations, limitations and verification status. Add meaningful changes to the unreleased section of [CHANGELOG.md](CHANGELOG.md).

Update player instructions for visible/control/installation changes, and SDK/porting guides for contract or native integration changes. README is a player introduction; implementation detail belongs in the focused guides.

## Work through your fork

This Remastered port is [kj-318/mariosouls](https://github.com/kj-318/mariosouls); the original PTDE project is [TedCassirer/mariosouls](https://github.com/TedCassirer/mariosouls). Remastered changes go to this fork: push a focused branch and open a PR against its `main`. Changes to shared framework, SDK or adapter code that would also help PTDE can be offered upstream as a separate PR. Confirm remotes and base branch before pushing.

A PR should describe the concrete problem and resulting behavior, link a reproduction or issue where available, list compilation and manual observations, and identify unverified cases. Include the documentation updates. See the [PR template](.github/PULL_REQUEST_TEMPLATE.md).

## Keep private data private

Commit source, manifests and documentation only. Exclude ROMs, saves, game assets, extracted props/collision, binaries, dependency checkouts, private captures and credentials. Preserve historical tags and local archives. Use [manual bug reports](docs/BUG_REPORTING.md) for ordinary problems and [security guidance](SECURITY.md) for sensitive reports.

Optional formatting uses `requirements-dev.txt` and `python tools/framework.py format`; it is not a CI gate. Contributions to original project code use [GPL-3.0-only](LICENSE); upstream licenses remain separate.
