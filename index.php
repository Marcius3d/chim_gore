<?php
/**
 * CHIM Gore - settings page (CHIM web UI -> Server Plugins -> chim_gore -> Plugin Page).
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

$message = '';
$error = '';
$dbReady = chimGoreDbReady();

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = (string)($_POST['action'] ?? 'save');
    if (!$dbReady) {
        $error = 'The plugin table is missing. Reinstall the plugin package so its migration runs.';
    } elseif ($action === 'reset_cooldown') {
        chimGoreSetValue('last_reflect_ts', '0');
        $message = 'Cooldown reset. The next qualifying fight can be commented on right away.';
    } elseif ($action === 'restore_text') {
        chimGoreSetValue('instruction', chimGoreDefaults()['instruction']);
        $message = 'Default instruction restored.';
    } else {
        $values = [
            'enabled' => isset($_POST['enabled']) ? '1' : '0',
            'chance' => (string)max(0, min(100, (int)($_POST['chance'] ?? 35))),
            'cooldown_minutes' => (string)max(0, min(1440, (int)($_POST['cooldown_minutes'] ?? 10))),
            'min_score' => (string)max(0, min(50, (int)($_POST['min_score'] ?? 4))),
            'instruction' => trim(mb_substr((string)($_POST['instruction'] ?? ''), 0, 2000)),
        ];
        $ok = true;
        foreach ($values as $key => $value) {
            $ok = chimGoreSetValue($key, $value) && $ok;
        }
        $ok ? $message = 'Settings saved.' : $error = 'Some settings could not be saved.';
    }
}

$settings = chimGoreGetSettings();
$lastTs = (int)$settings['last_reflect_ts'];
$cooldownLeft = max(0, $lastTs + (int)$settings['cooldown_minutes'] * 60 - time());
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
  main { max-width: 820px; margin: 0 auto; padding: 24px 16px 48px; }
  h1 { margin:0 0 4px; font-size: 26px; letter-spacing:.5px; }
  h1 span { color: var(--accent); }
  .sub { color: var(--muted); margin: 0 0 20px; }
  section { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:18px; margin-bottom:16px; }
  h2 { font-size:16px; margin:0 0 12px; }
  label { display:block; margin: 12px 0 4px; font-weight:600; }
  .hint { color:var(--muted); font-size:13px; margin:2px 0 0; }
  input[type=number] { width: 120px; }
  input, textarea { background:#0f0c0a; color:var(--text); border:1px solid var(--line); border-radius:6px; padding:8px; font:inherit; }
  textarea { width:100%; min-height:110px; }
  .row { display:grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 8px 16px; }
  button { background:var(--accent); color:#fff; border:0; border-radius:6px; padding:9px 16px; font:inherit; cursor:pointer; }
  button.secondary { background:transparent; border:1px solid var(--line); color:var(--text); }
  .msg { padding:10px 12px; border-radius:6px; margin-bottom:16px; }
  .ok { background:#1f2a1a; border:1px solid var(--ok); }
  .err { background:#2a1716; border:1px solid var(--accent); }
  table { border-collapse: collapse; width:100%; }
  td { padding:6px 4px; border-bottom:1px solid var(--line); }
  td:first-child { color:var(--muted); width:55%; }
  code { background:#0f0c0a; padding:1px 5px; border-radius:4px; }
  .inline { display:flex; gap:8px; flex-wrap:wrap; margin-top:14px; }
</style>
</head>
<body>
<main>
  <h1>CHIM <span>Gore</span></h1>
  <p class="sub">Followers remember decapitations and severed limbs, and one of them reacts after the fight. Version <?= cgH(CHIM_GORE_VERSION) ?>.</p>

  <?php if ($message): ?><div class="msg ok"><?= cgH($message) ?></div><?php endif; ?>
  <?php if ($error): ?><div class="msg err"><?= cgH($error) ?></div><?php endif; ?>
  <?php if (!$dbReady): ?><div class="msg err">Database table <code>plugins.chim_gore_settings</code> was not found. Defaults are used and nothing can be saved until the plugin package is reinstalled.</div><?php endif; ?>

  <form method="post">
    <section>
      <h2>After-combat reaction</h2>
      <label><input type="checkbox" name="enabled" value="1" <?= $settings['enabled'] === '1' ? 'checked' : '' ?>> Allow a follower to comment after a gory fight</label>
      <p class="hint">Gory kills are still written to CHIM's memory when this is off; only the spoken reaction is skipped.</p>
      <div class="row">
        <div>
          <label for="chance">Chance (%)</label>
          <input id="chance" type="number" name="chance" min="0" max="100" value="<?= cgH($settings['chance']) ?>">
          <p class="hint">Of qualifying fights, how many get a comment.</p>
        </div>
        <div>
          <label for="cooldown">Cooldown (real minutes)</label>
          <input id="cooldown" type="number" name="cooldown_minutes" min="0" max="1440" value="<?= cgH($settings['cooldown_minutes']) ?>">
          <p class="hint">Minimum time between two comments.</p>
        </div>
        <div>
          <label for="score">Minimum gore score</label>
          <input id="score" type="number" name="min_score" min="0" max="50" value="<?= cgH($settings['min_score']) ?>">
          <p class="hint">Head = 6 (+1 per 2 m it flew), limb = 2, finishing move = 1. The best moment of the fight counts.</p>
        </div>
      </div>
      <label for="instruction">Instruction sent to the follower</label>
      <textarea id="instruction" name="instruction"><?= cgH($settings['instruction']) ?></textarea>
      <p class="hint"><code>{NPC}</code> = the follower's name, <code>{DETAILS}</code> = what happened (written by the game plugin, in English).</p>
      <div class="inline">
        <button type="submit" name="action" value="save">Save</button>
        <button type="submit" name="action" value="restore_text" class="secondary">Restore default instruction</button>
        <button type="submit" name="action" value="reset_cooldown" class="secondary">Reset cooldown</button>
      </div>
    </section>
  </form>

  <section>
    <h2>Status</h2>
    <table>
      <tr><td>Reaction requests received from the game</td><td><?= cgH($settings['stat_requests']) ?></td></tr>
      <tr><td>Reactions allowed</td><td><?= cgH($settings['stat_spoken']) ?></td></tr>
      <tr><td>Last reaction</td><td><?= $lastTs > 0 ? cgH(date('Y-m-d H:i:s', $lastTs)) : 'never' ?></td></tr>
      <tr><td>Cooldown remaining</td><td><?= $cooldownLeft > 0 ? cgH(ceil($cooldownLeft / 60) . ' min') : 'none' ?></td></tr>
    </table>
    <p class="hint">Game-side options (delays, follower range, player kills, notifications) are in <code>Data/SKSE/Plugins/CHIMGore.ini</code>. Game log: <code>Documents/My Games/Skyrim Special Edition/SKSE/CHIMGore.log</code>.</p>
  </section>
</main>
</body>
</html>
