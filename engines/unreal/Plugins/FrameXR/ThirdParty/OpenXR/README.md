# ThirdParty/OpenXR

`include/openxr/` holds three headers from the OpenXR SDK **1.1.57** (Khronos, `Apache-2.0 OR MIT`, unmodified):
`openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h`. They are vendored because the engine's own
`Engine/Source/ThirdParty/OpenXR` headers (1.1.46 in UE 5.8) lack `XR_KHR_metal_enable`. `FRAMEXR_OPENXR_INCLUDE`
at build time points the plugin at another copy.

`lib/` is where the loader goes (gitignored): copy `libopenxr_loader.dylib` from the OXRSys build tree here, or set
`FRAMEXR_OPENXR_LOADER` to its path at run time. The plugin stays dormant without one.
