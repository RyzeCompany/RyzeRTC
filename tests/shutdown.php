<?php
declare(strict_types=1);

// Regression for SIGSEGV (exit 139) in process-global native worker teardown.
// This is intentionally executed as a fresh CLI process repeatedly in CI.
if (!extension_loaded('ryzertc')) {
    throw new RuntimeException('RyzeRTC extension not loaded');
}

for ($i = 0; $i < 20; ++$i) {
    $peer = new RyzeRTC\PeerConnection('127.0.0.1', 0, false);
    if ($i % 3 === 0) {
        $peer->close();
        $peer->close(); // Idempotent: must not double-close native transport.
    }
    unset($peer); // Exercises automatic zend_object destructor.
}

gc_collect_cycles();
if (RyzeRTC\Runtime::apiVersion() !== 2) {
    throw new RuntimeException('Invalid runtime API version');
}
echo "Clean shutdown regression complete\n";
// A successful print is not proof: GitHub Actions also checks exit code == 0.
