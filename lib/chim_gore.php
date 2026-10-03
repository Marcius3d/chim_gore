<?php
/**
 * CHIM Gore - shared server helpers.
 *
 * Note: HerikaServer loads hook files (preprocessing.php, context.php, ...) recursively from
 * every folder under ext/. Do not give files in this folder or in api/ a hook name.
 */

if (defined('CHIM_GORE_VERSION')) {
    return;
}

define('CHIM_GORE_VERSION', '0.2.0');
define('CHIM_GORE_TABLE', 'plugins.chim_gore_settings');
define('CHIM_GORE_LOG_TABLE', 'plugins.chim_gore_log');
define('CHIM_GORE_LOG_KEEP', 2000);

/** Marker added by CHIMGore.dll to its reaction request. */
define('CHIM_GORE_MARKER_REGEX', '/\[chim_gore\s+([^\]]*)\]\s*/i');

/** Settings that CHIMGore.dll reads from api/config.php (key => type). */
function chimGoreGameKeys(): array
{
    return [
        'enabled' => 'bool',
        'debug' => 'bool',
        'use_ngd' => 'bool',
        'use_df' => 'bool',
        'log_each_event' => 'bool',
        'reflect_after_combat' => 'bool',
        'include_player_kills' => 'bool',
        'check_delay_seconds' => 'float',
        'reflect_delay_min_seconds' => 'float',
        'reflect_delay_max_seconds' => 'float',
        'follower_range_units' => 'float',
        'max_summary_lines' => 'int',
    ];
}

function chimGoreDefaults(): array
{
    return [
        // Game behaviour (sent to the DLL)
        'enabled' => '1',
        'debug' => '0',
        'use_ngd' => '1',
        'use_df' => '1',
        'log_each_event' => '1',
        'reflect_after_combat' => '1',
        'include_player_kills' => '1',
        'check_delay_seconds' => '2.5',
        'reflect_delay_min_seconds' => '8',
        'reflect_delay_max_seconds' => '20',
        'follower_range_units' => '3000',
        'max_summary_lines' => '3',
        // Reaction policy (server only)
        'chance' => '35',              // % chance that a qualifying fight gets a comment
        'cooldown_minutes' => '10',    // real-time minutes between comments
        'min_score' => '4',            // 6 = any decapitation, 2 = one limb
        'skip_after_chim_combat_seconds' => '45', // skip if CHIM itself just commented the fight
        'instruction' => '{NPC} briefly reacts to the most gruesome moment of the fight that just ended, '
            . 'in their own voice and personality: shocked, amused, disgusted or proud, whatever fits them. '
            . 'One or two short sentences. Only mention injuries listed here. What happened: {DETAILS}',
        // State and counters
        'last_reflect_ts' => '0',
        'last_chim_combat_ts' => '0',
        'stat_requests' => '0',
        'stat_spoken' => '0',
        'game_status' => '',
        'game_status_ts' => '0',
    ];
}

function chimGoreDb()
{
    return $GLOBALS['db'] ?? null;
}

function chimGoreTableExists(string $table): bool
{
    static $cache = [];
    if (array_key_exists($table, $cache)) {
        return $cache[$table];
    }
    $db = chimGoreDb();
    if (!$db) {
        return $cache[$table] = false;
    }
    try {
        $row = $db->fetchOne('SELECT to_regclass($1) AS t', [$table]);
        return $cache[$table] = !empty($row['t']);
    } catch (Throwable $e) {
        return $cache[$table] = false;
    }
}

function chimGoreDbReady(): bool
{
    return chimGoreTableExists(CHIM_GORE_TABLE);
}

function chimGoreGetSettings(): array
{
    $settings = chimGoreDefaults();
    if (!chimGoreDbReady()) {
        return $settings;
    }
    try {
        $rows = chimGoreDb()->fetchAll('SELECT key, value FROM ' . CHIM_GORE_TABLE);
        foreach ((array)$rows as $row) {
            if (isset($row['key']) && array_key_exists($row['key'], $settings)) {
                $settings[$row['key']] = (string)$row['value'];
            }
        }
    } catch (Throwable $e) {
        // Defaults keep the plugin usable while the table is unavailable.
    }
    return $settings;
}

