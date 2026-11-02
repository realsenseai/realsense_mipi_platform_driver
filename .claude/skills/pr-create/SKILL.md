---
name: pr-create
description: Open a draft GitHub PR on realsense_mipi_platform_driver with the standard description structure — a short **Abstract**, a **What changed** section and a **Validation** section. Owns the branch-name check, the PR title convention, the description template and its rules, the show-before-create gate, and the `gh pr create` / read-back mechanics. Use when the user asks to open, create or raise a PR, or to write or rewrite a PR description. Not for release PRs (`r/58.x` → `master`), which follow the release flow.
---

# Skill: Create a PR

**Abstract:** every PR on this repo opens as a **draft** against `dev`. Its body has three
sections in a fixed order: **Abstract**, **What changed**, **Validation**. All three are always
present. A section with nothing to report says so and why; it is never left out.
Ported from `perc_hw_fw-rs400`'s `pr-create` skill.

## Hard rules

- **Create only on request.** Run `gh pr create` only if the user asked for a PR this turn.
  A request to write a description is not a request to open the PR.
- **Always `--draft`.** The PR leaves draft only when the user asks. A `ready_for_review` done
  by the user's own GitHub account counts as that request; never re-draft it.
- **Show the title and body, then wait for approval** before creating.
- **Branch name:** `fix/RSDSO-*`, `fix/RSDEV-*`, `feature/**` or `docs/**` (CLAUDE.md
  § "Branch names"), enforced on origin by the `branch-naming` ruleset. Rename with
  `git branch -m` before pushing, or the push fails with GH013.
- **Base is `dev`** (CLAUDE.md § "Branching"). `master` takes only release merges.
- **No AI attribution.** No `Co-Authored-By` or "Generated with Claude Code" lines in commits,
  titles or bodies (user-level CLAUDE.md).

## Steps

1. **Preflight.** Confirm the branch name is allowed and the branch is not `dev`/`master`.
   Ask whether to rebase onto `origin/dev` first, so the description matches the final diff.
2. **Gather facts.** Take them from the net diff (`git diff --stat origin/dev...HEAD`), not from
   memory of the session. Validation evidence comes from what was actually run: JetPack builds
   (`build` skill / CI), rig results (`v4l2-test`, `streaming-test`, `libci-test`), dmesg.
3. **Draft the title and body** from the template below, into a scratchpad file.
4. **Check the draft** against the section rules and CLAUDE.md § "Comment style" (short,
   skimmable). Then show it and wait for approval.
5. **Push and create:**
   ```bash
   git push -u origin <branch>
   gh pr create --draft --repo realsenseai/realsense_mipi_platform_driver \
     --base dev --head <branch> --title "<title>" --body-file "<scratchpad>/pr_description.md"
   ```
6. **Read back** with `gh pr view <n> --json isDraft,title,body,baseRefName`. Confirm it is a
   draft on `dev` and the body landed intact. Report the URL.
7. **After creation:** the JP6 required check comes from the Jenkins poller, not GitHub Actions
   (see the MIPI PR monitor memory / `mipi-ci-triggered` label). Aikido findings get a threaded
   `@AikidoSec ignore: <reason>` reply when they are false positives or out of scope.

**Rewriting an existing description:** same template, same approval gate. Push it with
`gh api -X PATCH repos/realsenseai/realsense_mipi_platform_driver/pulls/<n> -F body=@<file>`,
then read it back. `gh pr edit` can exit 0 without changing the body.

## Title

- Ticket work: `[<KEY>][<AREA>] <what the change does>`, e.g.
  `[RSDEV-15754][D58X] Allow aligned depth set while 3C Color streams`. `[AREA]` is optional.
- No ticket: `<area>: <what the change does>`, where area is `ci`, `build`, `docs`, `skills`, …
- Imperative, at most ~80 characters, no trailing period. Matches the commit subject.

## Body template

```markdown
**Abstract:** <2–4 lines, conclusion first: what the PR does, why, and the outcome.
Link the ticket.>

## What changed
- <one bullet per logical change, grouped by area; give the why where it is not obvious>
- <name any V4L2 control/CID, UAPI, DT-property, SerDes-op or locking-contract change explicitly>

## Validation
- **Build:** <JetPack versions built, locally or CI>
- **Hardware:** <rig, camera SKU(s), deserializer/overlay, JetPack, FW version, result> — or "Not run: <reason>"
- **Tests:** <v4l2-test / streaming-test / libCI run, pass/fail counts, link>
- **Not covered:** <JetPack generations, SKUs, SerDes or paths the change touches that no test exercised>
```

## Section rules

**Abstract**
- About 10% of the body, **hard cap 5 lines**. Lead with the result, not a restatement of the
  ticket. A reviewer who reads only the abstract should know what changes and whether it was proven.

**What changed**
- Describe the net diff, not the session's history. Drop abandoned approaches.
- One bullet per logical change, not per file. If a change touches patch carriers, say which
  generations (JP5 / JP6 / JP7) it covers.
- No raw code as explanation. Describe behaviour and link to the code at a pinned commit SHA.

**Validation**
- **List only what was actually run, each with its result.** "Should work" or "low risk" is not
  validation. An unproven claim is labelled THEORY.
- Docs/skills-only PRs still carry the section, with Hardware set to "Not run: docs only".
- **Not covered** is never left out on a driver PR. One driver source builds against every
  JetPack, so name the generations that were not built or run.

**Everywhere**
- Never include personal data: usernames, SSH key names, rig passwords, local paths.
- Write `@name` only for a real login you mean to notify.
