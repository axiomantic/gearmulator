# Divergence from upstream

This fork changes upstream `gearmulator`. Every change that is not a candidate
for an upstream pull request is a divergence, and every divergence gets a row in
the table below.

## The rules

1. One row for each divergence.
2. Add a new row at the end of the table. Do not change a row that another
   change added.
3. A row names the file, the reason for the divergence, and if the divergence
   can be removed.
4. A row that says a divergence can be removed also gives the condition that
   permits the removal.

## The divergences

| File | Reason | Can it be removed? |
|---|---|---|
| `.gitmodules` | The `source/dsp56300` submodule points at the fork `axiomantic/dsp56300` and not at `dsp56300/dsp56300`. The fork carries this project's framework changes before they merge upstream, and `source/nord/g2/` must compile against them. The pinned commit is `c051afad31612c2d2c7a81a7ab23e1c5ac9e61af`, which is the commit upstream `gearmulator` pinned, so this change moves the URL and not the framework version. | Yes. Remove it when every contribution of design section 21.2 has merged into `dsp56300/dsp56300`. Point the URL back at upstream and pin a commit on upstream. |
