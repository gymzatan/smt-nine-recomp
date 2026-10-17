# Contributing

Use the Japanese retail ISO identified in `config/retail.json` and follow `docs/BUILDING.md`. Keep game content, generated code, binaries, fonts, saves and logs out of commits. Changes to upstream runtime code must retain its notices and explain the actual invoked path and observed input/output conflict.

Reuse existing identities, writers and runtime consumers. Do not add silent fallback behavior or duplicate game/resource engines. Keep changes focused and document the resulting behavior. Run the relevant tests, rebuild affected consumers and reproduce the changed user flow. For translation edits, preserve source-reference identities, variables, execution boundaries and explicit page layouts.

CI runs checks that do not require a game image. Full disc-to-game validation is local because the repository does not supply the game. Include its scope and tool/input identities when reporting a build or gameplay result.
