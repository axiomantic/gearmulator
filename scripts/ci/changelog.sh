#!/usr/bin/env bash
# Split doc/changelog.txt into the per-version files the release upload reads.
# The generator is a build product, so this runs after build.sh.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

gen=${CI_CHANGELOG_GENERATOR:-${CI_REPO_ROOT}/bin/tools/changelogGenerator}

ci_log "generating the split changelog"
exec "${gen}" \
	-i "${CI_REPO_ROOT}/doc/changelog.txt" \
	-o "${CI_REPO_ROOT}/doc/changelog_split"
