<?php
/**
 * POST: status report and new log lines from CHIMGore.dll (JSON).
 */
require __DIR__ . DIRECTORY_SEPARATOR . 'bootstrap_inc.php';

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
    http_response_code(405);
    echo '{"ok":false,"error":"post_only"}';
    exit;
}
$error = chimGoreHandleStatus((string)file_get_contents('php://input', false, null, 0, 262145));
if ($error !== null) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => $error]);
    exit;
}
echo '{"ok":true}';
