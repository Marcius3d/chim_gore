<?php
/**
 * CHIM Gore - plugin page (CHIM web UI -> Server Plugins -> CHIM Gore -> Plugin Page).
 */

$enginePath = dirname(__DIR__, 2) . DIRECTORY_SEPARATOR;
require_once $enginePath . 'lib' . DIRECTORY_SEPARATOR . 'runtime_bootstrap.php';
chimRuntimeBootstrap($enginePath, [
    'load_general_settings' => true,
    'load_player_name' => false,
    'load_narrator' => false,
]);
$GLOBALS['db'] = $GLOBALS['db'] ?? new sql();

require_once __DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_gore.php';

function cgH($value): string
{
    return htmlspecialchars((string)$value, ENT_QUOTES, 'UTF-8');
}

function cgChecked(array $settings, string $key): string
{
    return ($settings[$key] ?? '0') === '1' ? 'checked' : '';
}

// Diagnostic file download
if (isset($_GET['download'])) {
    header('Content-Type: text/plain; charset=utf-8');
    header('Content-Disposition: attachment; filename="chim-gore-diagnostics-' . gmdate('Ymd-His') . '.txt"');
    header('Cache-Control: no-store');
    echo chimGoreDiagnosticReport();
    exit;
}

$message = '';
$error = '';
$dbReady = chimGoreDbReady();
$logReady = chimGoreTableExists(CHIM_GORE_LOG_TABLE);

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = (string)($_POST['action'] ?? 'save');
    if (!$dbReady) {
        $error = 'The plugin tables are missing. Reinstall the plugin package so its migrations run.';
    } elseif ($action === 'reset_cooldown') {
        chimGoreSetValue('last_reflect_ts', '0');
        chimGoreSetValue('last_chim_combat_ts', '0');
        $message = 'Cooldown reset. The next qualifying fight can be commented on right away.';
    } elseif ($action === 'restore_text') {
        chimGoreSetValue('instruction', chimGoreDefaults()['instruction']);
        $message = 'Default instruction restored.';
    } elseif ($action === 'clear_log') {
        chimGoreLogClear() ? $message = 'Diagnostics log cleared.' : $error = 'Could not clear the log.';
    } else {
        $bools = ['enabled', 'debug', 'use_ngd', 'use_df', 'log_each_event', 'reflect_after_combat', 'include_player_kills', 'enrich_chim_combat', 'enrich_death_line'];
        $values = [];
        foreach ($bools as $key) {
            $values[$key] = isset($_POST[$key]) ? '1' : '0';
        }
        $num = static fn(string $key, float $min, float $max, float $def): string
            => (string)max($min, min($max, is_numeric($_POST[$key] ?? null) ? (float)$_POST[$key] : $def));
        $values['check_delay_seconds'] = $num('check_delay_seconds', 0.5, 10, 2.5);
        $values['reflect_delay_min_seconds'] = $num('reflect_delay_min_seconds', 0, 600, 8);
        $values['reflect_delay_max_seconds'] = (string)max((float)$values['reflect_delay_min_seconds'], (float)$num('reflect_delay_max_seconds', 0, 600, 20));
        $values['follower_range_units'] = $num('follower_range_units', 200, 20000, 3000);
        $values['max_summary_lines'] = (string)(int)$num('max_summary_lines', 1, 6, 3);
        $values['chance'] = (string)(int)$num('chance', 0, 100, 35);
        $values['cooldown_minutes'] = (string)(int)$num('cooldown_minutes', 0, 1440, 10);
        $values['min_score'] = (string)(int)$num('min_score', 0, 50, 4);
        $values['skip_after_chim_combat_seconds'] = (string)(int)$num('skip_after_chim_combat_seconds', 0, 600, 45);
        $values['instruction'] = trim(mb_substr((string)($_POST['instruction'] ?? ''), 0, 2000));
        $ok = true;
        foreach ($values as $key => $value) {
            $ok = chimGoreSetValue($key, $value) && $ok;
        }
        if ($ok) {
            $message = 'Settings saved. The game picks them up within 30 seconds or on the next save load.';
            chimGoreLog('info', 'Settings changed on the plugin page (debug=' . $values['debug'] . ')');
        } else {
            $error = 'Some settings could not be saved.';
        }
    }
}

