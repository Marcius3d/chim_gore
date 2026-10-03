# Changelog

## 0.3.1

- Fix: the server could take the `(Context location: …)` part of CHIM's death line for the killer's name.
- Fix: the killer seen by the game (the weapon hit that severed the limb) is no longer replaced with CHIM's guess. CHIM's name is used only when the game did not see the blow.
- Wording: "… with Steel Sword in a power attack" instead of "with Steel Sword with a power attack".

## 0.3.0

- Fix: kills by followers were credited to the player ("Martin severed …"). Skyrim's death event often names the player. CHIM Gore now trusts the last weapon hit, as CHIM does. As a safety net, the server also corrects the name from CHIM's own death line.
- Fix: gore events were visible only to the player, because CHIM limited them to the names in the sentence. They now carry the people of the death event, so followers see them in their context.
- New: the gore is appended to CHIM's own death line for that victim (`… has defeated Bandit Vanguard using weapon Iron Sword — Sapphire severed the Bandit Vanguard's left forearm.`), visible in Prisma's recent context and in the event log. Option on the plugin page; on by default.
- New: power attacks are mentioned. The weapon is the one that struck the blow.
- New default instruction: a follower who did the cutting talks about their own blow in first person and may address a nearby companion; CHIM's rechat can continue the conversation. A customised instruction is kept.

## 0.2.2

- Fix: the follower reaction was almost always skipped as "CHIM already commented on this fight". The CHIM combat comment is now recorded only when CHIM really finishes it (`postrequest.php`), not when the request arrives.
- Fix: the killer's name was empty for some NPCs ("cut off the bandit's head"). Names now fall back to the reference name and the base NPC name.
- New: when CHIM itself comments on a finished fight, the fight's decapitations and severed limbs are added to that comment, so followers talk about them even when CHIM Gore's own reaction is skipped. Option on the plugin page; on by default.
- New: the game asks the server (`api/decide.php`) before requesting a reaction. Debug mode now shows the real outcome: `reaction skipped (cooldown / chance roll / not gory enough / CHIM already commented on this fight)` or `reaction request successful (name)`.

## 0.2.1

- Fix: Next-Gen Decapitations and Dismembering Framework were reported as "not installed" when their plugins are ESL-flagged (light), as in Nolvus. Light plugins are now checked too, and a loaded SKSE DLL of either mod also counts as installed.
- The log now records how each mod was detected (plugin and DLL).

## 0.2.0

- In-game messages. `CHIM-gore: activated (…)` is always shown after loading a save. In debug mode, `request sent / request failed (reason) / request successful` is shown for every check and every reaction request.
- The plugin page now controls the game plugin: enable, debug mode, which gore mods to use, delays, range. The DLL fetches these settings from the server every 30 s; `CHIMGore.ini` is only a fallback.
- Status panel showing whether the game plugin is connected, the detected mods with versions, counters and the last error.
- Diagnostics: server and game log viewer, plus a **Create diagnostic file** button for bug reports. The game log is now `chim-gore.log`.
- Limb nodes are read from the installed Dismembering Framework packs, so creature packs work too.
- Head distance is sampled several times while the head is still rolling.
- No comment when CHIM itself has just commented on the fight (configurable).
- A Papyrus watchdog reports a failure instead of waiting forever.

## 0.1.0

- First version.
- Detects decapitations (Next-Gen Decapitations, Dismembering Framework) and severed forearms, legs and tails (Dismembering Framework) caused by the player or followers.
- Measures how far the severed head flew.
- Writes each gory kill to CHIM's event log as `info_gore`.
- After combat, plus a random delay, one follower may react. The server applies chance, cooldown and minimum-score rules, all configurable on the plugin page.
