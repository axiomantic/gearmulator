#!/usr/bin/env bash
# Build whatever configure.sh produced.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

if [ -n "${CI_PRESET}" ]; then
	ci_log "build with preset ${CI_PRESET} (parallel ${CI_PARALLEL})"
	exec cmake --build --preset "${CI_PRESET}" --parallel "${CI_PARALLEL}"
fi

ci_log "build ${CI_BUILD_DIR} (${CI_BUILD_TYPE}, parallel ${CI_PARALLEL})"
exec cmake --build "${CI_BUILD_DIR}" --config "${CI_BUILD_TYPE}" --parallel "${CI_PARALLEL}"
