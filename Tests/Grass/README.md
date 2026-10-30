# Grass tests

`GrassTest.cpp` tests the CPU side of `plGrassSurfaceComponent`: strand counts from mesh area and the Grass Type's strands per m^2, deterministic regeneration and strand packing, vertex-length masks, pool page bookkeeping in `plGrassGpuManager`, and scene serialization. The Grass Type is created in code from a `plGrassTypeResourceDescriptor`. Simulation, culling and drawing run on the GPU and are not read back. `GrassMaterialTest.cpp` renders the grass material offscreen.

The tests are kept with the package because they test package behavior. `CMakeLists.txt.reference` records the upstream test dependencies without making PlasmaBuild treat the old CMake target as a package target. These legacy tests are not wired into a build target. The separate [GrassUpgrade suite](../GrassUpgrade/README.md) has a runnable PlasmaBuild target covering the procedural renderer and residency changes.

`GrassCoverageTest.cpp` tests `plGrassCoverageData` (paint, erase and thin round-trips, empty-tile removal, delta merging, resampling and deterministic strand counts). It needs no renderer or physics and registers in the same `Grass` test group.
