# Continuous integration

This fork builds on two systems. A self-hosted Forgejo instance at
`git.axiomantic.dev` is the primary one and holds `origin`. GitHub keeps only
the legs Forgejo has no hardware for.

Neither system carries build commands. The build, pack, test and changelog logic
lives in `scripts/ci/`, and both sets of workflow files check out the tree, set
up a toolchain, and call those scripts. A human runs the same scripts from a
shell.

## The shared scripts

| Script | What it does |
|--|--|
| `scripts/ci/lib.sh` | Sourced by the rest. Defines every `CI_*` variable and its default. |
| `scripts/ci/install-linux-deps.sh` | Installs the OpenGL, X and ALSA headers, plus a compiler, Clang, CMake and Ninja where the host lacks them. Exits 0 on a non-Linux host. |
| `scripts/ci/install-nim.sh` | Installs the pinned Nim toolchain and prints the directory to put on PATH. Forgejo only: the GitHub legs use `setup-nim-action`. |
| `scripts/ci/configure.sh` | CMake configure, either explicit or through a preset. |
| `scripts/ci/build.sh` | Builds what `configure.sh` produced. |
| `scripts/ci/test.sh` | `ctest`, excluding the labels `CI_TEST_EXCLUDE_LABELS` names. |
| `scripts/ci/pack.sh` | `scripts/pack.cmake`, or `cpack` in preset mode. |
| `scripts/ci/sanitizer-build-and-test.sh` | A second build and test run under the address and undefined sanitizers, in a build tree of its own. Reads `CI_CMAKE_ARGS`; sets the build type and the sanitizer flags itself. |
| `scripts/ci/require-cmake-version.sh` | Fails below CMake 3.20, the floor for `ctest --no-tests=error`. |
| `scripts/ci/changelog.sh` | Splits `doc/changelog.txt` for the release upload. |

### Running them by hand

```sh
./scripts/ci/configure.sh
./scripts/ci/build.sh
./scripts/ci/test.sh
```

Every knob is an environment variable with a default, so nothing has to be set:

| Variable | Default | Meaning |
|--|--|--|
| `CI_BUILD_DIR` | `<repo>/build` | Build tree. |
| `CI_BUILD_TYPE` | `Release` | `CMAKE_BUILD_TYPE`. |
| `CI_CMAKE_GENERATOR` | empty | Empty means the platform default; `Ninja` selects Ninja. |
| `CI_CMAKE_ARGS` | empty | Extra `-D` flags. Word-split, so no value may contain a space. |
| `CI_PRESET` | empty | When set, configure/build/pack go through that CMake preset. |
| `CI_PARALLEL` | host CPU count | Build parallelism. |
| `CI_TEST_EXCLUDE_LABELS` | `IntegrationTest\|PluginTest` | A CTest **label** regex. |

`IntegrationTest` is excluded because a test carrying that label fetches its data
through rclone and aborts when the credential is absent. `PluginTest` is excluded
because those tests load a built plugin into a host, which exercises the packaged
product rather than this fork's own code. Removing a label from the regex is how
to turn its tests on.

## Which workflow covers what

| File | Runs on | Covers |
|--|--|--|
| `.forgejo/workflows/cmake.yml` | Forgejo, `docker`, `ubuntu:24.04` | Linux build, default and Ninja generators, plus the sanitizer run in a job of its own, on every push and pull request. It does not pack; the file says why. |
| `.forgejo/workflows/nightly.yml` | Forgejo, same image | The nightly Linux build. It does not pack; the file says why. |
| `.github/workflows/cmake.yml` | GitHub | The same matrix across Linux, macOS and Windows. |
| `.github/workflows/nightly.yml` | GitHub | The nightly build across Linux, macOS and Windows. |
| `.github/workflows/ci.yml` | GitHub | Build and test, plus the sanitizer run. `main` only. |
| `.github/workflows/release.yml` | GitHub | Release build and the GitHub Release upload. |
| `.github/workflows/momus.yml` | GitHub | The review bot. |

## GitHub-only legs, and why

Private CI is one self-hosted Linux runner backed by Podman. There is no
Windows or macOS runner on it and none is planned.

- **macOS and Windows build legs** of `cmake.yml`, `nightly.yml` and
  `release.yml`. No self-hosted host can run them.
- **`release.yml` as a whole.** It publishes a GitHub Release. The build steps
  still call the shared scripts so the release build cannot drift from the CI
  build.
- **`momus.yml`.** It calls a reusable workflow hosted in a GitHub organisation
  (`axiomantic/.github`). Forgejo Actions cannot resolve a GitHub-hosted
  reusable workflow, and the bot it drives authenticates as a GitHub App against
  GitHub pull requests.

## The container image

The Forgejo jobs run in `ubuntu:24.04`, which is what `ubuntu-latest` resolves to
on the GitHub legs. The match is load-bearing rather than cosmetic: this tree does
not compile under the GCC 12 that Debian bookworm ships, because
`source/nord/g2/g2Lib/flash.h` reaches for `size_t` through an include that only
GCC 13 and newer provide. Pinning the same distribution as the GitHub Linux leg is
what keeps the two legs comparable.

Everything the build needs goes on at job time:

- `install-linux-deps.sh` adds the OpenGL, X and ALSA headers, `build-essential`,
  Ninja and Clang. Clang is there for the mcf5407 ABI gate, which reads a C syntax
  tree that only Clang prints and refuses to configure without it. A hosted Ubuntu
  runner already ships Clang, which is why only a bare container needs it named.
- `install-nim.sh` adds Nim at the version it pins. The MCF5407 core compiles its
  Nim sources during CMake configure and stops on any version but the one its
  `.nim-version` names, so it is an exact version. Advance it together with the
  `nim-version:` pins in `.github/workflows/`.
- CMake comes from the distribution. The tree's own floor is 3.26, set by
  the mcf5407 dependency; `require-cmake-version.sh` enforces a separate, lower
  3.20 floor, which is what `ctest --no-tests=error` needs.

## Remotes

`origin` is Forgejo. The GitHub remote is named `axiomantic` and is a manual
mirror target only — nothing in this repository pushes to it.
