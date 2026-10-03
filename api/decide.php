<?php
/**
 * GET ?score=N&npc=Name : should a follower react to this fight?
 * Plain text answer for CHIMGore.dll: "allow" or "skip:<reason>|<readable reason>".
 */
require __DIR__ . DIRECTORY_SEPARATOR . 'bootstrap_inc.php';

header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');

$score = max(0, min(1000, (int)($_GET['score'] ?? 0)));
$npc = mb_substr(trim((string)($_GET['npc'] ?? '')), 0, 80);
[$allowed, $reason] = chimGoreDecideForGame($score, $npc);
echo $allowed ? "allow\n" : 'skip:' . $reason . '|' . chimGoreReasonText($reason) . "\n";