function chimGoreSetValue(string $key, string $value): bool
{
    if (!chimGoreDbReady() || !array_key_exists($key, chimGoreDefaults())) {
        return false;
    }
    try {
        $row = chimGoreDb()->fetchOne(
            'INSERT INTO ' . CHIM_GORE_TABLE . ' (key, value, updated_at) VALUES ($1, $2, CURRENT_TIMESTAMP) '
            . 'ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = CURRENT_TIMESTAMP RETURNING key',
            [$key, $value]
        );
        return !empty($row);
    } catch (Throwable $e) {
        return false;
    }
}

function chimGoreIncrement(string $key): void
{
    $settings = chimGoreGetSettings();
    chimGoreSetValue($key, (string)((int)($settings[$key] ?? 0) + 1));
}

// ---------------------------------------------------------------------------------------
// Diagnostics log (plugins.chim_gore_log). source: server | game
// ---------------------------------------------------------------------------------------

function chimGoreLog(string $level, string $message, string $source = 'server'): void
{
    $line = '[chim_gore] ' . $message;
    if (class_exists('Logger') && method_exists('Logger', $level)) {
        Logger::$level($line);
    }
    chimGoreLogStore($source, $level, $message);
}

function chimGoreLogStore(string $source, string $level, string $message): void
{
    if (!chimGoreTableExists(CHIM_GORE_LOG_TABLE)) {
        return;
    }
    try {
        chimGoreDb()->fetchOne(
            'INSERT INTO ' . CHIM_GORE_LOG_TABLE . ' (source, level, message) VALUES ($1, $2, $3) RETURNING id',
            [substr($source, 0, 16), substr($level, 0, 16), mb_substr($message, 0, 1000)]
        );
        if (random_int(1, 50) === 1) {
            chimGoreLogTrim();
        }
    } catch (Throwable $e) {
        // Logging must never break a game request.
    }
}

function chimGoreLogTrim(): void
{
    try {
        chimGoreDb()->fetchOne(
            'DELETE FROM ' . CHIM_GORE_LOG_TABLE . ' WHERE id <= (SELECT COALESCE(MAX(id), 0) - $1 FROM '
            . CHIM_GORE_LOG_TABLE . ') RETURNING 1',
            [CHIM_GORE_LOG_KEEP]
        );
    } catch (Throwable $e) {
    }
}

function chimGoreLogFetch(int $limit = 200, ?string $source = null): array
{
    if (!chimGoreTableExists(CHIM_GORE_LOG_TABLE)) {
        return [];
    }
    $limit = max(1, min(CHIM_GORE_LOG_KEEP, $limit));
    try {
        $where = $source !== null ? " WHERE source = '" . chimGoreDb()->escape($source) . "'" : '';
        $rows = chimGoreDb()->fetchAll(
            'SELECT id, created_at, source, level, message FROM ' . CHIM_GORE_LOG_TABLE . $where . ' ORDER BY id DESC LIMIT ' . $limit
        );
        return array_reverse((array)$rows);
    } catch (Throwable $e) {
        return [];
    }
}

function chimGoreLogClear(): bool
{
    if (!chimGoreTableExists(CHIM_GORE_LOG_TABLE)) {
        return false;
    }
    try {
        chimGoreDb()->fetchOne('DELETE FROM ' . CHIM_GORE_LOG_TABLE . ' RETURNING 1');
        return true;
    } catch (Throwable $e) {
        return false;
    }
}

// ---------------------------------------------------------------------------------------
// Game <-> server
// ---------------------------------------------------------------------------------------

/** Plain key=value text for CHIMGore.dll (no JSON parser needed in the game plugin). */
function chimGoreConfigText(array $settings): string
{
    $lines = ['plugin=chim_gore', 'version=' . CHIM_GORE_VERSION];
    foreach (chimGoreGameKeys() as $key => $type) {
        $value = (string)($settings[$key] ?? '');
        if ($type === 'bool') {
            $value = $value === '1' ? '1' : '0';
        } elseif ($type === 'int') {
            $value = (string)(int)$value;
        } else {
            $value = (string)(float)$value;
        }
        $lines[] = "{$key}={$value}";
    }
    return implode("\n", $lines) . "\n";
}

