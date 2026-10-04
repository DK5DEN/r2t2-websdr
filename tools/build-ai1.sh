#!/bin/bash
# Build r2t2sdr for the R2T2 on the build host (default ai1), never on the device itself.
# Copies the working tree (tracked and modified files) there, builds in the armv7 container
# (build/Dockerfile.armv7) and fetches the binary to out/r2t2sdr.
#   BUILD_HOST=deisold@192.168.168.4 tools/build-ai1.sh
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${BUILD_HOST:-deisold@192.168.168.4}
DIR=r2t2-websdr-build

git ls-files -co --exclude-standard | grep -v '^out/' | tar cf - -T - \
  | ssh -o BatchMode=yes "$HOST" "rm -rf $DIR && mkdir -p $DIR && tar xf - -C $DIR"

ssh -o BatchMode=yes "$HOST" bash -s <<EOF
set -euo pipefail
cd $DIR
docker image inspect r2t2-build:armv7 >/dev/null 2>&1 || \
  docker build -q --platform linux/arm/v7 -t r2t2-build:armv7 -f build/Dockerfile.armv7 build >/dev/null
docker run --rm --platform linux/arm/v7 -v "\$PWD":/src r2t2-build:armv7 \
  sh -c 'make -s clean && make -s -j4 2>&1 | grep -v "^\$" || true; test -x r2t2sdr'
# the device has glibc 2.24: refuse anything that needs newer symbols
need=\$(objdump -T r2t2sdr | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
echo "built, needs \$need"
case \$need in GLIBC_2.2[5-9]|GLIBC_2.[3-9]*) echo "too new for the R2T2" >&2; exit 1;; esac
sha256sum r2t2sdr
EOF

mkdir -p out
scp -q -o BatchMode=yes "$HOST:$DIR/r2t2sdr" out/r2t2sdr
sha256sum out/r2t2sdr
