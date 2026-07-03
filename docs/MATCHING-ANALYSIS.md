# Why fingerprint matching can't be made reliable on this sensor (final analysis)

Sensor: ELAN7001 / eFSA80SC (80x80 SPI, PID 04f3:3104), ASUS VivoBook
X513EAN. Driver: `mincrmatt12/libfprint` branch `elanspi-3104` + our fixes.
Matcher: libfprint's bundled NBIS (mindtct minutiae + bozorth3), the only
matcher libfprint has.

## What we fixed along the way (all real improvements)

1. **2x upscale removed** (`fpi_image_resize(img, 2, 2)` in
   `elanspi_fp_frame_stitch_and_submit`). The assembled image is 120px wide
   with a ~10px ridge period — already exactly in NBIS's expected band
   (tuned for ~500dpi). The upscale doubled the ridge period and starved
   the minutiae detector: 7-12 minutiae per capture instead of 15-45.
2. **`FPI_IMAGE_PARTIAL` flag removed.** It makes NBIS cull perimeter
   minutiae; on a narrow 120px strip nearly *everything* is perimeter, so
   it deleted 70-85% of minutiae (27→8 on one test image).
3. **Gaussian smoothing added** (σ≈1.2 separable pass in the driver).
   Sensor noise created spurious minutiae; smoothing made minutiae
   repeatable. Genuine pair scores went from ~3 to ~10-26.

Combined effect: minutiae per capture went from ~7 to ~15-22, and genuine
match scores went from a flat 0 to a 0-26 range. First real-world
`verify-match` was observed after these fixes (1 in 5 attempts at
threshold 24).

## The measurement that ends the story

Built two offline tools (saved in the project repo):
- `minutiae-dump.c` — capture via the real driver path, dump image +
  minutiae.
- `offline-match.c` — run the exact fprintd minutiae→bozorth3 pipeline on
  saved captures, printing raw scores.

Captured a labeled batch: 6x right index (enrolled finger), 7x left index
(impostor), all with careful consistent technique, through the fixed
driver. Cross-matched everything:

**Genuine (right vs right, 15 pairs):**
0, 0, 0, 3, 5, 6, 6, 7, 9, 9, 10, 12, 13, 21, 22 — median ~7, max 22

**Genuine (left vs left, 9 pairs):** 0, 0, 3, 3, 5, 6, 9, 9, 23

**Impostor (left vs right, 42 pairs):**
mostly 3-17, notable values 19, 21, and **27** — median ~9, max 27

The genuine and impostor distributions **fully overlap**. An impostor
pair (leftA vs right2 = 27) scored *higher than any genuine pair* and
above the driver's match threshold (24). There is no threshold with both
usable acceptance and meaningful security:

| Threshold | Genuine accept/attempt* | Impostor accept/attempt* |
|---|---|---|
| 24 (current) | ~10-20% | ~4% per pair → up to ~25% vs 7 templates |
| 16 | ~60% | ~30%+ |
| 12 | ~75% | ~50%+ |

*rough estimates from the pair distributions; verify matches against the
best of 7 enrolled templates, which multiplies both rates.

## Why (root cause)

The assembled capture is a ~6mm-wide strip in which ridges run mostly
horizontal everywhere (swipe geometry guarantees it). bozorth3 matches
minutiae constellations by pairwise distance/angle consistency — and in
narrow near-parallel-ridge strips, *any* two fingers produce similar
constellations. The matcher output is dominated by chance geometric
alignments; it measures strip geometry, not finger identity. More/better
minutiae raised both genuine AND impostor scores in lockstep — improving
image quality cannot fix a discrimination failure of the matcher on this
image geometry.

This quantifies what upstream only hinted at: the driver author's comment
(`nr_enroll_stages = 7; /* these sensors are very hit or miss */`) and the
prototype README's note that "the libfprint image matching algorithm is
not designed to deal with such small sensors."

A real fix would require a different matching algorithm (e.g. the
correlation-based matching the Windows vendor driver appears to use, or
an on-host elastic ridge matcher) — out of scope for libfprint today,
which has NBIS as its only matcher.

## Final state / recommendations

- Capture, enrollment: **working** (first confirmed working report for
  PID 0x3104). Patches: X571 quirk removal, meson pixman fix, resize
  removal, PARTIAL flag removal, smoothing.
- Verification: **must not be trusted.** With the improvements it
  sometimes matches, but the false-accept probability is far too high for
  authentication. Arguably the pre-fix behavior (always no-match) was
  *safer*.
- **PAM integration: do not enable.** This is now a data-backed decision,
  not caution.
- **Publish everything** — the fixes, the tools, and the genuine/impostor
  score matrices are exactly the evidence upstream needs on issue #13.

---

# ADDENDUM: SOLVED with SIGFM (SIFT-based matching)

The analysis above stands — NBIS/bozorth3 cannot discriminate fingers on
these narrow strips. But an internet sweep found the goodix-fp-linux-dev
community hit the same wall with their small sensors and built **SIGFM**
("SIFT Is Good For Matching"): OpenCV SIFT keypoints + Lowe ratio test +
pairwise geometric consistency, integrated into a libfprint fork
(`github.com/goodix-fp-linux-dev/libfprint`, branch `sigfm`) with a
per-driver opt-in (`img_class->algorithm = FPI_DEVICE_ALGO_SIGFM`).

Evaluated on our capture corpus with the real libsigfm code:

- **Impostor pairs (left-index and left-middle vs enrolled right): 35 of
  36 pairs = 0, one pair = 7.**
- **Genuine pairs: 549 to 5,400,000.**

Weakest genuine ≈ 80x the strongest impostor. Threshold set to 100.

(One earlier capture batch, `leftA-D`, showed apparent impostor matches
in both NBIS and SIGFM — SIGFM's per-pair scores made it obvious those
were accidentally right-finger swipes; a controlled re-test with the
left middle finger — a finger never captured before — confirmed: 24/24
impostor pairs scored 0.)

**Live results through fprintd** (goodix sigfm fork + our elanspi patch,
threshold 100): enroll clean, **5/5 genuine verify-match, 0/3 impostor
matches.** Working, trustworthy fingerprint auth on PID 0x3104.

Also credit where due: the earlier NBIS false-accept scare (impostor 27 >
threshold 24) was real data on mislabeled captures — under NBIS those
right-vs-right pairs scored *in the impostor range*, which is itself the
clearest demonstration of bozorth3's non-discrimination on this sensor:
it couldn't even reliably match the same finger above other fingers.
