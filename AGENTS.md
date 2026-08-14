# gearmulator (axiomantic fork) — agent instructions

This is a **fork** of `dsp56300/gearmulator`. Upstream is a low-level IC emulator
for classic virtual-analog synthesizers. This fork adds the Nord Modular G2
device under `source/nord/g2/`.

Repository: `axiomantic/gearmulator`. Upstream: `dsp56300/gearmulator`.
Licence: GPL-3.0. Contributions must be GPL-3.0 compatible, with attribution.

**The upstream architecture is documented in `CLAUDE.md`, which is an upstream
file.** Read it for that. Do not restructure it. Its build commands are written
for Visual Studio on Windows and they configure the whole product line; the
section below is the fork's own, and it is the one to use here.

## Build and test

### Narrow — the G2 alone

This is the invocation for "do the `g2Lib` tests still build and pass". It
configures the G2 and no other synth, and no plugin.

**Use the preset. The flags cannot be forgotten because they are not typed.**

```bash
cmake --preset g2
cmake --build --preset g2
ctest --preset g2
```

`cmake --list-presets`, `--list-presets=build` and `--list-presets=test` name
the rest. The wide build is `full` in all three.

**The presets live in `CMakeUserPresets.json`, which `.gitignore` excludes.**
Upstream owns `CMakePresets.json`; a fork that overwrote it would conflict on
every merge, and `CMakeUserPresets.json` is the file CMake reserves for a local
addition. The consequence is that the presets are LOCAL TO THIS MACHINE: a fresh
clone has no `g2` preset and must use the raw form below, or recreate the file
from it.

**`ctest --preset g2` is not `ctest --test-dir <build>/source/nord/g2/g2Lib/test`.**
A test preset takes its build directory from its configure preset and CMake
gives it no field for a subdirectory of one, so the preset selects by test name
instead. The narrow tree still registers `dsp56300`'s and `dsp56kBase`'s tests,
which the narrow build target does not build, so an unfiltered run there would
report them `***Not Run` and count them failed. `^t0_` is every `g2Lib` test
plus `mcf5307`'s `t0_abi_gate_on`, a `cmake -P` script test that `mcf5307`
registers into a consumer tree on purpose and that needs no binary. The
`--test-dir` form below stays authoritative: it is the one whose interaction
with the compile-failure fixture has been exercised here.

#### The raw form, which is what the preset expands to

Keep this. A preset can be wrong, and reading it against the CMake is how you
find out.

```bash
cmake -S . -B <build> \
	-DCMAKE_BUILD_TYPE=Debug \
	-Dgearmulator_BUILD_JUCEPLUGIN=OFF \
	-Dgearmulator_SYNTH_OSIRUS=OFF \
	-Dgearmulator_SYNTH_OSTIRUS=OFF \
	-Dgearmulator_SYNTH_VAVRA=OFF \
	-Dgearmulator_SYNTH_XENIA=OFF \
	-Dgearmulator_SYNTH_NODALRED2X=OFF \
	-Dgearmulator_SYNTH_JE8086=OFF \
	-Dgearmulator_SYNTH_G2=ON \
	-DG2_MCF5307_SOURCE_DIR=<path to the mcf5307 checkout> \
	-DG2_NMG2_TOOLS_SOURCE_DIR=<path to the nmg2-tools checkout>
cmake --build <build> --parallel --target g2_test_executables_all
ctest --test-dir <build>/source/nord/g2/g2Lib/test --no-tests=error --output-on-failure
```

**`--target g2_test_executables_all` is the load-bearing half, not the option
list.** `source/3rdparty` is added unconditionally, so a bare `cmake --build
<build>` builds freetype, Lua, LunaSVG and RmlUi even with every option above
set. The target form builds `g2Lib`, its link libraries and the test
executables, and stops there.

The `-D` list is not a fixed set. `gearmulator_BUILD_JUCEPLUGIN` and
`_BUILD_JUCEPLUGIN_CLAP` are declared in the root `CMakeLists.txt` and the
`gearmulator_SYNTH_*` options in `source/CMakeLists.txt`. Read the current set
out of those two files rather than trusting the list above; a synth added
upstream is on by default and will not appear here.

