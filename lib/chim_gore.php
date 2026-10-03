<?php
/**
 * CHIM Gore - shared server helpers.
 *
 * Note: HerikaServer loads hook files (preprocessing.php, context.php, ...) recursively from
 * every folder under ext/. Do not give files in this folder a hook name.
 */

if (defined('CHIM_GORE_VERSION')) {
    return;
}

define('CHIM_GORE_VERSION', '0.1.0');
define('CHIM_GORE_TABLE', 'plugins.chim_gore_settings');

/** Marker added by CHIMGore.dll to its reflection request. */
define('CHIM_GORE_MARKER_REGEX', '/\[chim_gore\s+([^\]]*)\]\s*/i');

function chimGoreDefaults(): array
{
    return [
        'enabled' => '1',
        'chance' => '35',              // % chance that a qualifying fight gets a comment
        'cooldown_minutes' => '10',    // real-time minutes between comments
        'min_score' => '4',            // 6 = any decapitation, 2 = one limb
        'instruction' => '{NPC} briefly reacts to the most gruesome moment of the fight that just ended, '
            . 'in their own voice and personality: shocked, amused, disgusted or proud, whatever fits them. '
            . 'One or two short sentences. Only mention injuries listed here. What happened: {DETAILS}',
        'last_reflect_ts' => '0',
        'stat_requests' => '0',
        'stat_spoken' => '0',
    ];
}

function chimGoreDb()
{
    return $GLOBALS['db'] ?? null;
}

function chimGoreDbReady(): bool
{
    static $ready = null;
    if ($ready !== null) {
        return $ready;
    }
    $db = chimGoreDb();
    if (!$db) {
        return $ready = false;
    }
    try {
        $row = $db->fetchOne("SELECT to_regclass('" . CHIM_GORE_TABLE . "') AS t");
        $ready = !empty($row['t']);
    } catch (Throwable $e) {
        $ready = false;
    }
    return $ready;
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

/**
 * Splits a reflection request into marker values and the readable details.
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
 * Applies enable/score/cooldown/chance policy. Returns [allowed, reason].
 */
function chimGoreDecide(array $settings, array $values, ?int $now = null): array
{
    $now = $now ?? time();
    if ((string)$settings['enabled'] !== '1') {
        return [false, 'disabled'];
    }
    if ((int)$values['score'] < (int)$settings['min_score']) {
        return [false, 'score_below_minimum'];
    }
    $cooldown = max(0, (int)$settings['cooldown_minutes']) * 60;
    $elapsed = $now - (int)$settings['last_reflect_ts'];
    if ($cooldown > 0 && $elapsed < $cooldown) {
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

function chimGoreLog(string $level, string $message): void
{
    $line = '[chim_gore] ' . $message;
    if (class_exists('Logger') && method_exists('Logger', $level)) {
        Logger::$level($line);
    } else {
        error_log($line);
    }
}

/**
 * Called from prerequest.php (after the NPC profile is loaded) for every request. Returns:
 *   null           - not ours, continue normally
 *   ['allow'=>false]               - drop the request (terminate)
 *   ['allow'=>true,'data'=>string] - continue with the rewritten instruction
 */
function chimGoreHandleRequest(array $gameRequest): ?array
{
    if (($gameRequest[0] ?? '') !== 'instruction' || !isset($gameRequest[3])) {
        return null;
    }
    $parsed = chimGoreParseRequest((string)$gameRequest[3]);
    if ($parsed === null) {
        return null;
    }
    $settings = chimGoreGetSettings();
    chimGoreIncrement('stat_requests');
    [$allowed, $reason] = chimGoreDecide($settings, $parsed['values']);
    if (!$allowed) {
        chimGoreLog('info', "reflection skipped ({$reason}), score={$parsed['values']['score']}");
        return ['allow' => false];
    }
    chimGoreSetValue('last_reflect_ts', (string)time());
    chimGoreIncrement('stat_spoken');
    $npc = (string)($GLOBALS['HERIKA_NAME'] ?? '');
    chimGoreLog('info', "reflection allowed for {$npc}, score={$parsed['values']['score']}");
    return ['allow' => true, 'data' => $parsed['prefix'] . chimGoreBuildInstruction($settings, $npc, $parsed['details'])];
}
