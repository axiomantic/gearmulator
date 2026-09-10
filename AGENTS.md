# Agents

Instructions for AI coding agents working on this repository.

This is a **fork** of `dsp56300/gearmulator`. Upstream is a low-level IC
emulator for classic virtual-analog synthesizers. This fork adds the Nord
Modular G2 device under `source/nord/g2/`.

Repository: `axiomantic/gearmulator`. Upstream: `dsp56300/gearmulator`.
Licence: GPL-3.0. Contributions must be GPL-3.0 compatible, with attribution.
Code here cannot move into `mcf5307` or `nmg2-tools` unless this project wrote
it.

The G2 work is `source/nord/g2`: `g2Lib`, `g2JucePlugin`, `g2TestConsole`. It
is not on the default branch. It is on stacked branches.

This repository is part of the Nord Modular G2 emulator project. The work and
the execution ceremony live in the project roadmap, in the `nmg2-artifacts`
repository. Read it before you start a task. This file states the rules that
apply while you write code here.

**The upstream architecture is documented in `CLAUDE.md`, which is an upstream
file.** Read it for that. Do not restructure it. Its build commands are written
for Visual Studio on Windows and they configure the whole product line; the
build section below is the fork's own, and it is the one to use here.

## This project does not use the develop ceremony

**Do not invoke the `develop` skill here. Do not use a planning mode, a phase
ladder, an approval gate, or a quality-gate stack.** A general instruction to
route substantive changes through that ceremony does not apply to this project.
The operator has exempted it deliberately.

The process is the one the project roadmap describes. That is the whole of it.

**Do not build a checker.** This project spent most of its life on its own
tooling: a plan linter, a freshness tool, a pin mechanism, a check-target
generator, a payload guard with a file register, and a set of boundary lints.
All of them are deleted. They cost more than they caught, and one of them had
been silently switching off its neighbours for an unknown length of time.

So: no new lint, no new gate, no new register, no new audit script, and no test
that asserts on the shape of the repository rather than on the behaviour of the
code. Standard formatting and syntax tools stay — a formatter and a compiler are
not what is meant here. If you believe something needs checking, say so and let
the operator decide. Do not write it.

## Build and test

Formatting follows `source/.clang-format`: tabs, tab size 4, 120 columns.

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
report them `***Not Run` and count them failed. `^t0_` selects the `g2Lib` tests
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
	-Dgearmulator_SYNTH_G2=ON
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
out of those two files rather than trusting the list above. **A synth added
upstream is ON by default and the `g2` preset will not turn it off** — the
preset names the options that exist today; it cannot name one that does not.
Re-read both files after any upstream merge and add what appeared.

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
cmake -S . -B <build>
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

### The source-dir overrides, and why the default is the pin

`G2_MCF5307_SOURCE_DIR` and `G2_NMG2_TOOLS_SOURCE_DIR` replace a fetch of a
pinned commit with a sibling checkout. **Neither belongs in a build you will
report, record, or act on.** Build against the pin: leave both unset, and the
configure fetches exactly what the tree declares.

**An override substitutes whatever the sibling happens to be sitting on, which
is usually neither the pin nor the commit you meant to test.** A sibling
checkout is left wherever its last piece of work left it. Nothing about the
build says which tree it read, so every result from it is a statement about a
third tree wearing the authority of a statement about the pin. Both shapes have
been paid for here in the same day:

- A checkout a couple of commits past the pin produced findings about the
  MCF5307 ABI that described neither the pin nor the bump target. All of them
  were retracted.
- A checkout on a branch that carries no Nord tooling made
  `t0_extract_matches_python` fail with `ModuleNotFoundError: No module named
  'nmg2_tools.container'`. That was reported up as a cross-repo breakage caused
  by a restructure. It was a local misconfiguration: the same test passes
  against the pin, and flipping only that one variable in one build tree flips
  the verdict.

The second shape is the expensive one, because the error is plausible. A
`ModuleNotFoundError` naming a module that really was moved reads as an upstream
deletion, and nothing in it points at the flag that caused it.

**When an override is appropriate:** iterating on this repository and a
dependency together, where the point of the build is to exercise an
uncommitted or unpinned change in the sibling. That is a real workflow and the
flags exist for it.

**When it is not:** anything whose output leaves your terminal. A test verdict,
a bisect, a bug report, a PR comment, a decision about whether to advance a pin.
Re-run those against the pin before believing them.

