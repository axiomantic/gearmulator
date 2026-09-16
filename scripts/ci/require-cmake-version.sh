#!/usr/bin/env bash
# CTest 3.20 is the floor because `--no-tests=error` is a CTest flag and the
# earlier releases do not carry it. Without that flag a filtered run that selects
# nothing exits 0.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

cmake --version
ctest --version

have=$(cmake --version | head -1 | grep -Eo '[0-9]+\.[0-9]+' | head -1)
major=${have%%.*}
minor=${have##*.}
if [ "${major}" -lt 3 ] || { [ "${major}" -eq 3 ] && [ "${minor}" -lt 20 ]; }; then
	echo "FAILURE: CMake ${have} is older than the 3.20 floor." >&2
	exit 1
fi
ci_log "CMake ${have} meets the 3.20 floor"
