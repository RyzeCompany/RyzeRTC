#!/usr/bin/env bash
# Installer called by the RyzeMP Egg AFTER a tagged first-party release exists.
set -euo pipefail

if (( $# != 4 )); then
  echo 'usage: install-egg-runtime.sh /absolute/php v0.1.0-dev.1 SHA256 /server/.ryzertc-runtime' >&2
  exit 64
fi
PHP_BIN="$1"
RELEASE_TAG="$2"
EXPECTED_SHA="$3"
DEST="$4"
if [[ ! "$RELEASE_TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9.]+)?$ ]] ||
   [[ ! "$EXPECTED_SHA" =~ ^[0-9a-fA-F]{64}$ ]] ||
   [[ "$PHP_BIN" != /* ]] || [[ "$DEST" != /* ]] ||
   [[ "${DEST##*/}" != ".ryzertc-runtime" ]] || [[ ! -x "$PHP_BIN" ]]; then
  echo 'invalid release tag, pinned SHA-256, destination or PHP binary' >&2
  exit 64
fi
for bin in curl unzip sha256sum mktemp; do
  command -v "$bin" >/dev/null || { echo "missing: $bin" >&2; exit 69; }
done
if ! "$PHP_BIN" -n -r 'exit(PHP_VERSION_ID >= 80400 && PHP_VERSION_ID < 80500 && PHP_ZTS && PHP_INT_SIZE === 8 ? 0 : 1);'; then
  echo 'RyzeRTC requires matching PHP 8.4 ZTS 64-bit runtime' >&2
  exit 78
fi
umask 077
PARENT="$(dirname "$DEST")"
mkdir -p "$PARENT"
TEMP="$(mktemp -d "$PARENT/.ryzertc-stage.XXXXXX")"
trap 'rm -rf -- "$TEMP"' EXIT
URL="https://github.com/RyzeCompany/RyzeRTC/releases/download/${RELEASE_TAG}/ryzertc-linux-x86_64-php84-zts.zip"
curl --fail --location --silent --show-error --retry 3 --connect-timeout 20 --max-time 240 "$URL" -o "$TEMP/bundle.zip"
printf "%s  %s\n" "$EXPECTED_SHA" "$TEMP/bundle.zip" | sha256sum -c - --status || {
  echo 'RyzeRTC release checksum mismatch' >&2
  exit 65
}
mkdir -p "$TEMP/extract"
unzip -q "$TEMP/bundle.zip" -d "$TEMP/extract"
cd "$TEMP/extract"
test -f ryzertc.so && test -f SHA256SUMS.txt && test -d lib
sha256sum -c --status SHA256SUMS.txt
LD_LIBRARY_PATH="$TEMP/extract/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$PHP_BIN" -n -d "extension=$TEMP/extract/ryzertc.so" \
  -r 'if (!extension_loaded("ryzertc") || !class_exists("RyzeRTC\\Runtime") || RyzeRTC\Runtime::apiVersion() < 2) exit(1); echo "RyzeRTC ", RyzeRTC\Runtime::version(), " validated\n";'
cd "$PARENT"
# Replace only the native runtime: never mutate plugins, PHAR, or world directories.
rm -rf -- "${DEST}.previous"
if [[ -d "$DEST" ]]; then mv -- "$DEST" "${DEST}.previous"; fi
if ! mv -- "$TEMP/extract" "$DEST"; then
  if [[ -d "${DEST}.previous" ]]; then mv -- "${DEST}.previous" "$DEST"; fi
  exit 1
fi
rm -rf -- "${DEST}.previous"
echo "RyzeRTC installed to $DEST"
