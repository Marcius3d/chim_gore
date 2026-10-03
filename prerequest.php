<?php
/**
 * CHIM Gore - HerikaServer hook (loaded from main.php before dispatch, after the NPC profile).
 *
 * - "info_gore" events from CHIMGore.dll: appended to CHIM's own death line for that victim
 *   (or stored so that everyone nearby sees them).
 * - "instruction" requests with a [chim_gore ...] marker: rewritten into the configured
 *   follower reaction (the decision was made earlier through api/decide.php).
 * Everything else is left untouched.
 */

$chimGoreType = $gameRequest[0] ?? '';
if ($chimGoreType !== 'instruction' && $chimGoreType !== 'info_gore') {
    return;
}

require_once __DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_gore.php';

if ($chimGoreType === 'info_gore') {
    if (chimGoreHandleGoreEvent($gameRequest)) {
        terminate();
    }
    return;
}

$chimGoreResult = chimGoreHandleRequest($gameRequest);
if ($chimGoreResult === null) {
    return;
}
if (!$chimGoreResult['allow']) {
    terminate();
}
$gameRequest[3] = $chimGoreResult['data'];
