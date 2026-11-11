# Releasing

GitHub Actions builds the firmware only when a version tag is pushed. Pushing commits to `main` or opening a pull request does not build anything. The workflow is [.github/workflows/release.yml](.github/workflows/release.yml).

## Cut a release

1. Make sure `main` builds locally and the build was tested on the board (see the build command in the README).
2. Make sure everything is committed and pushed:

```bash
git status
```

```bash
git push origin main
```

3. Tag the commit and push the tag:

```bash
git tag -a v1.0.0 -m "v1.0.0"
```

```bash
git push origin v1.0.0
```

4. Watch the run under the repository's **Actions** tab. It takes about two minutes. When it is done, the release appears under **Releases**.

A tag with a dash, such as `v1.1.0-rc1`, is published as a **pre-release**.

## Version numbers

`vMAJOR.MINOR.PATCH`:

- **PATCH:** bug fixes, nothing changes for the user.
- **MINOR:** new features or changed defaults. The stored keymap is only reset when `VIAL_BUILD_ID` (`qmk_porting/keyboards/bnk16/rules.cmake`) was counted up for this release; if it was, mention that in the tag message.
- **MAJOR:** the flash layout or the bootloader changes in a way that needs a `wchisp` flash of the full image again.

The tag ends up in the firmware: the build uses `git describe`, so a tagged build reports exactly `v1.0.0`. Untagged local builds report something like `v1.0.0-3-gabc1234` (or `-dirty` with uncommitted changes).

## What the workflow does

1. Checks out the repository with full history and the QMK submodule.
2. Runs `firmware/ch585_v2/build-bnk16.sh` in the `nookcraftlab/riscv` Docker image, the same command as a local build. That builds the bootloader and the firmware, and creates the UF2 and the combined image.
3. Writes the release notes:
   - a changelog: every commit subject since the previous `v*` tag, oldest first (for the first release, all commits)
   - a table explaining the files
4. Creates the GitHub release for the tag and attaches the files.

Because the changelog is built from commit subjects, write subjects that make sense to a reader, like `BNK16: add media keys over BLE`. Docs-only commits show up too. That is fine, or reword them before tagging.

## Release files

| File | What it is | Who needs it |
|---|---|---|
| `bnk16-vX.Y.Z.uf2` | Firmware as UF2, for drag-and-drop onto the `BNK16BOOT` drive | Everyone, for normal updates |
| `bnk16-vX.Y.Z-full.bin` | Bootloader (`0x0000`) + firmware (`0x8000`) in one image | First install and recovery, flashed with `wchisp` |
| `bnk16-vX.Y.Z-app.bin` | Firmware only, a raw image that starts at `0x8000` | Debugging |
| `bnk16-bootloader-vX.Y.Z.bin` | Bootloader only | Debugging |
| `SHA256SUMS.txt` | Checksums of the files above | Anyone checking a download |

## Fix a broken release

If the workflow failed, or a release came out wrong:

1. Delete the release in the GitHub UI (or with `gh release delete vX.Y.Z`).
2. Delete the tag locally and on GitHub:

```bash
git tag -d v1.0.0
```

```bash
git push origin :refs/tags/v1.0.0
```

3. Fix the problem, commit, and tag again.

Only do this for a release nobody has downloaded yet. Otherwise, publish a new patch version.

## Local equivalent

Run the same build without GitHub:

```bash
docker run --rm --platform linux/amd64 -v "$PWD:/workspace" nookcraftlab/riscv:latest bash /workspace/firmware/ch585_v2/build-bnk16.sh
```

All files end up in `firmware/ch585_v2/build-bnk16/`: `bnk16.uf2`, `bnk16_full.bin`, `bnk16.bin`, `bnk16_bootloader.bin`.
