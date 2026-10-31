<?php
declare(strict_types=1);

/**
 * Two real libdatachannel ICE/DTLS/SCTP peers, with messages round-tripped
 * through PHP's native async event queue. No fake transport or mocked socket.
 */
if (!extension_loaded('ryzertc')) {
    throw new RuntimeException('RyzeRTC native extension not loaded');
}
$a = new RyzeRTC\PeerConnection('127.0.0.1', 0, false);
$b = new RyzeRTC\PeerConnection('127.0.0.1', 0, false);
$channelA = $a->createDataChannel('minecraft', true, true);
$a->beginOffer();

$offerSet = false;
$answerSet = false;
$pendingA = [];
$pendingB = [];
$readyA = false;
$channelB = null;
$sentA = false;
$sentB = false;
$receivedA = false;
$receivedB = false;
$payloadA = "RYZE\x00RTC\xff";
$payloadB = "ACK\x00BEDROCK";

$flush = static function (RyzeRTC\PeerConnection $peer, array &$pending): void {
    foreach ($pending as [$candidate, $mid]) {
        $peer->addRemoteCandidate($candidate, $mid);
    }
    $pending = [];
};

try {
    $deadline = microtime(true) + 14.0;
    while (microtime(true) < $deadline && !($receivedA && $receivedB)) {
        foreach ($a->pollEvents() as $event) {
            if ($event['type'] === 'description' && $event['detail'] === 'offer' && !$offerSet) {
                $b->setRemoteOffer($event['data']);
                $offerSet = true;
                $flush($b, $pendingB);
                $b->beginAnswer();
            } elseif ($event['type'] === 'candidate') {
                if ($offerSet) {
                    $b->addRemoteCandidate($event['data'], $event['detail']);
                } else {
                    $pendingB[] = [$event['data'], $event['detail']];
                }
            } elseif ($event['type'] === 'channel_open' && $event['channel'] === $channelA) {
                $readyA = true;
            } elseif ($event['type'] === 'message') {
                if ($event['data'] !== $payloadB) {
                    throw new RuntimeException('A received corrupt binary payload');
                }
                $receivedA = true;
            }
        }
        foreach ($b->pollEvents() as $event) {
            if ($event['type'] === 'description' && $event['detail'] === 'answer' && !$answerSet) {
                $a->setRemoteAnswer($event['data']);
                $answerSet = true;
                $flush($a, $pendingA);
                $flush($b, $pendingB);
            } elseif ($event['type'] === 'candidate') {
                if ($answerSet) {
                    $a->addRemoteCandidate($event['data'], $event['detail']);
                } else {
                    $pendingA[] = [$event['data'], $event['detail']];
                }
            } elseif ($event['type'] === 'channel') {
                $channelB = $event['channel'];
            } elseif ($event['type'] === 'message') {
                if ($event['data'] !== $payloadA) {
                    throw new RuntimeException('B received corrupt binary payload');
                }
                $receivedB = true;
            }
        }
        if ($readyA && !$sentA) {
            $a->sendMessage($channelA, $payloadA);
            $sentA = true;
        }
        if ($receivedB && $channelB !== null && !$sentB) {
            $b->sendMessage($channelB, $payloadB);
            $sentB = true;
        }
        usleep(10000);
    }
    if (!$offerSet || !$answerSet || !$readyA || $channelB === null || !$receivedA || !$receivedB) {
        throw new RuntimeException('RyzeRTC native loopback failed: ' . json_encode([
            'offer' => $offerSet,
            'answer' => $answerSet,
            'channelAOpen' => $readyA,
            'channelB' => $channelB,
            'messageB' => $receivedB,
            'messageA' => $receivedA,
        ], JSON_THROW_ON_ERROR));
    }
} finally {
    fwrite(STDERR, "[RyzeRTC-test] Closing peer A\n");
    $a->close();
    fwrite(STDERR, "[RyzeRTC-test] Closing peer B\n");
    $b->close();
    fwrite(STDERR, "[RyzeRTC-test] Peer close methods completed\n");
}
echo "RyzeRTC native ICE/DTLS/SCTP binary loopback and peer close OK\n";
