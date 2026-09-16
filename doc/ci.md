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
| `scripts/ci/install-linux-deps.sh` | Installs the OpenGL, X and ALSA headers, plus a compiler, CMake and Ninja where the host lacks them. Exits 0 on a non-Linux host. |
| `scripts/ci/configure.sh` | CMake configure, either explicit or through a preset. |
| `scripts/ci/build.sh` | Builds what `configure.sh` produced. |
| `scripts/ci/test.sh` | `ctest`, excluding the labels `CI_TEST_EXCLUDE_LABELS` names. |
| `scripts/ci/pack.sh` | `scripts/pack.cmake`, or `cpack` in preset mode. |
| `scripts/ci/sanitizer-build-and-test.sh` | A second build and test run under the address and undefined sanitizers. |
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
| `.forgejo/workflows/cmake.yml` | Forgejo, `docker`, `nimlang/nim:2.2.10` | Linux build and pack, default and Ninja generators, on every push and pull request. |
| `.forgejo/workflows/nightly.yml` | Forgejo, same image | The nightly Linux build and pack. |
| `.github/workflows/cmake.yml` | GitHub | The same matrix across Linux, macOS and Windows. |
| `.github/workflows/nightly.yml` | GitHub | The nightly build across the three. |
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

The Forgejo jobs run in `nimlang/nim:2.2.10`. The MCF5407 core compiles its Nim
sources during CMake configure and stops on any version but the one its
`.nim-version` names, so the image pins an exact Nim version — the same version
`setup-nim-action` installs on the GitHub legs. Everything else the build needs
comes from `install-linux-deps.sh` at job time. Advance the image tag together
with `.nim-version` and with the `nim-version:` pins in `.github/workflows/`.

## Remotes

`origin` is Forgejo. The GitHub remote is named `axiomantic` and is a manual
mirror target only — nothing in this repository pushes to it.
