# gearmulator (axiomantic fork) — agent instructions

This is a **fork** of `dsp56300/gearmulator`. Upstream is a low-level IC emulator
for classic virtual-analog synthesizers. This fork adds the Nord Modular G2
device under `source/nord/g2/`.

Repository: `axiomantic/gearmulator`. Upstream: `dsp56300/gearmulator`.
Licence: GPL-3.0. Contributions must be GPL-3.0 compatible, with attribution.

**Build commands and the upstream architecture are documented in `CLAUDE.md`,
which is an upstream file.** Read it for those. Do not restructure it. This file
carries only what is specific to the fork.

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

**One exception, and it is the only one.** A number that a mechanism reads and
checks at build time or at test time may stay. The check is then the source of
truth, not the comment, and it fails loudly when the number drifts. A number
that no mechanism reads is a liability.

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
