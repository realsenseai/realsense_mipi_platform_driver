# nvidia-oot-patch

Generates a formatted patch from the latest commit in the external `nvidia-oot` repository (the patches repo lives under
`sources_$(cat jetpack_version)/nvidia-oot`), places it into `sources_$(cat jetpack_version)/nvidia-oot/6.0`, and creates a
symlink in `sources_$(cat jetpack_version)/nvidia-oot/6.2`.

This skill chooses the next numeric 4-digit patch index automatically. It finds the maximal existing `NNNN-` prefix in the
target `6.0` folder and uses `NNNN+1` for the new patch filename.

Usage (manual):

From the repository root run the sequence below; the commands compute the next index and write the formatted patch using that
index (they use the `jetpack_version` file in the repo root to locate `sources_<JP>`):

```bash
# get JetPack tag and external nvidia-oot repo path
JP=$(cat jetpack_version)
SRCDIR="sources_${JP}/nvidia-oot"

# compute next 4-digit index and create formatted patch from latest commit in that repo
MAX=$(ls -1 "${SRCDIR}/6.0" 2>/dev/null | grep -E '^[0-9]{4}-' | sed -E 's/^([0-9]{4}).*/\1/' | sort -n | tail -n1 || echo 0)
NEXT=$(printf "%04d" $((10#$MAX + 1)))
git -C "${SRCDIR}" format-patch -1 --stdout > "${SRCDIR}/6.0/${NEXT}-short-desc.patch"

# create or refresh symlink in the 6.2 folder
ln -sf ../6.0/${NEXT}-short-desc.patch "${SRCDIR}/6.2/${NEXT}-short-desc.patch"
```

Notes / conventions:

- The apply script (`apply_patches.sh`) expects patches under `${BUILD_SRCS}/nvidia-oot`. For JetPack >= 6.0 `BUILD_SRCS` equals
	`sources_${JP_INPUT_VERSION}`, so creating patches in `sources_$(cat jetpack_version)/nvidia-oot` places them where the apply
	workflow will look for them.
- If `sources_${JP}` is not present (you have not run `./setup_workspace.sh`), you can fall back to the local repository's
	`nvidia-oot` directory by adding a small check:

```bash
JP=$(cat jetpack_version)
SRCDIR="sources_${JP}/nvidia-oot"
if [[ ! -d "${SRCDIR}" ]]; then
	SRCDIR="nvidia-oot"
fi
```

- Use a numeric 4-digit prefix to match the existing series (for example `0010-...`).
- Verify the generated filename before committing to avoid accidental collisions if you run the commands concurrently.
- The skill does not auto-commit; to record the generated patch(s) in git run:

```bash
git add "${SRCDIR}/6.0/${NEXT}-short-desc.patch" "${SRCDIR}/6.2/${NEXT}-short-desc.patch"
git commit -m "nvidia-oot: ${NEXT} Add <short description>"
```

Suggested automation: wrap the commands above in a small script that checks for concurrent runs, accepts a short description to
populate the filename, and optionally commits the result. Keep the script idempotent and safe to run concurrently by checking for
existing `${NEXT}` prior to creating the patch.

Validation (recommended)

After generating the patch, validate it does not touch kernel tree files (for example `kernel/realsense/d4xx.c`). The
`apply_patches.sh` flow expects only the external `nvidia-oot` drivers/header changes under `drivers/` and `include/`.
Add this quick check to the script to avoid accidentally generating patches from the main repo:

```bash
# PATCHPATH is the generated patch file path
if grep -qE '^diff --git (a|b)/kernel/' "${PATCHPATH}"; then
	echo "ERROR: patch touches kernel/ files — regenerate from the nvidia-oot repo, not the main repo."
	exit 1
fi

# More specific guard for the known problematic file
if grep -q 'kernel/realsense/d4xx.c' "${PATCHPATH}"; then
	echo "ERROR: patch contains d4xx.c; aborting to avoid applying kernel changes into nvidia-oot."
	exit 1
fi
```

If the validation fails, inspect the commit used to create the patch and re-run the `git -C "${SRCDIR}" format-patch` command
against the correct `nvidia-oot` repository. The automation script can exit non-zero on validation failure so CI or local checks
will catch the mistake early.

Move generated patches to top-level `nvidia-oot`

When you have a generated patch that is intended to live in the main repository (the `nvidia-oot/6.0` series), copy it into
the repository root's `nvidia-oot/6.0` and create a lightweight symlink in `nvidia-oot/6.2` that points at the top-level file.
Do not auto-commit these filesystem changes from automation — leave them unstaged so you can review before committing.

Example (run from the repo root):

```bash
# source values from earlier steps (JP, SRCDIR, NEXT, PATCHPATH)
PATCHNAME="${NEXT}-short-desc.patch"
PATCHPATH="${SRCDIR}/6.0/${PATCHNAME}"

# copy the generated patch into the main repo top-level nvidia-oot/6.0
mkdir -p nvidia-oot/6.0 nvidia-oot/6.2
cp "${PATCHPATH}" nvidia-oot/6.0/

# create or refresh a symlink in the 6.2 folder that points to the top-level patch
ln -sf ../6.0/${PATCHNAME} nvidia-oot/6.2/${PATCHNAME}

# IMPORTANT: do NOT auto-commit here. Leave the files unstaged for manual review.
echo "Patch copied to nvidia-oot/6.0 and symlink created in nvidia-oot/6.2 (unstaged)."
```

This keeps the patch series discoverable in the repository root (so `apply_patches.sh` can find it) while giving you a manual
checkpoint to inspect the generated contents and remove or adjust any kernel-sourced hunks (for example `kernel/realsense/d4xx.c`)
before committing.