/** Stores a status report from CHIMGore.dll. Returns an error string or null. */
function chimGoreHandleStatus(string $body): ?string
{
    if (strlen($body) > 262144) {
        return 'too_large';
    }
    try {
        $data = json_decode($body, true, 16, JSON_THROW_ON_ERROR);
    } catch (Throwable $e) {
        return 'invalid_json';
    }
    if (!is_array($data) || ($data['plugin'] ?? '') !== 'chim_gore') {
        return 'not_chim_gore';
    }
    $lines = is_array($data['log_lines'] ?? null) ? array_slice($data['log_lines'], -300) : [];
    unset($data['log_lines']);
    $status = [];
    foreach ($data as $key => $value) {
        if (is_string($key) && (is_scalar($value) || $value === null)) {
            $status[substr($key, 0, 64)] = is_string($value) ? mb_substr($value, 0, 300) : $value;
        }
    }
    chimGoreSetValue('game_status', json_encode($status, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE));
    chimGoreSetValue('game_status_ts', (string)time());
    foreach ($lines as $line) {
        if (!is_string($line) || trim($line) === '') {
            continue;
        }
        $level = 'info';
        if (preg_match('/\]\s*\[(error|warning|warn|info|debug|critical)\]/i', $line, $m)) {
            $level = strtolower($m[1]) === 'warning' ? 'warn' : strtolower($m[1]);
        }
        chimGoreLogStore('game', $level, $line);
    }
    return null;
}

function chimGoreGameStatus(array $settings): array
{
    $status = json_decode((string)$settings['game_status'], true);
    return is_array($status) ? $status : [];
}

// ---------------------------------------------------------------------------------------
// Reaction policy
// ---------------------------------------------------------------------------------------

/**
 * Splits a reaction request into marker values and the readable details.
 * Returns null when the text is not from CHIMGore.dll.
 */
function chimGoreParseRequest(string $data): ?array
{
    if (!preg_match(CHIM_GORE_MARKER_REGEX, $data, $match, PREG_OFFSET_CAPTURE)) {
        return null;
    }
    $values = ['score' => 0, 'heads' => 0, 'limbs' => 0, 'events' => 0];
    if (preg_match_all('/([a-z]+)=(\d+)/i', $match[1][0], $pairs, PREG_SET_ORDER)) {
        foreach ($pairs as $pair) {
            $values[strtolower($pair[1])] = (int)$pair[2];
        }
    }
    $prefix = substr($data, 0, $match[0][1]);  // e.g. "(Context location: ...)" added by CHIM
    $rest = substr($data, $match[0][1] + strlen($match[0][0]));
    $details = $rest;
    $pos = stripos($rest, 'What happened:');
    if ($pos !== false) {
        $details = trim(substr($rest, $pos + strlen('What happened:')));
    }
    return ['values' => $values, 'prefix' => $prefix, 'details' => trim($details)];
}

/**
 * Applies enable/score/CHIM-overlap/cooldown/chance policy. Returns [allowed, reason].
 */
function chimGoreDecide(array $settings, array $values, ?int $now = null): array
{
    $now = $now ?? time();
    if ((string)$settings['enabled'] !== '1' || (string)$settings['reflect_after_combat'] !== '1') {
        return [false, 'disabled'];
    }
    if ((int)$values['score'] < (int)$settings['min_score']) {
        return [false, 'score_below_minimum'];
    }
    $overlap = max(0, (int)$settings['skip_after_chim_combat_seconds']);
    if ($overlap > 0 && $now - (int)$settings['last_chim_combat_ts'] < $overlap) {
        return [false, 'chim_already_commented'];
    }
    $cooldown = max(0, (int)$settings['cooldown_minutes']) * 60;
    if ($cooldown > 0 && $now - (int)$settings['last_reflect_ts'] < $cooldown) {
        return [false, 'cooldown'];
    }
    $chance = max(0, min(100, (int)$settings['chance']));
    if ($chance < 100 && random_int(1, 100) > $chance) {
        return [false, 'chance'];
    }
    return [true, 'ok'];
}