**A result obtained under an override is labelled, not reported bare.** Say
which override was set and what the pointed-at tree's HEAD was. A result whose
tree is unstated is not a measurement.

Where the guard is present, the configure enforces both rules rather than
trusting this section. It is on the `g2/pin-override-guard` branch; check the
branch you are on before you rely on it.

- `G2_NMG2_TOOLS_SOURCE_DIR` at a tree with no `nmg2_tools/container.py` is a
  FATAL configure error naming the likely cause.
- `G2_MCF5307_SOURCE_DIR` at a tree whose HEAD is not `G2_MCF5307_GIT_TAG`, or
  whose working tree is dirty, is a FATAL configure error until you pass
  `-DG2_MCF5307_SOURCE_DIR_IS_NOT_THE_PIN=ON`. That flag is the label: after it,
  the configure warns on every run that the build does not describe the pin.

Both fire only when the override is set. The default path reaches neither.

### Gotchas of the G2 test run

- **Always pass `--no-tests=error` to ctest.** Without it, ctest exits 0 when the
  filter matches no test. A pass and an empty run then look the same. This
  project has a repository where that exact false green is live today.
- **Without `G2_MCF5307_SOURCE_DIR` and `G2_NMG2_TOOLS_SOURCE_DIR` the configure
  clones two repositories from GitHub** at the commits pinned in the root
  `CMakeLists.txt` and in `source/nord/g2/g2Lib/test/tests_board.cmake`. **That
  is the default and it is what you want.** Leave both unset unless you have a
  reason named in **The source-dir overrides, and why the default is the pin**
  above.
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
- **Xcode version detection falls over where `xcode-select` points at
  CommandLineTools**, which is this host's default state. `xcodebuild -version`
  then writes only to stderr, which `xcodeversion.cmake` discards through
  `ERROR_FILE /dev/null`, so its `OUTPUT_VARIABLE` is empty. What happens next
  is decided by one pair of characters at `xcodeversion.cmake:10`. Quoted, the
  empty value reaches `string(REGEX MATCH)` as an empty argument, that file's own
  warn-and-fallback branch runs, `XCODE_VERSION` becomes the 99 sentinel meaning
  "newest", and the configure completes. Unquoted, the argument vanishes instead,
  `string(REGEX MATCH)` is called with too few arguments, and CMake stops the
  configure. **Read the line before you diagnose a failure there.** Two ways
  past it, and neither is a repair: export
  `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` to get the real
  version detected rather than the sentinel — the presets carry it in their
  environment — or pass `-DXCODE_VERSION=<value>` to pin it, which skips
  detection rather than performing it. Neither reaches the case that matters: a
  ctest run reconfigures from inside its own build fixture and that fixture does
  not inherit `DEVELOPER_DIR`, so the run reports every test `Not Run`.
  `docs/divergence.md` carries the row for this file and the condition that
  retires it.

## Tests

Write the failing test first. Confirm that it fails for the intended reason.

A test must consume real values. A test that checks only exit status,
non-emptiness, or truthiness proves nothing. Before you call a test done, plant
a fault and confirm that the test goes red.

An invariant with no mechanism is a comment. If a property must hold, make
something go red when it stops holding, or state the acceptance once at the
site.

## Verify the artifact, not the signal

A step that can do nothing reports success in the same way as a step that
worked. When a step writes a file, regenerates code, or targets a path you did
not name, look at what it produced. Do not read the exit code and stop. A build
that succeeds is not a check.

Count with a command. Never estimate a number, and never recall one.

**Check the outcome, not a proxy for it.** Before a risky change, it is tempting
to verify a precondition that stands in for the thing you care about. Verify the
thing itself, afterwards. A guard that a deleted tool would not return was
written as "these three files are absent from the branch" — they were absent, and
the tool came back anyway through a workflow that referenced it. The question was
never "are these files here" but "does the tool exist when I am done".

State what you ran next to the result. A rule stated more broadly than what you
tested is false in a way the test will not show you.

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

**Recompute the fork point rather than trusting a literal** — it moves whenever
upstream is merged in. Use three-dot semantics so that commits merged in FROM
upstream are excluded.

Our work concentrates in `source/nord/g2/` (`g2Lib`, `g2JucePlugin`,
`g2TestConsole`), the `.github/workflows/` jobs, and a small number of root
files. Compute the current set with the command above; do not rely on that list.

