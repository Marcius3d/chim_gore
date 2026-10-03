# Changelog

## 0.1.0 (unreleased)

- First version.
- Detects decapitations (Next-Gen Decapitations, Dismembering Framework) and severed forearms, legs and tails (Dismembering Framework) caused by the player or followers.
- Measures how far the severed head flew.
- Writes each gory kill to CHIM's event log as `info_gore`.
- After combat, plus a random delay, one follower may react. The server applies chance, cooldown and minimum-score rules, all configurable on the plugin page.
