# Changelog

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