## Comments and docstrings

Comments are sparse. Write one only where a reader must otherwise reconstruct a
DECISION. The code says what it does. The comment says why you chose it instead
of the alternative.

**Never write these in a comment, a docstring, or a test name:**

- **A plan-task ID or a design-document pointer.** `PLG-nn`, `BRD-nn`, `SCH-nn`,
  `CHN-nn`, `CPU-nn`, `INT-nn`, `PROTO-nn`, `DSP-nn`, `USB-nn`, `PERF-nn`,
  `W3-nnnn`, `M-x`, a section-and-row citation, "task ...", "plan section ...",
  "design section ... rule ...", "step 2 of ...". They point into a ledger that
  lives in another repository, and they renumber. This has no exception: a
  citation of this project's own specification is forbidden in the same way as a
  citation of a task ledger. **State the FACT; drop the citation.** This is where
  the noise concentrates in this fork: the spellings above are common in
  `source/nord/g2/`, so expect to cut them in bulk.
- **A roster or a status list.** "the stub bodies below are not finished —
  `getChannelCountIn` has landed". A roster rots by construction: every task
  that lands makes it wrong until someone edits it. A stub's own body says it
  is a stub. A list of unfinished work goes with it.
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
- **A claim about the rest of the tree.** A comment describes the code beside
  it. Do not write what else includes this header, what its only caller is,
  which target links it next, or what another file does not name. The build
  graph and the include graph answer those and stay right; a sentence about them
  is derivable, goes stale the moment another target moves, and records no
  decision.

**These are protected. Never remove them:**

- Datasheet and hardware-manual citations.
- Hazard banners.
- Comments inherited from upstream or vendored code.
- A number that a mechanism reads and checks at build time or at test time. The
  check is then the source of truth, not the comment, and it fails loudly when
  the number drifts. A number that no mechanism reads is a liability. This is the
  only exception to the count rule, and it is protected where one already exists
  and never created new.

**The path rule is the one a machine could decide, and that is why it is stated
apart from the others.** Each other rule here needs a reader's judgement about
what a sentence claims. "Every path-shaped token resolves" is a regular
expression and a file test. Do not write that checker — see **This project does
not use the develop ceremony** above; raise it with the operator instead. Until
one exists, the rule is yours to hold, and a sweep is not evidence that it holds.

**A path that MOVED is corrected. A path that never existed is deleted.** "Delete,
do not correct" has no sensible reading for a path: a moved path has a correct
target, so give it one. A named script that exists nowhere has no target, so the
sentence goes — unless the sentence records a known GAP, and then the gap moves
to a tracked item BEFORE the comment goes. A deleted gap is an unknown gap, which
is worse than the stale comment.

**A cross-reference that helps a reader NAVIGATE still stands.** "The frame
layout is also computed in `g2Lib/frame.cpp`" earns its place and stays, provided
it asserts no exclusivity and no sequence. What goes is ONLY, FIRST, NEXT, and
"does not name": those are the falsifiable forms, and that difference is the
whole of the rule.

**A measured fact earns its place only while it stays measured.** A comment
about a FORMAT or about the hardware — a register address, a chip-select map, a
byte offset in firmware, a field width, an endianness — is safe, because the
thing it describes cannot change under it. A comment about OUR OWN
implementation choice rots the moment the implementation changes, and it keeps
a comment's authority while it does. **When you change behaviour, the comment
above it is part of the change.**

**A mixed block is split, not judged whole.** One block often holds a
restatement of the code AND a real why — a hazard, an ordering that is
load-bearing, a deliberate duplication. Edit inside the block: cut the
restatement and the task IDs, keep the why. Do not delete a whole block because
part of it is noise, and do not keep a whole block because part of it is real.

**Sweeps are permitted.** You can rewrite comments across many files in one
change. A sweep changes no behaviour. If you cannot say that with certainty
about a file, split the change and handle that file on its own.

**Prove a prose pass changed no code, mechanically.** For C++, compare the
preprocessed output of the file before and after, or the compiled object. At
minimum, show a diff in which every changed line is a comment line. Do not
assert "comments only" by eye.

**A date does not rescue a stale claim.** Within a day of churn a date
discriminates nothing.

