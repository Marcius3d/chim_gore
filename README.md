# CHIM Gore

**Your followers finally notice what their axe just did.**

CHIM Gore is an add-on for [CHIM](https://www.nexusmods.com/skyrimspecialedition/mods/126330) (AI NPCs for Skyrim). It connects CHIM to [Next-Gen Decapitations](https://www.nexusmods.com/skyrimspecialedition/mods/135254) and [Dismembering Framework](https://www.nexusmods.com/skyrimspecialedition/mods/126203).

Without it, CHIM only records *"Serana killed Bandit"*. With it, CHIM learns *"Serana cut off the bandit's head with her Dawnguard axe; the severed head landed about 6 meters away"*. Once the fight is over, one follower may bring it up:

> "Did you see that? His head went halfway across the room. I may have overdone it."

> **Status: 0.2.2, beta.** The packages build in CI and the server side is tested against PostgreSQL. The game-side behaviour has not been confirmed in game yet. Please report results (see [Feedback](#feedback)).


## ⬇ Download

**[Download the latest CHIM Gore for MO2 / Vortex (CHIM-Gore.zip)](https://github.com/Marcius3d/chim_gore/releases/latest/download/CHIM-Gore.zip)**

This link always points to the newest release. Older versions are on the [Releases](https://github.com/Marcius3d/chim_gore/releases) page; use the `CHIM-Gore-<version>.zip` file, not *Source code*.

---

## What it does

| When | What happens |
|---|---|
| During a fight | Each decapitation or severed limb caused by the player or a follower is written to CHIM's memory as a short event. Nobody speaks, so combat chatter is not flooded. Nearby NPCs see these events in their context. |
| After the fight | When combat has been over for a random 8–20 s, **one** follower may react to the most gruesome moment. The follower who did the cutting goes first, otherwise the nearest follower. If combat restarts in the meantime, the timer resets. |
| Not every fight | The server decides whether to react using a chance (default 35 %), a cooldown (default 10 real minutes) and a minimum "gore score". You can change all three in the CHIM web UI. |
| CHIM's own comment | When CHIM itself comments on the finished fight, the fight's decapitations and severed limbs are added to that comment, and CHIM Gore's separate reaction is skipped so nobody talks twice. |

Gore score: severed head = 6 (+1 for every 2 m the head flew, up to +5), each limb = 2, finishing move = +1. Only the best moment of a fight counts.

Detected injuries:

- Head (Next-Gen Decapitations, or Dismembering Framework's neck node)
- Every limb node of the installed Dismembering Framework packs (humanoid forearms and legs, tails, creature packs). The node list is read from the packs' JSON files at startup.
- How far the severed head flew (Next-Gen Decapitations; sampled several times while the head rolls)

## In-game messages

- `CHIM-gore: activated (Next-Gen Decapitations + Dismembering Framework)` is always shown a few seconds after a save loads. If no supported mod is active, the message says so.
- With **debug mode** on (CHIM web page or INI):
  - `CHIM-gore: request sent (…)`: the gore mods are being asked about a fresh corpse.
  - `CHIM-gore: request failed (reason)`: a gore mod or CHIM could not be reached, or Papyrus did not answer within 5 s.
  - `CHIM-gore: request successful`: the event was accepted into CHIM's memory.
  - Reaction requests after combat are reported the same way.

Messages appear wherever your HUD shows notifications: top left in vanilla, top right with SkyHUD (for example in Nolvus), next to CHIM's and NFF's own messages.

## Requirements

| Required | Version tested against |
|---|---|
| Skyrim SE/AE + [SKSE64](https://skse.silverlock.org/) | any runtime supported by Address Library |
| [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) | — |
| [CHIM](https://www.nexusmods.com/skyrimspecialedition/mods/126330) + DwemerDistro server | CHIM / HerikaServer 3.4.2 |
| At least one of: Next-Gen Decapitations, Dismembering Framework | NGD 1.2.0, DF 1.0.6 |

Both gore mods are optional, but with neither installed this add-on does nothing. Neither mod is modified, and no ESP/ESL is added, so CHIM Gore takes no plugin slot.

## Installation

1. Download **[CHIM-Gore.zip](https://github.com/Marcius3d/chim_gore/releases/latest/download/CHIM-Gore.zip)** (latest release). Do not use the *Source code* archives.
2. Install it with Mod Organizer 2 or Vortex like any other mod. Load order does not matter.
3. Start the game with the CHIM server running. CHIM finds the embedded server package (`CHIM/server-plugins/chim_gore/`) and installs it on the server automatically.
4. A few seconds after the save loads you should see `CHIM-gore: activated (…)`.
5. Optional: open the CHIM web UI → **Server Plugins** → *CHIM Gore* → **Plugin Page** to change settings.

**Updating:** install the new zip over the old one. The server part updates on the next game start, or via the **Update** button in Server Plugins.

**Uninstalling:** remove the mod. To also remove the server part, delete it in Server Plugins.

## Settings

Everything is configured on the CHIM web page: **Server Plugins** → *CHIM Gore* → **Plugin Page**. The game picks up changes within 30 seconds.

| Section | Options |
|---|---|
| Status | game plugin connected, detected mods and versions, counters, last error |
| General | enable, **debug mode** |
| Supported mods | use Next-Gen Decapitations, use Dismembering Framework (both are detected automatically; untick to ignore one) |
| In game | save each kill to CHIM memory, follower reaction, count player kills, delays, follower range |
| After-combat reaction | chance, cooldown, minimum gore score, skip when CHIM already commented the fight, instruction text |
| Diagnostics | **Create diagnostic file** (one .txt to attach to a bug report), server and game log viewer, clear log |

`Data/SKSE/Plugins/CHIMGore.ini` holds the same game options. They are used only while the server plugin cannot be reached. The `[Server]` section can override the server address, which is normally read from `AIAgent.ini` or discovered the same way CHIM does it.

### Logs

- Game: `Documents\My Games\Skyrim Special Edition\SKSE\chim-gore.log`, next to the other SKSE logs. Recent lines are also sent to the server and shown on the plugin page.
- Server: the Diagnostics section of the plugin page, which keeps the last 2000 entries.

## How it works

```text
 Skyrim                                         DwemerDistro / HerikaServer
 ───────────────────────────────────────────    ─────────────────────────────────────────
 TESDeathEvent (killer = player or follower)
   └─ wait 2.5 s
      ├─ NGDecapitations.IsDecapitated(victim)  ┐ Papyrus API of the gore mods
      ├─ DismemberingFramework.IsDismemberedNode┘
      └─ find NGD's head reference → distance
   gory? ── AIAgentFunctions.logMessage ──────▶ eventlog "info_gore" → NPC context
   collect for this fight
 player leaves combat → wait 8–20 s
   └─ pick follower ── requestMessageForEligibleActor("instruction")
                                              ▶ ext/chim_gore/prerequest.php
                                                 chance / cooldown / score policy
                                                 rewrite to configured instruction
                                              ◀ CHIM speaks with the follower's voice
```

- **`CHIMGore.dll`** is an SKSE plugin built with CommonLibSSE-NG. It talks to the gore mods only through their public Papyrus functions and to CHIM only through `AIAgentFunctions`, the same API CHIM's own scripts use. Every 30 s it also fetches its settings from `ext/chim_gore/api/config.php` and posts its status and new log lines to `api/status.php`. If the server is unreachable it falls back to the INI.
- **`ext/chim_gore/`** is a HerikaServer plugin with three hooks (`prerequest.php`, `prompts.php`, `postrequest.php`), a settings page, three small API endpoints (`config`, `status`, `decide`) and two tables, `plugins.chim_gore_settings` and `plugins.chim_gore_log`. It rewrites only requests that carry the `[chim_gore …]` marker. It also adds the fight's gore to CHIM's own `combatend` comment and remembers when CHIM made that comment, so CHIM Gore does not talk over it. Every other request passes through untouched.
- If the server part is missing, the follower still reacts, but without the chance/cooldown policy.
- CHIM's own on/off switch is respected: when CHIM interaction is off, no reaction is generated.

## Compatibility notes

- Followers are addressed by name when asking CHIM for a reaction. If two followers share the same name, CHIM may pick the other one.
- Works alongside CHIM's normal *combat end* comments. If you find them too chatty together, lower CHIM's combat-comment chance or this add-on's chance.
- Limb detection covers the node names from Dismembering Framework's official humanoid pack. Creature packs with other node names will still report heads, but not every limb.
- Event text is generated in English. CHIM's LLM answers in your configured language.

## Building from source

The GitHub Actions workflow in `.github/workflows/build.yml` builds everything on each push. Publishing a GitHub release (any tag name) builds the mod and attaches `CHIM-Gore-<version>.zip`, `CHIM-Gore.zip`, the `.dwpkg` and `chim_gore.tar.gz` to it. The release notes get download links automatically. Tags starting with `v` also do this.

Locally on Windows (Visual Studio 2022, CMake, Ninja, vcpkg with `VCPKG_ROOT` set):

```powershell
cd SkyrimPlugin
cmake --preset release
cmake --build --preset release
cd ..
python scripts/build_packages.py --dll SkyrimPlugin/build/release/CHIMGore.dll
```

The server side alone (no compiler needed): `python scripts/build_packages.py`.

To release, raise the version in `manifest.json`, `dwemer-package.json`, `SkyrimPlugin/CMakeLists.txt`, `SkyrimPlugin/vcpkg.json` and `lib/chim_gore.php`. The build checks that they match.

## Troubleshooting

| Symptom | Check |
|---|---|
| No `CHIM-gore: activated` after loading | The mod is not enabled, or SKSE/Address Library is missing. Check `chim-gore.log`. |
| `activated, but no supported gore mod is active` | Neither gore mod is loaded, or both are unticked on the plugin page. |
| `request failed (… not reachable)` | That gore mod's Papyrus script is missing or outdated. |
| `request failed (CHIM not reachable)` | CHIM is not installed or not loaded. |
| Events saved but nobody talks | Plugin page → Diagnostics. The server log says why a reaction was skipped (chance, cooldown, score, CHIM already commented). |
| Plugin page says *no report yet* | The game cannot reach the server plugin. Set `[Server]` in `CHIMGore.ini` if CHIM uses a non-default address. |

## Feedback

Please open an [issue](https://github.com/Marcius3d/chim_gore/issues) or post in the CHIM Discord. Attach the file from **Plugin Page → Diagnostics → Create diagnostic file** and, if possible, `chim-gore.log`.

## Credits

- **Seb263**: Next-Gen Decapitations and Dismembering Framework, and their Papyrus APIs.
- **Dwemer Dynamics**: CHIM, HerikaServer and the plugin package format. CHIM-Custom was the reference for the add-on layout.
- **CommonLibSSE-NG** contributors.

## License

MIT. See [LICENSE](LICENSE).

---

**⬇ [Download the latest CHIM-Gore.zip for MO2 / Vortex](https://github.com/Marcius3d/chim_gore/releases/latest/download/CHIM-Gore.zip)** · [All releases](https://github.com/Marcius3d/chim_gore/releases)
