# RyzeRTC

**Native WebRTC transport extension for PHP — a RyzeCompany project.**

RyzeRTC is RyzeCompany's own PHP extension, delivered as a native Linux `ryzertc.so` file independently of RyzeMP's PHAR. It uses [libdatachannel](https://github.com/paullouisageneau/libdatachannel) as a lower-level open-source C/C++ WebRTC engine, while the PHP extension and integration API are maintained by RyzeCompany.

> **Status: experimental native transport.** Builds for the Egg target **Debian 12 / glibc 2.36**; previous v0.1.0-dev.1 binaries compiled on Ubuntu 24.04 are incompatible with the Debian 12 Egg and must not be installed there. DataChannels and a bounded, mutex-protected event queue have been implemented and **verified** in real ICE/DTLS/SCTP bidirectional loopback under the exact official Egg PHP (see [passing native CI](https://github.com/RyzeCompany/RyzeRTC/actions/runs/41148078294)). The RyzeMP feature PR now integrates authenticated HTTP signaling, signed answers, shared RakLib UDP and Minecraft sessions. Retail-client login and gameplay remain a required host test; native CI alone does not establish that Terracotta is fixed.

## Native PHP API

```php
<?php
if (!extension_loaded('ryzertc')) {
    throw new RuntimeException('RyzeRTC native runtime missing');
}

echo RyzeRTC\Runtime::version();
var_dump(RyzeRTC\Runtime::apiVersion()); // 2
var_dump(RyzeRTC\Runtime::capabilities());

$peer = new RyzeRTC\PeerConnection('127.0.0.1', 0, false); // standalone example; not a public game port
$channel = $peer->createDataChannel('minecraft', true, true); // reliable and ordered
$peer->setRemoteOffer($trustedOfferSdp);
$peer->addRemoteCandidate($trustedIceCandidate, $trustedMediaId);
$peer->beginAnswer();
// Native ICE gathers asynchronously; poll until localDescription() is available.
$answer = $peer->localDescription();
$selectedRemote = $peer->remoteAddress(); // null until ICE selects a peer endpoint
$info = $peer->channelInfo($channel); // label, reliability, open, limits and buffered bytes
foreach ($peer->pollEvents(64) as $event) {
    // description/candidate/state/gathering/channel/channel_open/message/overflow
    // message.detail is binary or text. On overflow, close the peer.
    // Never use remote SDP/ICE without trusted Minecraft signaling identity.
}
// Only after channel_open: $peer->sendMessage($channel, $binaryFrame);
$peer->close();
```

ICE candidates and SDP are bounded and validated for basic syntax, but **calling these methods does not verify a Minecraft identity**. The sample shows the evolving **native transport API**. A RyzeMP game integration must additionally bind trusted Minecraft identity assertions to the DTLS fingerprint and the Bedrock login chain. Never treat untrusted SDP or an open native peer as a verified Minecraft player. All client identity verification and shared-UDP routing are separate security-sensitive stages.

## Build and test

The workflow [native-linux.yml](.github/workflows/native-linux.yml):

1. Builds pinned **PHP 8.4.27 ZTS** development tools (PHP API 20240924).
2. Builds pinned **libdatachannel v0.24.6** plus its upstream submodules from source.
3. Compiles RyzeRTC's own `ryzertc.so` using `phpize`.
4. Runs native smoke, teardown under GDB and real ICE/DTLS/SCTP bidirectional binary loopback (including embedded NUL bytes).
5. Verifies the module against the **official PocketMine PHP 8.4.16 ZTS runtime** used by the existing RyzeMP Egg.
6. Uploads an experimental CI package. Separately, the [first-party tagged prerelease](https://github.com/RyzeCompany/RyzeRTC/releases/tag/v0.1.0-dev.3) distributes the tested native runtime, shared libraries, license notices and SHA-256 checksums.

On a prepared Linux x86_64 build host with a matching ZTS PHP 8.4 `phpize`/`php-config` and a local libdatachannel install:

```bash
phpize
./configure --with-php-config=/path/to/php-config --with-ryzertc=/path/to/libdatachannel/install
make -j2
LD_LIBRARY_PATH=/path/to/libdatachannel/install/lib \
  php -n -d extension="$PWD/modules/ryzertc.so" tests/smoke.php
```

**Distribution:** An experimental prerelease, `v0.1.0-dev.3`, is now published. The Egg installer `scripts/install-egg-runtime.sh` downloads that exact tagged archive with a pinned SHA-256, verifies its inner manifest, tests it with the installed PHP and leaves plugins/worlds/PHAR untouched. The [installer CI](https://github.com/RyzeCompany/RyzeRTC/actions/runs/41148519352) verifies loading from the actual release and checksum rejection. **The Egg JSON and RyzeMP session adapter are integrated on their existing feature PRs.** Never announce Bedrock 2225 connectivity merely because the `.so` loads. The adapter uses existing `server-ip` and `server-port` for public TCP and UDP; its automatically allocated ICE mux listens only on loopback. See the [RyzeMP integration and test guide](https://github.com/RyzeCompany/RyzeMP/blob/feature/bedrock-1.26.60-nethernet/docs/BEDROCK_1_26_60_NETHERNET.md).

## Roadmap

- [x] Standalone RyzeCompany repository, native module and PHP 8.4 ABI design.
- [x] Native peer creation/destruction, SDP inspection and bounded remote ICE-candidate APIs.
- [x] Native reliable/unreliable DataChannel methods and bounded async event queue (implementation).
- [x] Verified two-peer ICE/DTLS/SCTP binary loopback on the actual Egg PHP.
- [x] Bounded native gathering/state/message event queue (GDB and loopback CI passing).
- [x] SCTP DataChannels with native send buffering and teardown handling (implementation, integration tests pending).
- [x] RyzeMP integration: official JWT/fingerprint binding, signed full-ICE answers, selected endpoint mapping and shared RakLib UDP.
- [ ] Retail Bedrock 1.26.60 handshake tests, load/reconnection tests.
- [x] Tagged, checksummed **experimental prerelease** and independently tested installation script.
- [x] Pterodactyl Egg JSON integration with pinned API 2 archive and checksum.
- [ ] Real public host/client validation and production approval.

## Attribution

Original code and API: Copyright (c) **RyzeCompany**, licensed MIT. libdatachannel and its submodules remain third-party software under their respective licenses: [THIRD_PARTY.md](THIRD_PARTY.md).
## API 2 integration contract

`PeerConnection::channelInfo(int)` validates ownership and reports `label`, `open`, `ordered`, `reliable`, `maxRetransmits`, `maxMessageSize` and `bufferedAmount`. `remoteAddress()` reports the **selected** ICE remote endpoint; RyzeMP matches its private relay port to RakLib's actual public peer observation. Never substitute an untrusted SDP address or TCP source port.

`pollEvents()` returns copied binary bytes only on the caller's PHP thread. Each `message` includes `detail=binary` or `detail=text`. Queue exhaustion produces a persistent `overflow` event for the affected peer. Close that peer: continuing after a lost reliable message would corrupt application framing. This API does not expose per-message SCTP acknowledgements, so local send success must never be used as a fake RakNet receipt.

Published `v0.1.0-dev.3` ZIP SHA-256: `9ecea6cc9d239b8c583d042c91df0448432321ceaf128850fe5b27bcde234602`.