This fork has already paid for the rule. A comment at
`g2Lib/CMakeLists.txt` read "the option defaults to OFF" while a nearby line in
the same file set the option `ON`. A separate block carried a hand-maintained
count of the executables a directory "currently declares" — inside a block whose
whole selling point was that a new target needs no edit there, so the number went
stale by the exact mechanism it documented. **That number was removed and not
restated**, and the loop that enumerates the targets is the honest replacement.

### Scope: this rule applies to code we authored, and NOT to upstream

**Do not sweep, rewrite, or delete comments that came from upstream.** They are
not ours to clean, and editing them creates merge conflicts for no gain. Apply
the rule to:

- files that appear only on our side of the fork point, and
- lines we change in an inherited file.

A comment that describes a line you changed may be repaired — a comment
contradicting the line beside it is a defect OF that line. That permission is
narrow: a comment elsewhere in an inherited file still belongs to upstream.

## Searching

`git grep` does not see untracked files, and this repository carries large
untracked build trees (`build-baseline/`, `build-pins/`) that another
investigation depends on. An empty result and an unsearched file look the same.
Use `grep -r`, `rg`, or `git grep --untracked`, name the tool beside any claim
that something appears nowhere, and **never run `git add -A` here.**

Quote every argument that contains a glob character. An unquoted `?` or `*` is
eaten by the shell, the command never runs, and the empty output reads exactly
like a measured absence.

**Run a positive control before you trust a zero.** Search for something you
know is there, in the same command shape, and confirm it is found. Every way a
search can lie returns an empty result and exit 0: a case difference, a quoted
glob the shell ate, a bare clone with no remote refs, a tool that skips
untracked files. None of them reports an error. The control is the only thing
that separates "absent" from "not looked at".

A path that is missing from a default branch is not missing from the repository.
Two repositories here hold their product work on stacked branches, and this is
one of them. Check the branch before you report a file as absent.

## Git

Never push to a default branch without permission. Never force push without
stating what it discards first.

Never run a tree-wide git operation in a checkout you share with anyone:
`git stash`, `git checkout .`, `git restore .`, `git clean -fd`,
`git reset --hard`. To compare against a commit, read it with `git show`.

Never put an issue number in a commit message, a pull request title, or a pull
request body. It notifies every subscriber.- A `Co-authored-by` trailer is permitted in this fork. Upstream's convention
  omits it; this fork keeps it, so a rewrite of history to strip it is not wanted.

Back up a commit before you destroy it. Push it to `refs/preserve/<name>`, then
read the ref back and confirm the sha before you delete anything.

**Push a backup to `refs/preserve/`, never to `refs/heads/preserve/`.** The
second one is a branch. A branch fires every push-triggered workflow, and a
branch cleanup deletes it. `refs/preserve/` sits outside `refs/heads` and
`refs/tags`, so it does neither.

Brace the variable in a refspec: write `"${SHA}":"${REF}"`. An unbraced
`$SHA:refs/...` is silently corrupted by the shell's history modifier, and the
push either fails or writes the wrong ref.

Work in a clone you created yourself. Never delete a path you did not create.

## Pull requests

Open every pull request against this project's own fork. **Never open one
against an upstream repository.** Confirm the base repository after you create
it; the command line tool defaults a fork's base to the upstream project.

Some repositories in this project are forks of other people's work. Their
default branch is this project's working branch. It carries the upstream project
plus the tooling this project needs to work on it, and it is never submitted. A
pull request in one of those forks is a FIRST DRAFT of what this project may one
day offer that project's maintainers. It is based on the FORK's default branch,
so it inherits this project's tooling while the work is in progress; a
submission is eventually rebased onto the UPSTREAM default branch, so none of
that tooling reaches it.

So sort every change in a fork by kind.

- **Work a maintainer would want.** A feature, a bug fix in their code, or a
  build fix that helps anyone who compiles the project. This goes in a pull
  request. A build or continuous-integration change belongs here whenever it
  fixes something real for every builder, not only for this project.
- **Tooling for operating the fork.** The review bot and these instructions.
  This goes on the fork's default branch and is never submitted.
- **A correction to this project's own unsubmitted work.** A comment sweep, a
  rubric pass, a fix to something written in a draft. Squash it into the pull
  request it corrects. Never open a pull request that repairs a change nobody
  outside this project has seen.
- **Nothing else.** A change that matches none of the first three does not
  belong in the fork.

The repositories this project owns outright have no upstream. Their pull
requests only need to be reviewable. Keep them coarse.

## Debugging the MCF5307 with GDB

