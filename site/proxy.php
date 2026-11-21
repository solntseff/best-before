<?php
// SPDX-License-Identifier: GPL-3.0-or-later
header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST');

$dbFile = __DIR__ . '/products.db';

// Инициализация базы SQLite при первом запуске
try {
    $db = new PDO("sqlite:" . $dbFile);
    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    $db->exec("CREATE TABLE IF NOT EXISTS custom_products (
        barcode TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    )");
} catch (Exception $e) {
    http_response_code(500);
    echo json_encode(['error' => 'Database error: ' . $e->getMessage()]);
    exit;
}

// РЕЖИМ 1: Сохранение пользовательского названия (Самообучение)
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $barcode = filter_input(INPUT_POST, 'barcode', FILTER_SANITIZE_FULL_SPECIAL_CHARS);
    $name = trim(filter_input(INPUT_POST, 'name', FILTER_SANITIZE_FULL_SPECIAL_CHARS));

    if (!$barcode || !$name || !preg_match('/^[0-9]{8,14}$/', $barcode)) {
        http_response_code(400);
        echo json_encode(['error' => 'Invalid barcode or name']);
        exit;
    }

    $stmt = $db->prepare("INSERT OR REPLACE INTO custom_products (barcode, name) VALUES (:barcode, :name)");
    $stmt->execute([':barcode' => $barcode, ':name' => $name]);
    echo json_encode(['status' => 'success', 'saved' => true]);
    exit;
}

// РЕЖИМ 2: Поиск товара (GET)
$barcode = filter_input(INPUT_GET, 'barcode', FILTER_SANITIZE_FULL_SPECIAL_CHARS);

if (!$barcode || !preg_match('/^[0-9]{8,14}$/', $barcode)) {
    http_response_code(400);
    echo json_encode(['error' => 'Invalid barcode format']);
    exit;
}

// Шаг А: Поиск в глобальной базе Open Food Facts
$url = "https://world.openfoodfacts.org/api/v2/product/{$barcode}.json";
$ch = curl_init();
curl_setopt($ch, CURLOPT_URL, $url);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 4);
curl_setopt($ch, CURLOPT_USERAGENT, 'FridgeTracker - MultiRegion - Version 2.0');
$response = curl_exec($ch);
$httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);

if ($httpCode === 200 && $response) {
    $data = json_decode($response, true);
    if (isset($data['status']) && $data['status'] === 1 && isset($data['product'])) {
        $p = $data['product'];
        // Мультиязычный каскад: русский -> иврит -> английский -> общий заголовок
        $title = $p['product_name_ru'] 
              ?? $p['product_name_he'] 
              ?? $p['product_name_en'] 
              ?? $p['product_name'] 
              ?? '';

        if (!empty(trim($title))) {
            echo json_encode([
                'status' => 1,
                'source' => 'openfoodfacts',
                'name' => trim($title)
            ]);
            exit;
        }
    }
}

// Шаг Б: Поиск в локальной базе SQLite на хостинге
$stmt = $db->prepare("SELECT name FROM custom_products WHERE barcode = :barcode LIMIT 1");
$stmt->execute([':barcode' => $barcode]);
$row = $stmt->fetch(PDO::FETCH_ASSOC);

if ($row && !empty($row['name'])) {
    echo json_encode([
        'status' => 1,
        'source' => 'local_sqlite',
        'name' => $row['name']
    ]);
    exit;
}

// Товар не найден нигде
echo json_encode([
    'status' => 0,
    'source' => 'none',
    'name' => ''
]);