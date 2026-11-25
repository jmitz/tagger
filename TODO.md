# TODO

## Goal: touch-first Spoolman client (requested 2026‑11‑20; scope reduced 2026‑11‑25, see below)
The Tagger UI should implement **all capabilities of the Spoolman web UI**
(`http://spoolman.local:7912`, Spoolman 0.27), touch-first with minimal text
entry, including **adding new spools** and **updating existing ones**.

Design rules:
- Pick from lists wherever Spoolman provides values (`/location`, `/material`,
  `/lot-number`, `/article-number`, extra-field values, vendors, filaments,
  `/external/filament` catalog). Numbers via keypad or ± steppers. Colour via
  palette. On-screen keyboard only for names, comments and search.
- UI is C++ (`esphome/components/tagger_app`), LVGL built in code; HTTP runs
  on a background task so the screen never blocks.
- Not feasible on the device: label/QR printing, CSV export (backup can be
  triggered).

**Scope decision (user, 2026‑11‑25): the device UI is the Scan screen (as
built) plus the spool list used to add tags to an existing spool ("Link to
spool…").** New spools are created on the Tagger and edited in the Spoolman web
UI. The earlier phases 2–5 (on-device spool/filament/vendor editors, search,
settings) are **cancelled**. Don't build them unless the user asks again.

Possible small improvements to the spool list (only if asked):
- Quick filter chips (material / vendor) or a "no tags yet" filter at the top.
- Sort by most recently used.

Decisions taken for phase 1 (the user said "continue with phase 1" without
answering; these were the stated defaults, so revisit if the user objects):
- Use **Spoolman's built-in tags** (`spool.tags`, `POST /spool/{id}/tag`,
  `POST /tag/scan`) instead of the `nfc_uid` extra field (it held no data).
- **Either tag identifies the spool.** Both should be linked, and the Scan screen
  offers "Link tag" for the one that isn't.
- **No deletion on the device** (user decision 2026‑11‑25): delete spools,
  filaments and vendors in the Spoolman web UI. The device can archive.

**Workflow change (2026‑11‑25):** new spools are created *on the Tagger* as
a bare record (placeholder filament + linked tags), and the paired Spoolman web
page opens for the details. Done and verified. This reduces the need for
on-device forms. Re-check with the user whether phases 2–5 are still wanted as
planned before building them.

Phase 1 behaviour as built (user-directed changes during testing):
- **Tags latch** in arrival order from either reader: the first UID is held
  until a second, *different* UID arrives, then both are held. Tags leaving a
  reader's field are ignored. (The first version cleared on removal, and the
  readers flicker or both see one tag.)
- **Reset:** the spool is lifted (weight above 30 g while latched, then below
  30 g for 1.5 s), or the **Clear** button.
- Both tags unknown → **Link to spool…** opens a scrollable spool list
  (non-archived, sorted by vendor/material), and tap + confirm links both tags.
  One tag known → **Link tag** links the other to the same spool.

## Two-tag spool capture with a single Send weight (done in phase 1)

**Goal (requested 2026‑11‑19).** Each spool carries **two tags**, read at the
same time by reader 1 and reader 2. The device captures both tags, then the
spool's weight, and then queries or updates Spoolman. The UI has **one**
Send weight button instead of one per reader.

### Flow
1. **Capture tags.** Wait until *both* readers report a tag. Treat the pair of
   UIDs as one spool. If only one reader has a tag, show "waiting for second
   tag" (not an error).
2. **Capture weight.** Once both tags are read, wait for a *stable* scale
   reading (e.g. within ±2 g for 2 s) and freeze it as the captured weight.
   Show it on screen with a "stable" indicator.
3. **Query Spoolman.** Look up the spool by the tag UIDs (`nfc_uid` field;
   see open question 1).
   - Found: show the spool (vendor, material, name, remaining) with the
     captured weight and the computed remaining filament.
   - Not found: offer to link the pair to a spool (see open question 2).
4. **Send weight.** A single button sends
   `remaining_weight = captured weight − empty-spool weight` to that spool
   (PATCH `/api/v1/spool/{id}`). The update is sent only on button press.
5. **Reset** when the spool is lifted (both tags gone). Ready for the next spool.

### UI changes
- Replace the two per-reader Send weight buttons with one button.
- Combine the two reader cards into one spool card. Keep a small per-reader
  tag indicator (✔ / waiting).
- Disable Send weight until: both tags read, spool identified, weight stable.

### Open questions (settle before implementing)
1. ~~How to store two UIDs.~~ Resolved: Spoolman 0.27 supports several native
   tags per spool, and either tag identifies the spool.
2. ~~"Updated" when the spool isn't found.~~ Link to an existing spool is done
   (picker); creating a new spool is phase 3.
3. **Mismatched tags.** If the two UIDs belong to *different* Spoolman spools,
   block Send weight and show a warning (proposed).
4. **Stability threshold.** ±2 g / 2 s is a proposal. Confirm against real
   spool placement.

### Done when
- Placing a two-tag spool shows "tags 2/2 → weight stable → spool #N"
  without any taps.
- One Send weight tap updates Spoolman, and the new remaining weight is visible
  in Spoolman.
- Lifting the spool resets the screen.

## Backlog
- Wire PN532 RSTPDN (side-row pad) to GPIO39/40 and add hard reset on hang.
- Test OpenSpool NDEF tags (NTAG215) end to end.
- Write tags from the touchscreen.
- Optional: OpenSpool's Bambu printer link (not currently a goal).
