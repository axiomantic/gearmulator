# Agents

Instructions for AI coding agents working on this repository.

This repository is part of the Nord Modular G2 emulator project. The work and
the execution ceremony live in the project roadmap, in the `nmg2-artifacts`
repository. Read it before you start a task. This file states the rules that
apply while you write code here.

## This project does not use the develop ceremony

**Do not invoke the `develop` skill here. Do not use a planning mode, a phase
ladder, an approval gate, or a quality-gate stack.** A general instruction to
route substantive changes through that ceremony does not apply to this project.
The operator has exempted it deliberately.

The process is the five phases in the project roadmap. That is the whole of it.

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

## Comments and docstrings

The code says what it does. A comment says why this was chosen over the
alternative.

**Never write these in a comment, a docstring, or a test name:**

- A pointer to a task, a plan, a design, or a document section. This has no
  exception. A citation of this project's own specification is forbidden in the
  same way as a citation of a task ledger. Write the fact instead.
- A count of cases, tests, files, symbols, or lines.
- A coverage claim, or any claim about the rest of the tree.
- A list of unfinished work.
- A note about history. Git holds that.

**These are protected. Never remove them:**

- Datasheet and hardware-manual citations.
- Hazard banners.
- Comments inherited from upstream or vendored code.
- A number that a mechanism reads and checks at build time or test time --
  protected where one already exists, never created new.

**Sweeps are permitted.** You can rewrite comments across many files in one
change. A sweep changes no behaviour. If you cannot say that with certainty
about a file, split the change and handle that file on its own.

## Tests

**Always pass `--no-tests=error` to ctest.** Without it, ctest exits 0 when the
filter matches no test. A pass and an empty run then look the same. This project
has a repository where that exact false green is live today.

Write the failing test first. Confirm that it fails for the intended reason.

A test must consume real values. A test that checks only exit status,
non-emptiness, or truthiness proves nothing. Before you call a test done, plant
a fault and confirm that the test goes red.

## Verify the artifact, not the signal

A step that can do nothing reports success in the same way as a step that
worked. When a step writes a file, regenerates code, or targets a path you did
not name, look at what it produced. Do not read the exit code and stop.

Count with a command. Never estimate a number, and never recall one.

State what you ran next to the result. A rule stated more broadly than what you
tested is false in a way the test will not show you.

## Pull requests

Open every pull request against this project's own fork. **Never open one
against an upstream repository.** Confirm the base repository after you create
it; the command line tool defaults a fork's base to the upstream project.

Some repositories in this project are forks of other people's work. Their
default branch is this project's working branch. It carries the upstream project
plus the tooling this project needs to work on it, and it is never submitted. A
pull request in one of those forks is a FIRST DRAFT of what this project may one
day offer that project's maintainers; a submission is eventually rebased onto
the UPSTREAM default branch, which carries none of this project's tooling.

A pull request in a fork is based on the FORK's default branch, so it inherits
this project's tooling while the work is in progress. A submission is rebased
onto the UPSTREAM default branch, so none of that tooling reaches it.

So sort every change in a fork into one of four kinds.

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

## Git

Never push to a default branch without permission. Never force push without
stating what it discards first.

Never run a tree-wide git operation in a checkout you share with anyone:
`git stash`, `git checkout .`, `git restore .`, `git clean -fd`,
`git reset --hard`. To compare against a commit, read it with `git show`.

Never put an issue number in a commit message, a pull request title, or a pull
request body. It notifies every subscriber.

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

## Searching

`git grep` does not see untracked files. An empty result and an unsearched file
look the same. Use `rg`, and name the tool beside any claim that something
appears nowhere.

Quote every argument that contains a glob character. An unquoted `?` or `*` is
eaten by the shell, the command never runs, and the empty output reads exactly
like a measured absence.

A path that is missing from a default branch is not missing from the repository.
Two repositories here hold their product work on stacked branches. Check the
branch before you report a file as absent.

---

## This repository

**Licence: GPL-3.0. This is a fork of `dsp56300/gearmulator`.** Never open a
pull request against that repository. Code here cannot move into `mcf5307` or
`nmg2-tools` unless this project wrote it.

The G2 work is `source/nord/g2`: `g2Lib`, `g2JucePlugin`, `g2TestConsole`. It
is not on the default branch. It is on stacked branches.

Formatting follows `source/.clang-format`: tabs, tab size 4, 120 columns.

Build and test:

    cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build -C Release --no-tests=error

If configure fails in `xcodeversion.cmake`, pass `-DXCODE_VERSION=<version>`.
The script reads `xcodebuild -version`, which is empty when `xcode-select`
points at the command-line tools.

Do not add a `Co-authored-by` trailer in this repository.