Passing `-Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF` as well is redundant:
`source/CMakeLists.txt` reads it only inside the `_BUILD_JUCEPLUGIN` branch.

### Full

The wide build stays available and named. Narrow coverage is not a substitute
for it.

```bash
cmake --preset full
cmake --build --preset full
ctest --preset full
```

The raw form:

```bash
cmake -S . -B <build> \
	-DG2_MCF5307_SOURCE_DIR=<path> -DG2_NMG2_TOOLS_SOURCE_DIR=<path>
cmake --build <build> --parallel
ctest --test-dir <build> --no-tests=error --output-on-failure
```

The `full` preset keeps the universal `CMAKE_OSX_ARCHITECTURES` default and sets
no `CMAKE_BUILD_TYPE`, which is what the raw form does. The `g2` preset names one
architecture instead, because `ctest` runs the host slice of a universal binary
and the other slice is compiled and never executed by that run.

**The narrow run cannot see a break it does not configure.** A change to
`base.cmake`, `synthLib`, `baseLib`, `hardwareLib`, the `source/dsp56300` or
`source/mc68k` submodule pins, or `source/juce.cmake` reaches consumers the
narrow tree never builds — this project has already been bitten by a
compiler-flag change that broke a consumer no narrow run touched. Run the full
build for anything below the G2.

### Gotchas of the G2 test run

- **Without `G2_MCF5307_SOURCE_DIR` and `G2_NMG2_TOOLS_SOURCE_DIR` the configure
  clones two repositories from GitHub** at the commits pinned in the root
  `CMakeLists.txt` and in `source/nord/g2/g2Lib/test/tests_board.cmake`. Point
  both at sibling checkouts when they exist. Until a flag points at a sibling, a
  local change in that sibling is not what the tests measure.
- **The G2 tests write into the SOURCE tree.** `t0_clock_guard` plants a scratch
  header under `source/nord/g2/`, runs a nested configure and removes it again.
  Two ctest runs over this repository at the same time collide **whatever build
  directories they use**, because the shared resource is the source tree and not
  the build tree. A collision shows itself as one run that fails for a reason
  which belongs to the other run, and the failed test then passes when you run it
  alone. Read a lone failure of `t0_clock_guard` as a possible collision before
  you read it as a defect.
- `t0_timebase_header` and `t0_clock_guard` both need `git` and read
  `CMAKE_SOURCE_DIR`, so neither works from a tree that is not the repository.
- `t0_extract_matches_python` runs the Python oracle from the `nmg2-tools`
  checkout. It is registered whether or not Python and the oracle were found,
  and it FAILS rather than skipping when either is missing.
- `ctest` in this directory BUILDS: the compile-failure fixture
  `g2_test_executables_build` runs `cmake --build` inside the run. Do not start a
  ctest run against a build tree another process is building.
- **On this host a bare configure of this repository does not generate at all.**
  `xcode-select` points at CommandLineTools while full Xcode is installed, so
  `xcodebuild -version` fails, `xcodeversion.cmake:10` hands an empty string to
  `string(REGEX MATCH)`, and the configure stops with `Configuring incomplete`
  before writing a Makefile. The fix is
  `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`, which the presets
  carry in their environment; a raw `cmake -S . -B <build>` needs it as a prefix.
  Setting `-DXCODE_VERSION=<anything>` also gets past it, by skipping the
  detection rather than by fixing it.
- **A synth added upstream is ON by default and the `g2` preset will not turn it
  off.** The preset names the options that exist today; it cannot name one that
  does not. Read the current set out of the root `CMakeLists.txt` and
  `source/CMakeLists.txt` after any upstream merge and add what appeared.

## The authorship boundary

Everything in this repository is either inherited from upstream or authored
here. The rules below that say "code we authored" mean the second kind, and the
boundary is computable rather than guessed:

```bash
git fetch upstream
git merge-base HEAD upstream/main          # the fork point
git diff --name-only $(git merge-base HEAD upstream/main)...HEAD   # our paths
git log $(git merge-base HEAD upstream/main)..HEAD                 # our commits
```

