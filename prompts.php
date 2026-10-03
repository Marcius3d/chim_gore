<?php
/**
 * CHIM Gore - HerikaServer hook (loaded from prompts/prompts.php).
 * When CHIM itself comments on a finished fight, add the fight's decapitations/severed limbs.
 */

$chimGoreType = $gameRequest[0] ?? '';
if ($chimGoreType !== 'combatend' && $chimGoreType !== 'combatendmighty') {
    return;
}

require_once __DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_gore.php';
chimGoreEnrichCombatPrompt($chimGoreType);
