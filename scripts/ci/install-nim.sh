#!/usr/bin/env bash
# Install the pinned Nim toolchain into CI_NIM_PREFIX and print the PATH entry.
#
# THE VERSION IS NOT WRITTEN IN THIS FILE. mcf5407's .nim-version is the only
# authority: the core compiles its Nim sources during CMake configure and stops
# the configure on any version other than the one that file names, so a second
# spelling of the number here could only ever disagree with it. The
# `nim-version:` inputs under .github/workflows are the one second spelling that
# cannot be removed, because setup-nim-action takes the version as an input and
# has no way to read a file out of a dependency.
#
# The file is read from the pinned commit over the network rather than from the
# fetched tree. FetchContent populates mcf5407 during configure, and configure is
# the step that needs Nim on PATH already, so nothing is on disk to read at the
# moment this runs. G2_MCF5407_SOURCE_DIR short-circuits the fetch: a build
# against a sibling checkout reads that checkout's file instead.
#
# There is no fallback version. A default here would be the drift this script
# exists to remove, wearing the costume of a fix, so an unreadable .nim-version
# stops the run.
#
# This exists because the Forgejo jobs run in a plain distribution image rather
# than through a marketplace action. It is the only script here that a GitHub
# workflow does not call.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

# Overridable so the resolution path can be exercised against a substitute host.
CI_MCF5407_RAW_BASE=${CI_MCF5407_RAW_BASE:-https://raw.githubusercontent.com/axiomantic/mcf5407}

# The pin lives in the root CMakeLists.txt next to the FetchContent declaration
# that uses it, so it is read from there rather than restated.
ci_mcf5407_pin() {
	local cml="${CI_REPO_ROOT}/CMakeLists.txt" pin
	if [ ! -r "${cml}" ]; then
		echo "FAILURE: ${cml} is not readable, so the mcf5407 pin cannot be found." >&2
		return 1
	fi
	pin=$(sed -n 's/^set(G2_MCF5407_GIT_TAG "\([^"]\{1,\}\)".*/\1/p' "${cml}" | head -1)
	if [ -z "${pin}" ]; then
		echo "FAILURE: ${cml} declares no G2_MCF5407_GIT_TAG pin." >&2
		return 1
	fi
	printf '%s\n' "${pin}"
}

ci_nim_version() {
	local raw where src="${G2_MCF5407_SOURCE_DIR:-}" pin
	if [ -n "${src}" ]; then
		where="${src}/.nim-version"
		if ! raw=$(cat -- "${where}" 2>/dev/null); then
			echo "FAILURE: G2_MCF5407_SOURCE_DIR is ${src} but ${where} cannot be read." >&2
			return 1
		fi
	else
		pin=$(ci_mcf5407_pin) || return 1
		where="${CI_MCF5407_RAW_BASE}/${pin}/.nim-version"
		if ! raw=$(curl -fsSL "${where}"); then
			echo "FAILURE: cannot read ${where}." >&2
			echo "That file names the Nim version this build requires and this script has no" \
				"default to use instead." >&2
			return 1
		fi
	fi
	raw=${raw//[[:space:]]/}
	# An empty file and an error page served with a 200 both reach here looking
	# like a successful read, and a range would install the wrong compiler
	# silently. Only an exact three-part version can be handed to the installer.
	if [[ ! ${raw} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
		echo "FAILURE: ${where} does not hold an exact Nim version. It holds '${raw}'." >&2
		return 1
	fi
	ci_log "Nim ${raw} required by ${where}"
	printf '%s\n' "${raw}"
}

if [ -z "${CI_NIM_VERSION:-}" ]; then
	CI_NIM_VERSION=$(ci_nim_version) || exit 1
fi
# Outside the workspace on purpose: under it, the compiler tree becomes a subtree
# of the checkout that the build's recursive source globs would walk.
CI_NIM_PREFIX=${CI_NIM_PREFIX:-/opt/nim-${CI_NIM_VERSION}}

if command -v nim >/dev/null 2>&1 &&
   [ "$(nim --version | head -1 | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -1)" = "${CI_NIM_VERSION}" ]; then
	ci_log "Nim ${CI_NIM_VERSION} is already on PATH"
	exit 0
fi

case "$(uname -m)" in
	x86_64|amd64) arch=x64 ;;
	*)
		echo "FAILURE: nim-lang.org publishes Linux binaries for x86_64 only," \
			"and this host is $(uname -m)." >&2
		echo "Install Nim ${CI_NIM_VERSION} by another route and put it on PATH." >&2
		exit 1
		;;
esac

url="https://nim-lang.org/download/nim-${CI_NIM_VERSION}-linux_${arch}.tar.xz"
ci_log "installing Nim ${CI_NIM_VERSION} into ${CI_NIM_PREFIX}"
mkdir -p "${CI_NIM_PREFIX}"
curl -fsSL "${url}" | tar -xJ -C "${CI_NIM_PREFIX}" --strip-components=1

"${CI_NIM_PREFIX}/bin/nim" --version | head -1
have=$("${CI_NIM_PREFIX}/bin/nim" --version | head -1 | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [ "${have}" != "${CI_NIM_VERSION}" ]; then
	echo "FAILURE: asked for Nim ${CI_NIM_VERSION}, got ${have}." >&2
	exit 1
fi

# The caller puts this on PATH. A workflow appends it to $GITHUB_PATH; a shell
# can eval the line this prints.
echo "${CI_NIM_PREFIX}/bin"
