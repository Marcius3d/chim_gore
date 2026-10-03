<?php
// Shared bootstrap for the CHIM Gore API endpoints (not a HerikaServer hook name).
error_reporting(E_ALL);
ini_set('display_errors', '0');

$enginePath = dirname(__DIR__, 3) . DIRECTORY_SEPARATOR;
require_once $enginePath . 'lib' . DIRECTORY_SEPARATOR . 'runtime_bootstrap.php';
chimRuntimeBootstrap($enginePath, [
    'load_general_settings' => false,
    'load_stt_connector' => false,
    'load_itt_connector' => false,
    'load_player_name' => false,
    'load_narrator' => false,
]);
$GLOBALS['db'] = $GLOBALS['db'] ?? new sql();

require_once dirname(__DIR__) . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_gore.php';
