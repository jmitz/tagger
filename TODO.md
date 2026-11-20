# TODO

## Next: two-tag spool capture with a single Send weight

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
1. **How to store two UIDs in Spoolman.** Spoolman 0.27 has one text extra
   field, `nfc_uid`. Options: both UIDs comma-separated in `nfc_uid`, or add
   a second field (e.g. `nfc_uid_2`). Should a match need *both* UIDs,
   or is either one enough?
2. **"Updated" when the spool isn't found.** Link the tag pair to an existing
   spool (chosen on the touchscreen from a list?), create a new spool, or
   only show "not linked"?
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