function chimGoreBuildInstruction(array $settings, string $npcName, string $details): string
{
    $template = trim((string)$settings['instruction']);
    if ($template === '') {
        $template = chimGoreDefaults()['instruction'];
    }
    if (strpos($template, '{DETAILS}') === false) {
        $template .= ' What happened: {DETAILS}';
    }
    return strtr($template, ['{NPC}' => $npcName !== '' ? $npcName : 'The follower', '{DETAILS}' => $details]);
}

/**
 * Called from prerequest.php (after the NPC profile is loaded) for every request. Returns:
 *   null                            - not ours, continue normally
 *   ['allow'=>false]                - drop the request (terminate)
 *   ['allow'=>true,'data'=>string]  - continue with the rewritten instruction
 */
function chimGoreHandleRequest(array $gameRequest): ?array
{
    $type = (string)($gameRequest[0] ?? '');
    if ($type === 'combatend' || $type === 'combatendmighty') {
        chimGoreSetValue('last_chim_combat_ts', (string)time());
        return null;
    }
    if ($type !== 'instruction' || !isset($gameRequest[3])) {
        return null;
    }
    $parsed = chimGoreParseRequest((string)$gameRequest[3]);
    if ($parsed === null) {
        return null;
    }
    $settings = chimGoreGetSettings();
    chimGoreIncrement('stat_requests');
    $npc = (string)($GLOBALS['HERIKA_NAME'] ?? '');
    [$allowed, $reason] = chimGoreDecide($settings, $parsed['values']);
    if (!$allowed) {
        chimGoreLog('info', "Reaction for {$npc} skipped ({$reason}), score {$parsed['values']['score']}");
        return ['allow' => false];
    }
    chimGoreSetValue('last_reflect_ts', (string)time());
    chimGoreIncrement('stat_spoken');
    chimGoreLog('info', "Reaction allowed for {$npc}, score {$parsed['values']['score']}: {$parsed['details']}");
    return ['allow' => true, 'data' => $parsed['prefix'] . chimGoreBuildInstruction($settings, $npc, $parsed['details'])];
}

// ---------------------------------------------------------------------------------------
// Diagnostic file
// ---------------------------------------------------------------------------------------

function chimGoreDiagnosticReport(): string
{
    $settings = chimGoreGetSettings();
    $status = chimGoreGameStatus($settings);
    $out = [];
    $out[] = 'CHIM Gore diagnostic report';
    $out[] = 'Created: ' . gmdate('Y-m-d H:i:s') . ' UTC';
    $out[] = 'Server plugin version: ' . CHIM_GORE_VERSION;
    $version = @file_get_contents(dirname(__DIR__, 3) . '/.version.txt');
    $out[] = 'HerikaServer version: ' . trim((string)$version);
    $out[] = 'PHP: ' . PHP_VERSION;
    $out[] = '';
    $out[] = '== Game plugin (last report ' . ((int)$settings['game_status_ts'] > 0 ? gmdate('Y-m-d H:i:s', (int)$settings['game_status_ts']) . ' UTC' : 'never') . ') ==';
    foreach ($status as $key => $value) {
        $out[] = sprintf('%-22s %s', $key . ':', is_bool($value) ? ($value ? 'true' : 'false') : (string)$value);
    }
    $out[] = '';
    $out[] = '== Settings ==';
    foreach ($settings as $key => $value) {
        if (in_array($key, ['game_status'], true)) continue;
        $out[] = sprintf('%-32s %s', $key . ':', str_replace("\n", ' ', (string)$value));
    }
    $out[] = '';
    $out[] = '== Log (server and game, oldest first) ==';
    foreach (chimGoreLogFetch(CHIM_GORE_LOG_KEEP) as $row) {
        $out[] = sprintf('%s [%s/%s] %s', $row['created_at'] ?? '', $row['source'] ?? '', $row['level'] ?? '', $row['message'] ?? '');
    }
    $out[] = '';
    $out[] = 'Full game log: Documents\\My Games\\Skyrim Special Edition\\SKSE\\chim-gore.log';
    return implode("\n", $out) . "\n";
}
