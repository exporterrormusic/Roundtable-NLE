# Reference frames — drop your real tier-list samples here

So I can match the renderer to your existing template **exactly** (not eyeballed), save full-res
(ideally 1920×1080) frames from your actual tier-list videos into this folder:

- `sample_list.png` — a frame showing the **full tier list** (all rows populated, no spotlight) — like the 3rd image you pasted.
- `sample_spotlight.png` — a frame with an **entry spotlighted** (discussion state) — like the 1st image you pasted.
- (optional) `sample_banner.png` — any frame that clearly shows the **top channel banner** and the **right commentator column**.

Once they're here I'll run a measurement script and extract exact defaults:
- canvas size; **top banner** band height; left **title-sidebar** width; **tier label-cell** width.
- each **tier row's** y-range / height (are they equal?), and inter-row borders.
- **right safe-margin** (where entries stop / commentators begin) and **entry gaps/padding**.
- **entry thumbnail** size for each `entryAspect`.
- **spotlight panel** rect (exact position + size, and how much it dims the grid).
- exact **tier hex colours**, plus the title/banner font sizes & weights.

These become the `TierListClip` layout defaults so a fresh tier list looks like your videos out of the box.
