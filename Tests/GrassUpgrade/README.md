# Grass upgrade tests

From the package root, with `PLASMA_SDK_ROOT` pointing to a built engine SDK, run `plasmabuild build GrassUpgrade`. Add the SDK's `Binaries/Windows_Development` directory to PATH and copy its `D3D12` runtime directory alongside the test executable if it is not already deployed there. Create `tmp/grass-shadercheck` under the SDK root for writable shader caches and screenshots.

Run `Bin/Windows_x64_Development/GrassUpgradeTest.exe -noGui -nosave -all`. The suite requires a DX12-capable adapter and the SDK's shader compiler and renderer plugins.

Add `-grassGpuValidation -noGpuValidation` to enable the D3D12 debug layer without GPU instrumentation. Omit `-noGpuValidation` for GPU-based validation, which can compile shaders substantially more slowly. Offscreen rendering uses masked grass with multithreaded extraction/recording; the debug-layer run also checks the Hi-Z mip transitions across worker and main command encoders.

The CPU checks cover nested deterministic root residency, clump correlation, descriptor version compatibility, serialization and settings sanitization. GPU readback checks verify indexed indirect arguments for all three LODs in mono and stereo. Offscreen rendering checks near, middle and distant procedural grass, a vertical surface, and allocation/release of simulation state. The render checks assert visible coverage; inspect `tmp/grass-shadercheck/grass-shot-*.png` for appearance.

A painted-layer streaming regression fills a one-page pool with two distant tiles, then moves the camera back and forth. It verifies that the nearby tile replaces the far resident even after the pool has refused an earlier upload.

Its adaptive-density variant reads the GPU root buffer back and verifies that both tiles remain populated while the denser population follows the camera, within the same one-page budget.

Add `-grassStress` for a separate large-field run: 524288 painted roots, full-detail blades, a lit main view and a shadow view, with async-compute support enabled at device creation. It repeatedly forces pipeline rebuilds to catch culling being sorted behind the draws. This replaces the small rendering/streaming cases; run both modes. The CPU suite also verifies that vegetation and grass blobs remain self-contained when serialized or read inside a scene's string-deduplication context, and that the outer context is restored afterward.

DX12 execution does not validate Vulkan rendering. Compile the four shaders in `Assets/Shaders/Vegetation/Grass` using the SDK ShaderCompiler for both DX12 and VULKAN when changing their shared layouts or shader code.

The normal run also renders material AO: shot 4 verifies dark roots and unoccluded tips at a grazing view; shots 5 and 6 verify uniform distant-cover AO on ground and a wall; shot 7 verifies that disabling both procedural AO strengths restores fully unoccluded grass. These check the final material AO path, including the shared material helper's view-angle handling.

Shots 8 and 9 verify that `Vegetation.Grass.AO.Mode = 0` disables near-root and distant-cover procedural AO with nonzero material strengths; shot 4 runs in Grass Only mode.

Run with `-grassContactAO` for Grass Surface receiver tests: a controlled AO input with existing occlusion must darken around roots on a ground plane and wall, preserve auxiliary channels, switch off ground contact in Grass Only/None modes, reject a receiver raised above the roots, work in perspective, and respect view exclusions. Add `-grassContactAuto` to test a painted Vegetation Layer and automatic insertion after a real AO Pass (its intensity is zeroed to isolate the grass contribution). Both paths force pipeline rebuilds while changing modes. Receiver tests disable the unrelated mesh GPU culler because its Batch Table barriers currently fail DX12 validation; the grass GPU culler and D3D12 debug layer remain enabled.

### Contact AO performance replay

`-grassAOReplay <fixture-folder>` replaces scene rendering/streaming checks with an isolated replay of captured contact AO draws. The fixture contains `globals.bin` (the captured `plGlobalConstants` bytes), `draws.bin` (one 32-byte `plGrassContactAOConstants` per draw), `roots.bin`, `pages.bin`, `sources.bin`, and tightly packed `depth-d24.bin`. Dimensions come from the global viewport constants. The current replay format supports mono perspective D24S8 captures.

For a before/after comparison, place the original quad shader at `tmp/grass-shadercheck/GrassContactAOBaseline.plShader`. Run without `-grassAOReplayVolumes` for the baseline, then with that flag for the optimized indexed volumes. Each run collects at least 20 GPU timestamp samples after warm-up and writes an AO image to the shader cache. The optimized run compares against the baseline and asserts that significant pixel differences affect at most 1% of the original AO pixels plus a 32-pixel rasterization allowance. Use GPU validation for correctness; avoid GPU instrumentation for timing. Capture fixtures are local and are not distributed with the package.
