# Motion Vectors and Temporal Reconstruction

## Overview

Bloodborne does not produce a native velocity buffer. To enable high-quality temporal upscaling (AMD FSR 3.1, FSR 4, and TAA), the port reconstructs motion vectors dynamically on the GPU:
- **Camera Motion Vectors**: Derived analytically per-pixel using depth buffer reconstruction, current and previous view-projection matrices, and sub-pixel camera jitter offset compensation.
- **Object Motion Vectors**: Extracted from dynamic vertex streams (skinned meshes, cloth, characters, and weapons) by capturing transform states across frames.

## Key Pipeline Elements

- **History Preservation**: History tracking evaluates dynamic vertex buffer layouts, indices, vertex ranges, and instance streams. Topology changes or discarded frames gracefully reset tracking.
- **Occlusion Handling**: Alpha channels in intermediate targets track depth thresholds. If a late opaque draw occludes an earlier dynamic object, the camera vector takes precedence to avoid smearing artifacts.
- **Memory Management**: Coherent host-visible staging memory and vertex-to-vertex synchronization barriers ensure race-free vector generation before downscaled G-buffer resolves.
