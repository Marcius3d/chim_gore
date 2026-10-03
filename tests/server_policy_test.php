<?php
/**
 * Standalone check of the server policy (no HerikaServer needed): php tests/server_policy_test.php
 */

final class ChimGoreFakeDb
{
    public array $rows = [];

    public function fetchOne($q, array $p = [])
    {
        if (str_contains($q, 'to_regclass')) {
            return ['t' => 'plugins.chim_gore_settings'];
        }
        if ($p) {
            $this->rows[$p[0]] = (string)$p[1];
            return ['key' => $p[0]];
        }
        return [];
    }

    public function fetchAll($q)
    {
        $out = [];
        foreach ($this->rows as $k => $v) {
            $out[] = ['key' => $k, 'value' => $v];
        }
        return $out;
    }
}

$GLOBALS['db'] = new ChimGoreFakeDb();
$GLOBALS['HERIKA_NAME'] = 'Serana';
require __DIR__ . '/../lib/chim_gore.php';

$failures = 0;
function check(string $name, bool $ok): void
{
    global $failures;
    echo ($ok ? 'ok   ' : 'FAIL ') . $name . PHP_EOL;
    if (!$ok) $failures++;
}

$text = '(Context location: Bleak Falls Barrow)[chim_gore score=9 heads=1 limbs=1 events=2] The fight is over. '
    . 'Briefly react to the most gruesome moment of it, in character. What happened: Serana cut off the bandit\'s head; '
    . 'the severed head landed about 6 meters away.';
$request = ['instruction', '1', '2', $text];

$parsed = chimGoreParseRequest($text);
check('marker parsed', $parsed !== null && $parsed['values']['score'] === 9 && $parsed['values']['heads'] === 1);
check('location prefix kept', $parsed['prefix'] === '(Context location: Bleak Falls Barrow)');
check('details extracted', str_starts_with($parsed['details'], 'Serana cut off'));

check('foreign instruction ignored', chimGoreHandleRequest(['instruction', '1', '2', 'Go to Whiterun']) === null);
check('other request types ignored', chimGoreHandleRequest(['inputtext', '1', '2', $text]) === null);

chimGoreSetValue('chance', '100');
$first = chimGoreHandleRequest($request);
check('allowed when chance 100', $first['allow'] === true);
check('instruction rewritten', str_contains($first['data'], 'Serana briefly reacts') && str_contains($first['data'], '6 meters'));
check('marker removed', !str_contains($first['data'], '[chim_gore'));
check('cooldown blocks second', chimGoreHandleRequest($request)['allow'] === false);

chimGoreSetValue('last_reflect_ts', '0');
chimGoreSetValue('min_score', '10');
check('min score blocks', chimGoreHandleRequest($request)['allow'] === false);

chimGoreSetValue('min_score', '0');
chimGoreSetValue('chance', '0');
check('chance 0 blocks', chimGoreHandleRequest($request)['allow'] === false);

chimGoreSetValue('chance', '100');
chimGoreSetValue('enabled', '0');
check('disabled blocks', chimGoreHandleRequest($request)['allow'] === false);

check('counters updated', (int)chimGoreGetSettings()['stat_requests'] === 5 && (int)chimGoreGetSettings()['stat_spoken'] === 1);
check('unknown key rejected', chimGoreSetValue('evil', 'x') === false);

echo $failures === 0 ? "All checks passed\n" : "{$failures} check(s) failed\n";
exit($failures === 0 ? 0 : 1);
