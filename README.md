# CHIM Gore

**Your followers finally notice what their axe just did.**

CHIM Gore is an add-on for [CHIM](https://www.nexusmods.com/skyrimspecialedition/mods/126330) (AI NPCs for Skyrim). It connects CHIM to [Next-Gen Decapitations](https://www.nexusmods.com/skyrimspecialedition/mods/135254) and [Dismembering Framework](https://www.nexusmods.com/skyrimspecialedition/mods/126203).

Without it, CHIM only records *"Serana killed Bandit"*. With it, CHIM learns *"Serana cut off the bandit's head with her Dawnguard axe; the severed head landed about 6 meters away"*. Once the fight is over, one follower may bring it up:

> "Did you see that? His head went halfway across the room. I may have overdone it."

> **Status: 0.1.0, beta.** The packages build and the server side is unit-tested. The game-side behaviour has not been confirmed in game yet. Please report results (see [Feedback](#feedback)).

---

## What it does

| When | What happens |
|---|---|
| During a fight | Each decapitation or severed limb caused by the player or a follower is written to CHIM's memory as a short event. Nobody speaks, so combat chatter is not flooded. Nearby NPCs see these events in their context. |
| After the fight | When combat has been over for a random 8–20 s, **one** follower may react to the most gruesome moment. The follower who did the cutting goes first, otherwise the nearest follower. If combat restarts in the meantime, the timer resets. |
| Not every fight | The server decides whether to react using a chance (default 35 %), a cooldown (default 10 real minutes) and a minimum "gore score". You can change all three in the CHIM web UI. |

Gore score: severed head = 6 (+1 for every 2 m the head flew, up to +5), each limb = 2, finishing move = +1. Only the best moment of a fight counts.

Detected injuries:

- Head (Next-Gen Decapitations, or Dismembering Framework's neck node)
- Left and right forearm, left and right leg, and tail (Dismembering Framework official humanoid pack)
- How far the severed head flew (Next-Gen Decapitations)

## Requirements

| Required | Version tested against |
|---|---|
| Skyrim SE/AE + [SKSE64](https://skse.silverlock.org/) | any runtime supported by Address Library |
| [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) | — |
| [CHIM](https://www.nexusmods.com/skyrimspecialedition/mods/126330) + DwemerDistro server | CHIM / HerikaServer 3.4.2 |
| At least one of: Next-Gen Decapitations, Dismembering Framework | NGD 1.2.0, DF 1.0.6 |

Both gore mods are optional, but with neither installed this add-on does nothing. Neither mod is modified, and no ESP/ESL is added, so CHIM Gore takes no plugin slot.

## Installation

1. Download `CHIM-Gore-<version>.zip` from [Releases](https://github.com/Marcius3d/chim_gore/releases).
2. Install it with Mod Organizer 2 or Vortex like any other mod. Load order does not matter.
3. Start the game with the CHIM server running. CHIM finds the embedded server package (`CHIM/server-plugins/chim_gore/`) and installs it on the server automatically.
4. Optional: open the CHIM web UI → **Server Plugins** → *CHIM Gore* → **Plugin Page** to change chance, cooldown and wording.

**Updating:** install the new zip over the old one. The server part updates on the next game start, or via the **Update** button in Server Plugins.

**Uninstalling:** remove the mod. To also remove the server part, delete it in Server Plugins.

## Settings

### Game side: `Data/SKSE/Plugins/CHIMGore.ini`

| Key | Default | Meaning |
|---|---|---|
| `bEnabled` | 1 | Master switch |
| `bLogEachEvent` | 1 | Write each gory kill into CHIM's memory |
| `bReflectAfterCombat` | 1 | Ask a follower to react after combat |
| `bIncludePlayerKills` | 1 | Also count the player's own decapitations |
| `bDebugNotifications` | 0 | On-screen notifications for testing |
| `fCheckDelaySeconds` | 2.5 | Wait after a death before checking (the head is still flying) |
| `fReflectDelayMinSeconds` / `Max` | 8 / 20 | Random delay after combat |
| `fFollowerRangeUnits` | 3000 | How close a follower must be (70 units ≈ 1 m) |
| `iMaxSummaryLines` | 3 | How many gory moments are described |

### Server side: CHIM web UI → Server Plugins → CHIM Gore

- Enable or disable after-combat reactions
- Chance (%), cooldown (real minutes), minimum gore score
- The instruction text sent to the follower (`{NPC}`, `{DETAILS}` placeholders)
- Counters, last reaction time, cooldown reset

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

- **`CHIMGore.dll`** is an SKSE plugin built with CommonLibSSE-NG. It talks to the gore mods only through their public Papyrus functions and to CHIM only through `AIAgentFunctions`, the same API CHIM's own scripts use. It has no HTTP code and no settings for the server address.
- **`ext/chim_gore/`** is a HerikaServer plugin with one hook (`prerequest.php`), a settings page and one table, `plugins.chim_gore_settings`. It changes only requests that carry the `[chim_gore …]` marker; every other request passes through untouched.
- If the server part is missing, the follower still reacts, but without the chance/cooldown policy.
- CHIM's own on/off switch is respected: when CHIM interaction is off, no reaction is generated.

## Compatibility notes

- Works alongside CHIM's normal *combat end* comments. If you find them too chatty together, lower CHIM's combat-comment chance or this add-on's chance.
- Limb detection covers the node names from Dismembering Framework's official humanoid pack. Creature packs with other node names will still report heads, but not every limb.
- Event text is generated in English. CHIM's LLM answers in your configured language.

## Building from source

The GitHub Actions workflow in `.github/workflows/build.yml` builds everything on each push. Tagging `vX.Y.Z` publishes a release.

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
| Nothing happens | `Documents/My Games/Skyrim Special Edition/SKSE/CHIMGore.log` should list `Integrations: Next-Gen Decapitations=true …` and `Gore event (score …)` lines. |
| Events logged but nobody talks | Server Plugins → CHIM Gore → Status. Is the chance or cooldown blocking? Is a follower within range? Is CHIM interaction on? Is the follower registered as a CHIM agent? |
| "Could not reach CHIM" in the log | CHIM is not installed or not loaded. |

## Feedback

Please open an [issue](https://github.com/Marcius3d/chim_gore/issues), or post in the CHIM Discord, with:

- your `CHIMGore.log`
- your CHIM version
- which gore mods you use

## Credits

- **Seb263**: Next-Gen Decapitations and Dismembering Framework, and their Papyrus APIs.
- **Dwemer Dynamics**: CHIM, HerikaServer and the plugin package format. CHIM-Custom was the reference for the add-on layout.
- **CommonLibSSE-NG** contributors.

## License

MIT. See [LICENSE](LICENSE).
