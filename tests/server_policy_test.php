<?php
/**
 * Standalone check of the server logic (no HerikaServer needed): php tests/server_policy_test.php
 */

final class ChimGoreFakeDb
{
    public array $rows = [];
    public array $log = [];

    public function fetchOne($q, array $p = [])
    {
        if (str_contains($q, 'to_regclass')) {
            return ['t' => $p[0] ?? 'x'];
        }
        if (str_contains($q, 'chim_gore_settings')) {
            $this->rows[$p[0]] = (string)$p[1];
            return ['key' => $p[0]];
        }
        if (str_contains($q, 'INSERT INTO plugins.chim_gore_log')) {
            $this->log[] = ['id' => count($this->log) + 1, 'created_at' => 'now', 'source' => $p[0], 'level' => $p[1], 'message' => $p[2]];
            return ['id' => count($this->log)];
        }
        if (str_contains($q, 'DELETE FROM plugins.chim_gore_log')) {
            if (!$p) $this->log = [];
            return [];
        }
        return [];
    }

    public function fetchAll($q)
    {
        if (str_contains($q, 'chim_gore_log')) {
            $rows = $this->log;
            if (preg_match("/source = '(\w+)'/", $q, $m)) {
                $rows = array_values(array_filter($rows, fn($r) => $r['source'] === $m[1]));
            }
            return array_reverse($rows);
        }
        $out = [];
        foreach ($this->rows as $k => $v) {
            $out[] = ['key' => $k, 'value' => $v];
        }
        return $out;
    }

    public function escape($s) { return addslashes((string)$s); }
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

// Parsing
$parsed = chimGoreParseRequest($text);
check('marker parsed', $parsed !== null && $parsed['values']['score'] === 9 && $parsed['values']['heads'] === 1);
check('location prefix kept', $parsed['prefix'] === '(Context location: Bleak Falls Barrow)');
check('details extracted', str_starts_with($parsed['details'], 'Serana cut off'));
check('foreign instruction ignored', chimGoreHandleRequest(['instruction', '1', '2', 'Go to Whiterun']) === null);
check('other request types ignored', chimGoreHandleRequest(['inputtext', '1', '2', $text]) === null);

// Policy
chimGoreSetValue('chance', '100');
$first = chimGoreHandleRequest($request);
check('allowed when chance 100', $first['allow'] === true);
check('instruction rewritten', str_contains($first['data'], 'Serana briefly reacts') && str_contains($first['data'], '6 meters'));
check('marker removed', !str_contains($first['data'], '[chim_gore'));
check('cooldown blocks second', chimGoreHandleRequest($request)['allow'] === false);

chimGoreSetValue('last_reflect_ts', '0');
check('CHIM combatend is recorded and passes through', chimGoreHandleRequest(['combatend', '1', '2', 'x']) === null);
check('skip right after CHIM commented the fight', chimGoreHandleRequest($request)['allow'] === false);
chimGoreSetValue('skip_after_chim_combat_seconds', '0');
check('overlap check can be disabled', chimGoreHandleRequest($request)['allow'] === true);

chimGoreSetValue('last_reflect_ts', '0');
chimGoreSetValue('min_score', '10');
check('min score blocks', chimGoreHandleRequest($request)['allow'] === false);
chimGoreSetValue('min_score', '0');
chimGoreSetValue('chance', '0');
check('chance 0 blocks', chimGoreHandleRequest($request)['allow'] === false);
chimGoreSetValue('chance', '100');
chimGoreSetValue('reflect_after_combat', '0');
check('reaction switch blocks', chimGoreHandleRequest($request)['allow'] === false);
chimGoreSetValue('reflect_after_combat', '1');
chimGoreSetValue('enabled', '0');
check('disabled blocks', chimGoreHandleRequest($request)['allow'] === false);
chimGoreSetValue('enabled', '1');
check('unknown key rejected', chimGoreSetValue('evil', 'x') === false);

// Config for the game
chimGoreSetValue('debug', '1');
chimGoreSetValue('use_df', '0');
$config = chimGoreConfigText(chimGoreGetSettings());
check('config has plugin marker', str_starts_with($config, "plugin=chim_gore\n"));
check('config carries debug and mod switches', str_contains($config, "debug=1\n") && str_contains($config, "use_df=0\n") && str_contains($config, "use_ngd=1\n"));
check('config never contains the instruction text', !str_contains($config, 'gruesome'));

// Status from the game
$status = json_encode(['plugin' => 'chim_gore', 'version' => '0.2.0', 'ngd_loaded' => true, 'df_loaded' => false,
    'log_lines' => ['[2026-10-03 18:00:00.000] [info] Ready', '[2026-10-03 18:00:01.000] [warning] Papyrus call failed']]);
check('status accepted', chimGoreHandleStatus($status) === null);
check('status rejects other plugins', chimGoreHandleStatus('{"plugin":"x"}') === 'not_chim_gore');
check('status rejects bad json', chimGoreHandleStatus('{oops') === 'invalid_json');
$saved = chimGoreGameStatus(chimGoreGetSettings());
check('status stored without log lines', ($saved['version'] ?? '') === '0.2.0' && !isset($saved['log_lines']));
$gameLog = chimGoreLogFetch(50, 'game');
check('game log lines stored with levels', count($gameLog) === 2 && $gameLog[1]['level'] === 'warn');

// Diagnostic file
$report = chimGoreDiagnosticReport();
check('report has versions, status and log', str_contains($report, 'Server plugin version: ' . CHIM_GORE_VERSION)
    && str_contains($report, 'ngd_loaded:') && str_contains($report, 'Papyrus call failed') && str_contains($report, 'chim-gore.log'));
check('log can be cleared', chimGoreLogClear() && chimGoreLogFetch(10) === []);

echo $failures === 0 ? "All checks passed\n" : "{$failures} check(s) failed\n";
exit($failures === 0 ? 0 : 1);
