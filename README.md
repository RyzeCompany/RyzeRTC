# RyzeRTC

**Native WebRTC transport extension for PHP — a RyzeCompany project.**

RyzeRTC is RyzeCompany's own PHP extension, delivered as a native Linux \`ryzertc.so\` file independently of RyzeMP's PHAR. It uses [libdatachannel](https://github.com/paullouisageneau/libdatachannel) as a lower-level open-source C/C++ WebRTC engine, while the PHP extension and integration API are maintained by RyzeCompany.

> **Status: Phase 1 / experimental.** This branch contains a *new native extension*, **not yet a working Minecraft 1.26.60 server transport**. Do not replace a working server runtime until peer lifecycle, event queues, DataChannels, fingerprint authentication and the full Bedrock handshake are implemented and validated.

## Native PHP API

\`\`\`php
<?php
if (!extension_loaded('ryzertc')) {
    throw new RuntimeException('RyzeRTC native runtime missing');
}

echo RyzeRTC\Runtime::version();
var_dump(RyzeRTC\Runtime::apiVersion()); // 1
var_dump(RyzeRTC\Runtime::capabilities());

$peer = new RyzeRTC\PeerConnection('127.0.0.1', 19134, true);
$peer->setRemoteOffer($trustedOfferSdp);
$peer->beginAnswer();
// Native ICE gathers asynchronously; poll until localDescription() is available.
$answer = $peer->localDescription();
$peer->close();
\`\`\`

The sample shows the *Phase 1 API shape*. Never treat untrusted SDP or an open native peer as a verified Minecraft player. All client identity verification and shared-UDP routing are separate security-sensitive stages.

## Build and test

The workflow [native-linux.yml](.github/workflows/native-linux.yml):

1. Builds pinned **PHP 8.4.27 ZTS** development tools (PHP API 20240924).
2. Builds pinned **libdatachannel v0.24.6** plus its upstream submodules from source.
3. Compiles RyzeRTC's own \`ryzertc.so\` using \`phpize\`.
4. Runs smoke tests for peer creation, teardown, error handling and version/capability metadata.
5. Verifies the module against the **official PocketMine PHP 8.4.16 ZTS runtime** used by the existing RyzeMP Egg.
6. Uploads a temporary experimental package including the native module, its shared library and license/ABI metadata.

On a prepared Linux x86_64 build host with a matching ZTS PHP 8.4 \`phpize\`/\`php-config\` and a local libdatachannel install:

\`\`\`bash
phpize
./configure --with-php-config=/path/to/php-config --with-ryzertc=/path/to/libdatachannel/install
make -j2
LD_LIBRARY_PATH=/path/to/libdatachannel/install/lib \
  php -n -d extension="$PWD/modules/ryzertc.so" tests/smoke.php
\`\`\`

**Distribution:** only issue a versioned GitHub Release and update the Pterodactyl Egg **after** the native runtime is feature-complete and green against the actual server PHP. The Egg will download \`ryzertc.so\` separately; it will not be inside RyzeMP.phar. It must preserve plugins/worlds and reuse the existing \`server-ip\`/\`server-port\`.

## Roadmap

- [x] Standalone RyzeCompany repository, native module and PHP 8.4 ABI design.
- [x] Initial native peer creation/destruction and SDP interfaces.
- [ ] Native asynchronous gathering/state event queue with bounded resource limits.
- [ ] Reliable and unreliable DataChannels (SCTP), backpressure, teardown tests.
- [ ] DTLS fingerprint binding, Minecraft official identity verification and shared UDP RakNet coexistence.
- [ ] Retail Bedrock 1.26.60 handshake tests, load/reconnection tests.
- [ ] Stable Linux native release and Pterodactyl Egg integration.

## Attribution

Original code and API: Copyright (c) **RyzeCompany**, licensed MIT. libdatachannel and its submodules remain third-party software under their respective licenses: [THIRD_PARTY.md](THIRD_PARTY.md).