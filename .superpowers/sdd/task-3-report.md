# Task 3 Report: Create Asset Inventory

## What I implemented

Created `docs/tutorial/m5tab5-hyperframes-assets.md` as the Task 3 asset inventory for the M5Tab5 HyperFrames tutorial.

The file includes:
- The required asset policy section.
- The required asset table with IDs `A001` through `A012`.
- The optional asset table with IDs `O001` through `O003`.
- The generated assets table with IDs `G001` through `G003`.
- The capture checklist.
- The scene coverage check for S1 through S7.

## Verification

Ran the brief’s storyboard read command:

```bash
rg -n "Visual plan:|^- " docs/tutorial/m5tab5-hyperframes-storyboard.md
```

Result: confirmed visual plan bullets exist for S1 through S7.

Ran the asset ID uniqueness check from the brief:

```bash
rg -o "^[|] [A-Z][0-9]{3}" docs/tutorial/m5tab5-hyperframes-assets.md | sort | uniq -d
```

Result: no output, so no duplicate asset IDs were found.

Committed the file with:

```bash
git commit -m "docs(tutorial): add M5Tab5 video asset inventory"
```

Result: commit succeeded.

## Files changed

- `docs/tutorial/m5tab5-hyperframes-assets.md`

## Self-review findings

- The content matches the task brief exactly in structure and wording where specified.
- Asset IDs are unique and cover the full S1 through S7 scene mapping.
- The report intentionally avoids touching storyboard, voiceover, `DESIGN.md`, plan files, or `docs/TUTORIAL-outline.md`.

## Concerns

- `docs/TUTORIAL-outline.md` remains untracked in the workspace as requested and was not modified or committed.

## Fix report for review follow-up

Updated `docs/tutorial/m5tab5-hyperframes-assets.md` to address the review findings:

- Split the collapsed LED entry into two distinct required assets:
  - `A008` for `Email LED` with `app/apps/app_email_led`
  - `A013` for `Unit-Puzzle LED` with `app/apps/app_unit_puzzle`
- Added an explicit `Required Evidence Sources` section that names:
  - `docs/TUTORIAL-outline.md`
  - `git history`
  - HA stability / flashing memory
  - Tab5-to-local-Claude approval / handoff memory
- Removed `O003` from the optional asset list so `git history` is no longer treated as optional.
- Updated the capture checklist and scene coverage notes to match the split LED assets.

Verification run after the edit:

```bash
rg -o "^[|] [A-Z][0-9]{3}" docs/tutorial/m5tab5-hyperframes-assets.md | sort | uniq -d
```

Output: no duplicates, no output.

```bash
rg -n "Email LED|Unit-Puzzle LED|Required Evidence Sources|docs/TUTORIAL-outline.md|git history|HA stability|Tab5-to-local-Claude" docs/tutorial/m5tab5-hyperframes-assets.md
```

Output confirmed the Email LED row, the Unit-Puzzle LED row, and all required evidence sources are present.

Files changed in this follow-up:

- `docs/tutorial/m5tab5-hyperframes-assets.md`
- `.superpowers/sdd/task-3-report.md`

Self-review:

- The inventory now distinguishes the two storyboard LED assets instead of merging them.
- The required evidence sources are explicit and marked required for later production use.
- The unique asset ID check still passes.
- I did not touch the storyboard, voiceover, `DESIGN.md`, plan files, or the untracked outline.

## Task 3 re-review fix

Adjusted `docs/tutorial/m5tab5-hyperframes-assets.md` to move `A013` from `S5` to `S3` so the Unit-Puzzle LED asset matches the storyboard's maker-feature placement.

Updated the scene coverage check so `S3` now explicitly includes `A013` alongside the existing S3 assets, while `S5` remains covered by the pitfall cards entry `A011` and the required evidence sources already listed in the inventory.

Verification run after the edit:

```bash
rg -n "A013|S3 has|S5 has|Unit-Puzzle LED" docs/tutorial/m5tab5-hyperframes-assets.md
```

Result:

```text
23:| A013 | S3 | Unit-Puzzle LED photo or generated strip visualization | `app/apps/app_unit_puzzle` runtime | Missing | Show ticker or puzzle state without reusing the Email LED slot. |
58:- [ ] Capture Unit-Puzzle LED strip or decide to draw it.
67:- S3 has A003 through A010, plus A013 for Unit-Puzzle LED, with A008 reserved for Email LED.
69:- S5 has A011.
```

```bash
rg -o "^[|] [A-Z][0-9]{3}" docs/tutorial/m5tab5-hyperframes-assets.md | sort | uniq -d
```

Result: no output, so no duplicate asset IDs were found.

Commit created:

```bash
git commit -m "docs(tutorial): move Unit-Puzzle LED to S3"
```

Result: commit succeeded.

Self-review:

- `A013` is now aligned with the S3 maker feature instead of S5.
- The S3 coverage check is explicit about the Unit-Puzzle LED asset.
- The S5 surface still has the pitfall card asset and no fake screenshot requirement was added.
- Asset IDs remain unique.

## Task 3 final re-review fix

Updated `docs/tutorial/m5tab5-hyperframes-assets.md` to address the final review findings:

- Expanded `A002` scenes from `S2, S4` to `S2, S4, S7` so the closing scene's reused system map is covered explicitly.
- Added a `Production Format` section requiring all captures, screenshots, generated visuals, and diagrams to be framed for `16:9 landscape` and safe within a `1920x1080` output area.

Verification run after the edit:

```bash
rg -n "A002|16:9|1920x1080|S7 has" docs/tutorial/m5tab5-hyperframes-assets.md
```

Result:

```text
9:All captures, screenshots, generated visuals, and diagrams must be framed for 16:9 landscape composition and remain safe within a 1920x1080 output area.
16:| A002 | S2, S4, S7 | Architecture diagram data | Derived from repo and gateway memory | Ready to draw | Firmware, app layer, Mac services, LAN HTTP JSON. |
70:- S2 has A002.
72:- S4 has A002 and A005.
75:- S7 has A001 and A002.
```

```bash
rg -o "^[|] [A-Z][0-9]{3}" docs/tutorial/m5tab5-hyperframes-assets.md | sort | uniq -d
```

Result: no output, so the asset IDs remain unique.

Self-review:

- The final review items are both closed in the asset inventory, and no storyboard or voiceover files were touched.
- The required evidence sources and existing asset IDs were preserved as-is.
