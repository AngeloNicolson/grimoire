#!/bin/sh
set -e

REPO="AngeloNicolson/grimoire"
INSTALL_DIR="${HOME}/.local/bin"

OS="$(uname -s)"
ARCH="$(uname -m)"

case "${OS}-${ARCH}" in
  Linux-x86_64)  ARTIFACT="grimoire-linux-x86_64" ;;
  Darwin-x86_64) ARTIFACT="grimoire-macos-x86_64" ;;
  Darwin-arm64)  ARTIFACT="grimoire-macos-arm64" ;;
  *) echo "Unsupported platform: ${OS}-${ARCH}" >&2; exit 1 ;;
esac

LATEST=$(curl -sL "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | head -1 | cut -d'"' -f4)
if [ -z "$LATEST" ]; then
  echo "Failed to fetch latest release" >&2
  exit 1
fi

URL="https://github.com/${REPO}/releases/download/${LATEST}/${ARTIFACT}"

echo "Downloading grimoire ${LATEST} for ${OS} ${ARCH}..."
mkdir -p "${INSTALL_DIR}"
curl -sL "$URL" -o "${INSTALL_DIR}/grimoire"
chmod +x "${INSTALL_DIR}/grimoire"
echo "Installed to ${INSTALL_DIR}/grimoire"
echo "Make sure ${INSTALL_DIR} is in your PATH."
