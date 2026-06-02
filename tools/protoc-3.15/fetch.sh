#!/usr/bin/env bash
# Pull a pinned protoc 3.15.8 release into this directory.
# Generated files are version-controlled, so this only runs when
# adding or regenerating .proto messages.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
version="${PROTOC_VERSION:-3.15.8}"
url="https://github.com/protocolbuffers/protobuf/releases/download/v${version}/protoc-${version}-linux-x86_64.zip"

if [ -x "${here}/bin/protoc" ] && \
   "${here}/bin/protoc" --version | grep -qx "libprotoc ${version}"; then
    echo "protoc ${version} already installed at ${here}/bin/protoc"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "Downloading protoc ${version} ..."
curl -fsSL -o "${tmp}/protoc.zip" "${url}"
unzip -q -o "${tmp}/protoc.zip" -d "${here}"

chmod +x "${here}/bin/protoc"
"${here}/bin/protoc" --version
