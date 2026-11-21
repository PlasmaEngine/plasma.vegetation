# Vegetation CPU tests

Headless tests of the vegetation runtime that need no GPU, window or engine process: the Poisson-disk scatter used by Paint and Fill (spacing between and across types, spacing to existing plants, density, clearings with avoidance, region mask, accept callback, cap, determinism, a 500 m x 500 m forest at 600 trees per hectare), the Vegetation Type file versions (version 2 loads with the version 3 defaults), layer clearings and the stroke request versions.

From the package root, with `PLASMA_SDK_ROOT` pointing to a built engine SDK, run `plasmabuild build VegetationTests`. Add the SDK's `Binaries/Windows_Development` directory to PATH and run `Bin/Windows_x64_Development/VegetationTests.exe -noGui -nosave -all -close`.
