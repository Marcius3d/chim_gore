# CHIM Gore: agent notes

This repository is the source of a CHIM add-on. It has two parts.

| Part | Path | Installs to |
|---|---|---|
| SKSE plugin (C++, CommonLibSSE-NG) | `SkyrimPlugin/` | `Data/SKSE/Plugins/CHIMGore.dll` + `.ini` |
| HerikaServer plugin (PHP) | repository root (`prerequest.php`, `index.php`, `lib/`, `migrations/`, `manifest.json`) | `HerikaServer/ext/chim_gore/` |

## Contracts to preserve

- **Game → CHIM.** `AIAgentFunctions.logMessage(text, "info_gore")` is used for events. `AIAgentFunctions.requestMessageForEligibleActor(text, "instruction", npcName)` is used for the reaction. Check signatures against CHIM `Plugin/Papyrus.cpp` before changing them.
  - For `instruction`, CHIM sends the message unchanged, prefixed with `(Context location: …)`.
  - For other types, CHIM prefixes `PlayerName:` to the message.
- **Game → gore mods.** Only public Papyrus natives are used:
  - `NGDecapitations.IsDecapitated(Actor)`
  - `DismemberingFramework.IsDismemberedNode(Actor, String)`

  NGD forms read from `Next-Gen Decapitations.esp`: keywords 0x800 and 0x801, activator 0x802 (linked refs to the body and head).
- **Marker.** The reaction text starts with `[chim_gore score=N heads=N limbs=N events=N]`. `lib/chim_gore.php` parses it. Keep both sides in sync.
- **Server hook.** Only `prerequest.php` is used. It runs after the NPC profile is loaded, so `$GLOBALS['HERIKA_NAME']` is the follower. Never give files in `lib/` a hook name (HerikaServer loads hook names recursively); `scripts/build_packages.py` enforces this.
- **Database.** One table, `plugins.chim_gore_settings` (key/value). Migrations are append-only once released.

## Versioning

Bump the same version in:

- `manifest.json`
- `dwemer-package.json`
- `SkyrimPlugin/CMakeLists.txt`
- `SkyrimPlugin/vcpkg.json`
- `lib/chim_gore.php`

`python scripts/build_packages.py --check-only` verifies they match. Add a `CHANGELOG.md` entry.

## Validation

- `php -l` on changed PHP files.
- Run the server unit check: `php tests/server_policy_test.php`.
- CI must build the DLL.
- Report in-game testing separately from build results. A successful build does not prove that a follower reacts in Skyrim.
- Never commit API keys, saves, logs or generated audio.
