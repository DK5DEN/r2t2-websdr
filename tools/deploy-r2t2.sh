#!/bin/bash
# Install the binary built by tools/build-ai1.sh plus www/ and dist/ on the R2T2 and restart
# the service. Nothing is compiled on the device.
#   R2T2_HOST=dk5den@44.149.24.154 R2T2_KEY=~/.ssh/r2t2 R2T2_SUDO=... tools/deploy-r2t2.sh
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${R2T2_HOST:-dk5den@44.149.24.154}
KEY=${R2T2_KEY:-$HOME/.ssh/r2t2}
SUDO_PW=${R2T2_SUDO:?set R2T2_SUDO to the sudo password of the R2T2 account}
test -x out/r2t2sdr || { echo "out/r2t2sdr missing, run tools/build-ai1.sh" >&2; exit 1; }
SUM=$(sha256sum out/r2t2sdr | cut -c1-64)

tar cf - Makefile out/r2t2sdr www dist \
  | ssh -o BatchMode=yes -i "$KEY" "$HOST" bash -c "'
set -e
rm -rf ~/r2t2-websdr-deploy && mkdir ~/r2t2-websdr-deploy && cd ~/r2t2-websdr-deploy && tar xf -
test \"\$(sha256sum out/r2t2sdr | cut -c1-64)\" = $SUM || { echo checksum mismatch >&2; exit 1; }
ldd out/r2t2sdr | grep -q \"not found\" && { ldd out/r2t2sdr; exit 1; }
echo $SUDO_PW | sudo -S -p \"\" make -s install-files BIN=out/r2t2sdr
echo $SUDO_PW | sudo -S -p \"\" systemctl restart r2t2sdr
sleep 2; systemctl is-active r2t2sdr
'"
