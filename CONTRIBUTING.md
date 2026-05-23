# Contributing

Thanks for taking a look! PocketWebGames is a small hobby project, so the
process is lightweight.

## Quick start

- Read [README.md](README.md) for what it is and how to run it.
- Read [architecture.md](architecture.md) for how it works.
- Read [Agents.md](Agents.md) for conventions (including a checklist for
  **adding a new game**).

## Building & testing

- Firmware: `pio run` (and `pio run -t upload`/`-t uploadfs` to flash a Core2).
- Browser games can be played and tested on a PC with the mock:
  `uv run mock_server.py <slug>` — see the README for the slugs.

## Pull requests

- Keep changes focused — one fix or one feature per PR is easier to review.
- Make sure `pio run` and `pio run -t buildfs` still succeed.
- A short description of *what* and *why* in the PR body is appreciated.

That's it. If you're unsure about anything, just open an issue or draft PR
and ask — happy to help.