$settings = chimGoreGetSettings();
$status = chimGoreGameStatus($settings);
$statusTs = (int)$settings['game_status_ts'];
$statusAge = $statusTs > 0 ? time() - $statusTs : null;
$gameOnline = $statusAge !== null && $statusAge < 90;
$lastTs = (int)$settings['last_reflect_ts'];
$cooldownLeft = max(0, $lastTs + (int)$settings['cooldown_minutes'] * 60 - time());
$logSource = in_array($_GET['source'] ?? '', ['game', 'server'], true) ? $_GET['source'] : null;
$logRows = chimGoreLogFetch(150, $logSource);

function cgModRow(string $label, bool $known, bool $loaded, string $version, string $extra = ''): string
{
    if (!$known) {
        return '<span class="muted">unknown (no report from the game yet)</span>';
    }
    if (!$loaded) {
        return '<span class="bad">not installed</span>';
    }
    return '<span class="good">installed</span>' . ($version !== '' ? ' <span class="muted">v' . cgH($version) . '</span>' : '') . $extra;
}
?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CHIM Gore</title>
<style>
  :root { color-scheme: dark; --bg:#14110f; --panel:#1e1916; --line:#3a302a; --text:#e9e2d8; --muted:#a99c8c; --accent:#b8402f; --ok:#6f9a5a; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font:15px/1.5 system-ui, sans-serif; }
  main { max-width: 900px; margin: 0 auto; padding: 24px 16px 48px; }
  h1 { margin:0 0 4px; font-size: 26px; letter-spacing:.5px; }
  h1 span { color: var(--accent); }
  .sub { color: var(--muted); margin: 0 0 20px; }
  section { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:18px; margin-bottom:16px; }
  h2 { font-size:16px; margin:0 0 12px; }
  label { display:block; margin: 12px 0 4px; font-weight:600; }
  label.check { font-weight:500; }
  .hint { color:var(--muted); font-size:13px; margin:2px 0 0; }
  input[type=number] { width: 120px; }
  input, textarea, select { background:#0f0c0a; color:var(--text); border:1px solid var(--line); border-radius:6px; padding:8px; font:inherit; }
  textarea { width:100%; min-height:110px; }
  .row { display:grid; grid-template-columns: repeat(auto-fit, minmax(230px, 1fr)); gap: 8px 16px; }
  button, .button { background:var(--accent); color:#fff; border:0; border-radius:6px; padding:9px 16px; font:inherit; cursor:pointer; text-decoration:none; display:inline-block; }
  button.secondary, .button.secondary { background:transparent; border:1px solid var(--line); color:var(--text); }
  .msg { padding:10px 12px; border-radius:6px; margin-bottom:16px; }
  .ok { background:#1f2a1a; border:1px solid var(--ok); }
  .err { background:#2a1716; border:1px solid var(--accent); }
  table { border-collapse: collapse; width:100%; }
  td { padding:6px 4px; border-bottom:1px solid var(--line); vertical-align: top; }
  td:first-child { color:var(--muted); width:42%; }
  code { background:#0f0c0a; padding:1px 5px; border-radius:4px; }
  .inline { display:flex; gap:8px; flex-wrap:wrap; margin-top:14px; align-items:center; }
  .good { color:#8fc27a; } .bad { color:#e0705f; } .muted { color:var(--muted); }
  .log { background:#0f0c0a; border:1px solid var(--line); border-radius:6px; padding:8px; max-height:420px; overflow:auto; font:12px/1.45 ui-monospace, Consolas, monospace; white-space:pre-wrap; word-break:break-word; }
  .log .warn { color:#e3b65c; } .log .error { color:#e0705f; } .log .debug { color:var(--muted); }
  .tag { display:inline-block; min-width:52px; color:var(--muted); }
  .sticky { position: sticky; bottom: 0; background: var(--bg); padding: 10px 0; }
</style>
</head>
<body>
<main>
  <h1>CHIM <span>Gore</span></h1>
  <p class="sub">Followers remember decapitations and severed limbs, and one of them reacts after the fight. Server plugin v<?= cgH(CHIM_GORE_VERSION) ?>.</p>

  <?php if ($message): ?><div class="msg ok"><?= cgH($message) ?></div><?php endif; ?>
  <?php if ($error): ?><div class="msg err"><?= cgH($error) ?></div><?php endif; ?>
  <?php if (!$dbReady || !$logReady): ?><div class="msg err">Plugin database tables are missing. Defaults are used and nothing can be saved until the plugin package is reinstalled.</div><?php endif; ?>

  <section>
    <h2>Status</h2>
    <table>
      <tr><td>Game plugin (CHIMGore.dll)</td><td>
        <?php if ($statusTs === 0): ?><span class="bad">no report yet</span> <span class="muted">start the game and load a save</span>
        <?php elseif ($gameOnline): ?><span class="good">connected</span> <span class="muted">v<?= cgH($status['version'] ?? '?') ?>, Skyrim <?= cgH($status['game_version'] ?? '?') ?>, last report <?= (int)$statusAge ?> s ago</span>
        <?php else: ?><span class="muted">offline, last report <?= cgH(date('Y-m-d H:i', $statusTs)) ?></span><?php endif; ?>
      </td></tr>
      <tr><td>Next-Gen Decapitations</td><td><?= cgModRow('NGD', $statusTs > 0, !empty($status['ngd_loaded']), (string)($status['ngd_version'] ?? ''), (!empty($status['ngd_loaded']) && empty($status['ngd_forms_ok'])) ? ' <span class="bad">(head forms missing, distance unavailable)</span>' : '') ?></td></tr>
      <tr><td>Dismembering Framework</td><td><?= cgModRow('DF', $statusTs > 0, !empty($status['df_loaded']), (string)($status['df_version'] ?? ''), !empty($status['df_loaded']) ? ' <span class="muted">(' . (int)($status['df_nodes'] ?? 0) . ' limb nodes)</span>' : '') ?></td></tr>
      <tr><td>This session: kills checked / gory / saved to CHIM memory</td><td><?= (int)($status['kills_seen'] ?? 0) ?> / <?= (int)($status['gore_events'] ?? 0) ?> / <?= (int)($status['logged_to_chim'] ?? 0) ?></td></tr>
      <tr><td>Reaction requests: from game / allowed by server</td><td><?= cgH($settings['stat_requests']) ?> / <?= cgH($settings['stat_spoken']) ?></td></tr>
      <tr><td>CHIM's own combat comments enriched with gore</td><td><?= cgH($settings['stat_enriched']) ?></td></tr>
      <tr><td>CHIM death lines extended with gore</td><td><?= cgH($settings['stat_death_lines']) ?></td></tr>
      <tr><td>Last reaction / cooldown left</td><td><?= $lastTs > 0 ? cgH(date('Y-m-d H:i:s', $lastTs)) : 'never' ?> / <?= $cooldownLeft > 0 ? cgH(ceil($cooldownLeft / 60) . ' min') : 'none' ?></td></tr>
      <?php if (!empty($status['last_error'])): ?><tr><td>Last error in game</td><td class="bad"><?= cgH($status['last_error']) ?></td></tr><?php endif; ?>
    </table>
  </section>

  <form method="post">
    <section>
      <h2>General</h2>
      <label class="check"><input type="checkbox" name="enabled" value="1" <?= cgChecked($settings, 'enabled') ?>> CHIM Gore enabled</label>
      <label class="check"><input type="checkbox" name="debug" value="1" <?= cgChecked($settings, 'debug') ?>> Debug mode</label>
      <p class="hint">Shows <code>CHIM-gore: request sent / request failed / request successful</code> in game (where your HUD shows notifications) and writes a detailed log. <code>CHIM-gore: activated</code> is always shown after loading a save.</p>
    </section>

    <section>
      <h2>Supported mods</h2>
      <p class="hint">Installed mods are detected automatically; one or both can be used. Untick a mod to ignore it.</p>
      <label class="check"><input type="checkbox" name="use_ngd" value="1" <?= cgChecked($settings, 'use_ngd') ?>> Use Next-Gen Decapitations (heads, how far the head flew)</label>
      <label class="check"><input type="checkbox" name="use_df" value="1" <?= cgChecked($settings, 'use_df') ?>> Use Dismembering Framework (heads and limbs, including creature packs)</label>
    </section>

    <section>
      <h2>In game</h2>
      <label class="check"><input type="checkbox" name="log_each_event" value="1" <?= cgChecked($settings, 'log_each_event') ?>> Save each gory kill to CHIM's memory (nearby NPCs will know)</label>
      <label class="check"><input type="checkbox" name="enrich_death_line" value="1" <?= cgChecked($settings, 'enrich_death_line') ?>> Append it to CHIM's own death line (shown in Prisma's recent context)</label>
      <label class="check"><input type="checkbox" name="reflect_after_combat" value="1" <?= cgChecked($settings, 'reflect_after_combat') ?>> A follower may react after the fight</label>
      <label class="check"><input type="checkbox" name="include_player_kills" value="1" <?= cgChecked($settings, 'include_player_kills') ?>> Count the player's own kills too (not only followers')</label>
      <div class="row">
        <div><label>Check delay after death (s)</label><input type="number" step="0.5" name="check_delay_seconds" min="0.5" max="10" value="<?= cgH($settings['check_delay_seconds']) ?>"></div>
        <div><label>Reaction delay after combat (s)</label><input type="number" name="reflect_delay_min_seconds" min="0" max="600" value="<?= cgH($settings['reflect_delay_min_seconds']) ?>"> – <input type="number" name="reflect_delay_max_seconds" min="0" max="600" value="<?= cgH($settings['reflect_delay_max_seconds']) ?>"></div>
        <div><label>Follower range (units, 70 ≈ 1 m)</label><input type="number" name="follower_range_units" min="200" max="20000" value="<?= cgH($settings['follower_range_units']) ?>"></div>
        <div><label>Moments described per fight</label><input type="number" name="max_summary_lines" min="1" max="6" value="<?= cgH($settings['max_summary_lines']) ?>"></div>
      </div>
    </section>

    <section>
      <h2>After-combat reaction</h2>
      <label class="check"><input type="checkbox" name="enrich_chim_combat" value="1" <?= cgChecked($settings, 'enrich_chim_combat') ?>> Add the fight's gore to CHIM's own combat-end comment</label>
      <p class="hint">CHIM often comments on a finished fight by itself. With this on, that comment also gets the decapitations and severed limbs of the fight, so the follower can talk about them even when CHIM Gore's own reaction is skipped.</p>
      <div class="row">
        <div><label>Chance (%)</label><input type="number" name="chance" min="0" max="100" value="<?= cgH($settings['chance']) ?>"><p class="hint">Of qualifying fights, how many get a comment.</p></div>
        <div><label>Cooldown (real minutes)</label><input type="number" name="cooldown_minutes" min="0" max="1440" value="<?= cgH($settings['cooldown_minutes']) ?>"><p class="hint">Minimum time between two comments.</p></div>
        <div><label>Minimum gore score</label><input type="number" name="min_score" min="0" max="50" value="<?= cgH($settings['min_score']) ?>"><p class="hint">Head = 6 (+1 per 2 m it flew), limb = 2, finishing move = 1.</p></div>
        <div><label>Skip if CHIM commented the fight (s)</label><input type="number" name="skip_after_chim_combat_seconds" min="0" max="600" value="<?= cgH($settings['skip_after_chim_combat_seconds']) ?>"><p class="hint">Avoids two comments on the same fight. 0 = never skip.</p></div>
      </div>
      <label for="instruction">Instruction sent to the follower</label>
      <textarea id="instruction" name="instruction"><?= cgH($settings['instruction']) ?></textarea>
      <p class="hint"><code>{NPC}</code> = the follower's name, <code>{DETAILS}</code> = what happened (written by the game plugin, in English).</p>
    </section>

    <div class="inline sticky">
      <button type="submit" name="action" value="save">Save settings</button>
      <button type="submit" name="action" value="restore_text" class="secondary">Restore default instruction</button>
      <button type="submit" name="action" value="reset_cooldown" class="secondary">Reset cooldown</button>
    </div>
  </form>

  <section>
    <h2>Diagnostics</h2>
    <p class="hint">If something does not work, create a diagnostic file and send it with your report. It contains versions, settings, detected mods and the log below (no API keys or dialogue history).</p>
    <div class="inline">
      <a class="button" href="?download=1">Create diagnostic file</a>
      <form method="post" style="display:inline"><button type="submit" name="action" value="clear_log" class="secondary">Clear log</button></form>
      <span class="muted">Show:</span>
      <a class="button secondary" href="?">all</a>
      <a class="button secondary" href="?source=game">game</a>
      <a class="button secondary" href="?source=server">server</a>
    </div>
    <p class="hint">The complete game log is on the gaming PC: <code>Documents\My Games\Skyrim Special Edition\SKSE\chim-gore.log</code> (the same folder as the other SKSE logs). Turn on debug mode for more detail.</p>
    <div class="log"><?php if (!$logRows): ?><span class="muted">No log entries yet.</span><?php endif; ?><?php foreach ($logRows as $row): ?><div class="<?= cgH($row['level']) ?>"><span class="tag"><?= cgH($row['source']) ?></span> <?= cgH($row['source'] === 'game' ? $row['message'] : ($row['created_at'] . ' ' . $row['message'])) ?></div><?php endforeach; ?></div>
  </section>
</main>
</body>
</html>
