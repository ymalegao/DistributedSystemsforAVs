# Protocol source layout

The protocol implementation is being moved here from the vendored Veins tree
without a namespace change. During the staged migration the compatibility
build still consumes `veins-veins-5.3.1/src/veins/modules/application/resDB`;
the source split and Veins patch are finalized only after a structure-only
build proves equivalent behavior.
