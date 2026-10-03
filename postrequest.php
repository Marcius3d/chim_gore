<?php
/**
 * CHIM Gore - HerikaServer hook (loaded at the end of main.php).
 * Remembers when CHIM really finished its own combat-end comment, so CHIM Gore does not talk over it.
 */

$chimGoreType = $gameRequest[0] ?? '';
if ($chimGoreType !== 'combatend' && $chimGoreType !== 'combatendmighty') {
    return;
}

require_once __DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_gore.php';
chimGoreNoteChimCombatComment($chimGoreType);
