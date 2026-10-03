<?php
/**
 * CHIM Gore - HerikaServer hook (loaded from main.php before dispatch, after the NPC profile).
 *
 * - Remembers when CHIM itself comments on a finished fight (combatend), so CHIM Gore does not
 *   talk over it.
 * - CHIMGore.dll asks one follower to react after combat with an "instruction" request that
 *   carries a [chim_gore ...] marker. Here we apply the chance/cooldown/score policy from the
 *   plugin page and rewrite the request into the configured instruction.
 * Everything else is left untouched.
 */

$chimGoreType = $gameRequest[0] ?? '';
if ($chimGoreType !== 'instruction' && $chimGoreType !== 'combatend' && $chimGoreType !== 'combatendmighty') {
    return;
}

require_once __DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_gore.php';

$chimGoreResult = chimGoreHandleRequest($gameRequest);
if ($chimGoreResult === null) {
    return;
}
if (!$chimGoreResult['allow']) {
    terminate();
}
$gameRequest[3] = $chimGoreResult['data'];
