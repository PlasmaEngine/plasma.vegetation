# Forest cell tests

CPU checks of how vegetation cells draw types whose first LOD mesh is a foliage composite. They need no renderer or window.

- `ClassifyTree`: a tree shows its parts within the switch distance times its scale, its merged mesh beyond, and nothing past the cull distance; the shadow bit follows the shadow cull distance and the NoShadow flag; Vegetation.DensityScale drops non-hero trees by colour seed.
- `SplitOnlyChangesWhenATreeCrosses`: `plVegetationCompositeSplit` reports a change only when a tree crosses a threshold, and does no per-tree work while a whole cell stays on one side of every threshold.
- `SwitchDistance`: the distance where the merged mesh's error projects to the pixel threshold.
- `PartSwayBound`: the bound a part instance's culling radius is grown by covers `plFoliageSkeleton::ComputeDisplacementBound` for every point of the part.
- `SplitCost`: logs the per-cell, per-frame cost of the split for a far cell and a straddling cell.
- `BonePoolIdTravelsWithSwap`: a tree's bone pool id stays with it when cells remove instances by swapping.

From the package root, with `PLASMA_SDK_ROOT` pointing to a built engine SDK, run `plasmabuild build ForestCells`, add the SDK's `Binaries/Windows_Development` directory to PATH and run `Bin/Windows_x64_Development/ForestCellsTest.exe -noGui -nosave -all -close`.
