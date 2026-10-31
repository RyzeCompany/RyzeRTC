<?php
declare(strict_types=1);

if (!extension_loaded('ryzertc')) {
    throw new RuntimeException('Missing RyzeRTC native module');
}
if (RyzeRTC\Runtime::apiVersion() !== 1) {
    throw new RuntimeException('Unexpected RyzeRTC API version');
}
$features = RyzeRTC\Runtime::capabilities();
if (!$features['peerConnection'] || !$features['iceUdpMux']) {
    throw new RuntimeException('Native PeerConnection backend unavailable');
}
if (!$features['dataChannels'] || !$features['asyncEventQueue'] || $features['bedrockIdentity']) {
    throw new RuntimeException('Native channel capability flags are inconsistent');
}
$peer = new RyzeRTC\PeerConnection("127.0.0.1", 0, false);
if ($peer->pollEvents() !== []) {
    throw new RuntimeException('New peer unexpectedly has pending events');
}
foreach (['', str_repeat('x', 129), "bad\0label"] as $label) {
    try {
        $peer->createDataChannel($label);
        throw new RuntimeException('Invalid channel label was accepted');
    } catch (ValueError) {
    }
}
foreach ([0, 257] as $limit) {
    try {
        $peer->pollEvents($limit);
        throw new RuntimeException('Invalid event limit accepted');
    } catch (ValueError) {
    }
}
if ($peer->remoteDescription() !== null) {
    throw new RuntimeException('Unexpected remote SDP before offer');
}
foreach ([['', '0'], ["bad\ncandidate", '0'], [str_repeat('x', 8193), '0'], ['candidate:1 1 udp 1 127.0.0.1 19132 typ host', '']] as [$candidate, $mid]) {
    try {
        $peer->addRemoteCandidate($candidate, $mid);
        throw new RuntimeException('Invalid ICE candidate was accepted');
    } catch (ValueError) {
    }
}
$peer->close();
$peer->close();
try {
    $peer->localDescription();
    throw new RuntimeException('Closed peer accepted an operation');
} catch (Error $e) {
    if (!str_contains($e->getMessage(), 'closed')) throw $e;
}
try {
    new RyzeRTC\PeerConnection("invalid.example", 0, false);
    throw new RuntimeException('Invalid bind address accepted');
} catch (ValueError) {
}
try {
    new RyzeRTC\PeerConnection("127.0.0.1", 0, true);
    throw new RuntimeException('UDP mux without a port accepted');
} catch (ValueError) {
}
echo "RyzeRTC native SCTP and bounded event API smoke OK\n";
