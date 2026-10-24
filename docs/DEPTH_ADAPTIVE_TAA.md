# Depth-Adaptive Temporal Anti-Aliasing (TAA)

**Status:** Superseded by the unified temporal reprojection and FSR reconstruction pipeline.

## Historical Summary

This document describes earlier experiments evaluating standalone depth-adaptive TAA passes. The core insights—specifically handling velocity disocclusions via neighborhood depth clipping and sub-pixel camera jitter accumulation—have been merged directly into the unified motion vector pipeline (`docs/motion_vectors.md`) and the compute-based TAA provider (`docs/upscaler.md`).
