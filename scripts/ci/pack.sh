#!/usr/bin/env bash
# Package the built tree. scripts/pack.cmake reads the build tree it is invoked
# from, so the working directory is part of its contract.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

if [ -n "${CI_PRESET}" ]; then
	ci_log "cpack with preset ${CI_PRESET}"
	exec cpack --preset "${CI_PRESET}"
fi

ci_log "pack ${CI_BUILD_DIR}"
cd "${CI_BUILD_DIR}"
exec cmake -P "${CI_REPO_ROOT}/scripts/pack.cmake"