At the time of writing, the fork point is
`6ff5ef3b19e2156c5ec9bd68fa85abe11b0b77f5`. **Recompute it rather than trusting
that literal** — the fork point moves whenever upstream is merged in. Use
three-dot semantics so that commits merged in FROM upstream are excluded.

Our work concentrates in `source/nord/g2/` (`g2Lib`, `g2JucePlugin`,
`g2TestConsole`), the `.github/workflows/` jobs, and a small number of root
files. Compute the current set with the command above; do not rely on that list.

## Comments

Comments are sparse. Write one only where a reader must otherwise reconstruct a
DECISION. The code says what it does. The comment says why you chose it instead
of the alternative.

Never write these in a comment:

- **A count** — cases, tests, scenarios, targets, symbols, files, or lines. The
  next change makes it wrong, and nothing catches it.
- **A present-tense claim about what the tests cover**, or about what a wrong
  implementation would fail. If coverage matters, assert it in a test. A failing
  test is the only durable statement about coverage.
- **A note about history** ("this used to...", "an earlier version..."). Git
  holds that.
- **An enumeration whose length is the claim.** A stale enumeration is a stale
  count with the number spelled out. Delete the word "four" from "any of those
  four values" and the list above it still says four. It goes wrong by the
  mechanism the word did.
- **A path that does not resolve.** A comment that names a file, a script, a
  test, or a type must name one that exists.

**One exception, and it is the only one.** A number that a mechanism reads and
checks at build time or at test time may stay. The check is then the source of
truth, not the comment, and it fails loudly when the number drifts. A number
that no mechanism reads is a liability.

**The path rule is the one a machine can decide, and that is why it is stated
apart from the others.** Each other rule here needs a reader's judgement about
what a sentence claims. "Every path-shaped token resolves" is a regular
expression and a file test. Write the check. Do not trust a sweep to hold.

**A path that MOVED is corrected. A path that never existed is deleted.** "Delete,
do not correct" has no sensible reading for a path: a moved path has a correct
target, so give it one. A named script that exists nowhere has no target, so the
sentence goes — unless the sentence records a known GAP, and then the gap moves
to a tracked item BEFORE the comment goes. A deleted gap is an unknown gap, which
is worse than the stale comment.

**A date does not rescue a stale claim.** Within a day of churn a date
discriminates nothing.

This fork has already paid for the rule twice. A comment at
`g2Lib/CMakeLists.txt` read "the option defaults to OFF" while a nearby line in
the same file set the option `ON`. A separate block carried a hand-maintained count of the
executables a directory "currently declares" — inside a block whose whole selling
point was that a new target needs no edit there, so the number went stale by the
exact mechanism it documented. **That number was removed and not restated**, and
the loop that enumerates the targets is the honest replacement.

### Scope: this rule applies to code we authored, and NOT to upstream

**Do not sweep, rewrite, or delete comments that came from upstream.** They are
not ours to clean, and editing them creates merge conflicts for no gain. Apply
the rule to:

- files that appear only on our side of the fork point, and
- lines we change in an inherited file.

A comment that describes a line you changed may be repaired — a comment
contradicting the line beside it is a defect OF that line. That permission is
narrow: a comment elsewhere in an inherited file still belongs to upstream.

## Gotchas

- `git grep` skips untracked files. This repository carries large untracked
  build trees (`build-baseline/`, `build-pins/`) that another investigation
  depends on. Use `grep -r`, `rg`, or `git grep --untracked`, name the tool
  beside any "appears nowhere" claim, and **never run `git add -A` here.**
- A build that succeeds is not a check. Verify the artifact a step should have
  produced, not the exit status.
- An invariant with no mechanism is a comment. If a property must hold, make
  something go red when it stops holding, or state the acceptance once at the
  site.

## Related

This fork is one repository of a Nord Modular G2 emulator project. The
cross-repository rules, the implementation plan and the private-submodule
prohibition live in the `nord-modular-emulator` workspace. `source/dsp56300` and
`source/mc68k` are submodules pointing at their own repositories; a change to
either belongs in that repository's fork, not here.
