# Experimental frame interpolation

This branch experiments with presentation-only interpolation toward 60 fps. It is
opt-in, supports only the interpreter and Vulkan, and needs more work before a
release. It does not change guest simulation or the native software frames.

Build with `-DFF8_FRAME_PROVENANCE=ON`, then run the executable with
`--interpreter --renderer vulkan --frame-interpolation on`. You can also enable
**Frame interpolation** in the **Video** menu. The default is off.

Interpreter provenance tags follow vertex data through registers, memory, GTE
projection and GPU packets. Per-corner identities match geometry across frames.
Unknown or ambiguous identities fall back to the current frame. Camera cuts,
mode changes and invalid history reset interpolation. An independent
correspondence oracle records projection inputs separately from the tags.
`FF8_RECORD_FRAME_ORACLE` enables that diagnostic build option;
`tools/verify_frame_correspondence.py` checks its captures against tag traces.
`tools/analyze_frame_tags.py` reports pixel residuals without certifying identity.

## Measured status

Battle and one verified fixed-camera field, `bgroad_4`, interpolate within a
conservative acceptance set. Other fields are unverified. World interpolation
is disabled because the CPU adds camera translation into vertex XYZ and clears
the GTE translation. The current correspondence rule cannot accept that path.

Measured pacing is about 58 Hz, with CPU p99 spikes above the frame budget.
Geometry adds about 67 ms of latency in battle and 33 ms in the verified field.
These are history delays, not physical controller-to-display measurements.
Effects remain at the current frame N, so they can disagree with delayed geometry.
See [effect timing](https://github.com/rash-pro/lionheart/issues/12).
With interpolation off, measured throughput is still about 4% below the baseline.
The visual quality is not ready for general use.

## What to work on next

Reduce CPU construction-frame spikes and the remaining off-mode cost. Improve
effect timing, verify more fields and develop a world correspondence adapter
before enabling those paths. Preserve fail-closed fallback and guest-state
exactness throughout. Track the work in
[presentation interpolation](https://github.com/rash-pro/lionheart/issues/4).