**USE THE DEBUGGER EARLY AND OFTEN. This is a standing rule for every agent,
human or dispatched subagent, and it applies BEFORE any other technique.** For
any question about the running firmware — is this routine reached, who writes
this address, what do the registers hold, when does this flag change — the GDB
session is the FIRST tool to reach for, not the last. The failure mode this
rule exists to prevent is measured, not hypothetical: this project has burned
days building one-off probe scaffolds (fetch histograms, ring snapshots,
register captures) inside `t1_patch_running.cpp` to answer questions a single
GDB breakpoint or watchpoint answers in one run. **Before adding any new
instrument to a test file, before guessing from static disassembly, before
reasoning from a histogram: arm a breakpoint.** Static disassembly enumerates
candidate code paths; only the debugger tells you which one the machine
actually took. Reserve scaffolds for questions the stub genuinely cannot reach
(DSP-side state, whole-run statistics, cross-quantum aggregates) — and note the
known-positive discipline below, which applies to GDB runs exactly as it does
to every other instrument here.

**When a subagent is dispatched on firmware-tracing work, its dispatch prompt
must say this explicitly**: "Use `g2TestConsole --gdb` for runtime questions;
breakpoints and watchpoints first, scaffolds only for what the stub cannot
reach. For static structure questions (what does this routine do, who calls
it), use the Ghidra headless decompiler documented in
`nmg2-artifacts/AGENTS.md` — decompile first to plan where to break, then break
to confirm. Neither alone is evidence." An agent that inherits only this file's
later paragraphs tends to default to disassembly and print-probes.

`g2TestConsole --gdb <port>` places the same machine `--boot` places — the OS
image at `0x30000400`, the vector table, the reset — and then serves a GDB
remote session on `127.0.0.1:<port>`, blocking until a debugger attaches. A port
of `0` asks for a free one and the program prints the port it bound. Nothing
advances the machine until the debugger says so.

```bash
NMG2_ARTIFACTS=<artifacts> g2TestConsole --gdb 3333
m68k-elf-gdb -ex 'target remote 127.0.0.1:3333'
```

`lldb` speaks the same protocol: `gdb-remote 127.0.0.1:3333`.

**Which questions this answers faster than a print statement.** Each of these
was answered at least once by adding a `std::cout`, rebuilding, reading one line
and reverting, at roughly ninety seconds a cycle:

| Question | Command |
|---|---|
| Who writes this address? | `watch *0x30057040` — the stop reply names the address that was written |
| Who reads this address? | `rwatch *0x30057040` |
| Is this routine ever reached? | `break *0x30056fea` then `continue` |
| What do the registers hold at a fault? | `continue` to the fault, then `info registers` |
| What does this memory region contain right now? | `x/32xb 0x302a0db8` |

**The session drives the WHOLE machine.** `--gdb` builds the same `Scheduler`
`--boot` builds and hands it to the stub, so a `continue` turns whole quanta in
the scheduler's order — panel, SOF, MCU, the eight DSPs, the chain — and
the MCU can complete a host-command handshake. The stub adds one thing the
quantum does not offer: a decision point between two MCU instructions, which is
where the breakpoint compare happens. A breakpoint is therefore still an exact
equality on the MCU program counter, tested after every single instruction, and
the DSP advance cannot swallow or delay a hit.

**`stepi` retires one MCU instruction and turns one whole quantum.** That is not
timing-faithful and the gap is not small: a quantum's MCU budget is thousands of
cycles and a step spends one instruction's worth. It is the smaller of the two
available distortions — freezing the DSP set instead would distort the rate
without limit and would make a stepped session unable to cross a handshake at
all — and it is the direction the hardware goes, since halting one core at a
debugger prompt does not halt the other nine. **Ask timing questions with
`continue` and a breakpoint, never with a stepping session.**

**One skew is worth knowing about.** The quantum in which a stop happens runs its
remaining phases to completion, so the DSP set can be up to one block ahead of
the MCU at the stop. Returning from the middle of a quantum would leave the
machine in a state the scheduler's order never produces.

### What this still cannot do

**It reaches the MCF5307 only. There is no DSP56300 stub.** The DSPs now RUN;
they are not INSTRUMENTED. There is no way to break on a DSP program counter, to
read a DSP register through this session, or to watch a DSP memory address. That
core has no stock GDB target and would need its own register map and a custom
target description. **A question about DSP state still needs the older method.**

**A breakpoint that is never reached reports a plausible MISS and not an error.**
The stop reply a `continue` sends after exhausting its own bound is the same
`T05` a real hit sends, so "the machine did not stop where I armed it" is
reported as "not reached" whatever the reason. **Pair every negative with a known
positive** — arm a second breakpoint you are certain is reached, and treat a run
in which BOTH are missed as a broken instrument rather than as evidence. This
cost the project a day once: a breakpoint on `0x30038D1E` came back a clean miss
while the session had in fact stalled at `0x300505D4`, the MCU spinning on an
HDI08 ISR byte for a DSP that could not run. That specific stall is what the
Scheduler above fixes; the reporting shape it hid behind has not changed.

**A watchpoint sees MCU bus traffic and nothing else.** The wrapper it works
through sits in front of the MCU memory map's targets, so `watch` answers "who
among the MCU's accesses wrote this" and never "did a DSP DMA write this". A
watchpoint record is also cleared at the start of each quantum's MCU phase, so a
stop reply names an access the MCU made in that phase.

**No symbols and no source-level anything.** The firmware is a stripped binary.

**The stub is opt-in and absent by default.** A run without `--gdb` executes none
of it, and `Scheduler` pays one null check for each quantum when no stub is
attached.

**The scripted client: `gdbScript.py`.** `lldb`'s `gdb-remote` client cannot
drive this stub — a measured finding, not an assumption — so the repo ships a
minimal RSP client at `source/nord/g2/g2TestConsole/gdbScript.py` (stdlib only,
class-based, importable). An operator session looks like:

```bash
NMG2_ARTIFACTS=<artifacts> g2TestConsole --gdb 0 &          # prints the bound port
python3 source/nord/g2/g2TestConsole/gdbScript.py --port <bound> --script myscript
```

The script file is one `break 0xADDR` or `watch 0xADDR` per line (a `watch` is
a write watchpoint, `Z2`); the client arms each one, runs one
continue-until-stop per line, and prints one `stop pc=0x… hit=<kind>@0x…
count=N` line per stop. A stop that names no armed point prints `hit=none` — a
miss is REPORTED as a miss, and the `pc` field is read from the machine's
register file, not from the client's belief, so a client that lies about the
hit is caught by its own pc line. `D` ends the session and exits the console
cleanly.

**Bare session or harness?** Use a bare session (`m68k-elf-gdb`/`lldb`/
`gdbScript.py` by hand) for boot-path and no-traffic questions: is this routine
reached on boot, who writes this address during the banner, what do the
registers hold at the fault. Use the harness — `gdbScript.py` driven against a
machine that has `InternalClient` traffic originated into it, the way
`t0_gdb_script.cpp` does — for anything on the USB delivery path: with no
producer attached the endpoint-3 interrupt bit is never asserted and
`0x30055D36`/`0x30055DD0` and the `0x302A26F8` queue watchpoint never fire, and
that absence says nothing about the patch path. A breakpoint question about the
delivery path answered from a no-traffic session is an unsound negative.

**ARM BREAKPOINTS AFTER BOOT, AND BOUND EVERY WINDOW — both rules exist
because a violation of either already cost seven hours of a runaway run.** The
firmware's boot crosses mailbox-wait and HDI08-handshake loops (`0x3005454C`,
`0x300018FC` and neighbours) hundreds of times; a breakpoint armed before the
banner stops the machine at each crossing, and a continue-and-rearm loop over
those stops never escapes to the delivery phase it was armed for — the measured
shape was 35 stops, all in the handshake region, and a log that hit its cap
without one delivery-window record. **Boot the machine with breakpoints
disarmed (or with the stub unattached), arm the delivery-window points only
after the boot predicate passes, and give every driven window a hard quantum
bound AND a wall-clock timeout so a stuck run terminates itself instead of
spinning.** An unbounded window is a run that hangs; a bound without a clock is
a run that outlives the session that launched it. Both were learned here, not
assumed.

The listening socket binds loopback and never `INADDR_ANY`. It is an
unauthenticated channel with full read and write access to the emulated machine.

## Related

This fork is one repository of the Nord Modular G2 emulator project. The
cross-repository rules, the roadmap and the private-submodule prohibition live
in the `nmg2-artifacts` repository. `source/dsp56300` and `source/mc68k` are
submodules pointing at their own repositories; a change to either belongs in
that repository's fork, not here.
