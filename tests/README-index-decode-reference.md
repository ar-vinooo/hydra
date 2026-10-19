# Guest primitives and index buffers: Python model vs IndexCache

Metal cannot draw every guest primitive type (line loops, triangle fans,
polygons, quads, quad strips), cannot read 8-bit indices and restarts strips only
at the maximum index. `IndexCache::decode` turns such draws into plain list
draws. `index_decode_reference.py` decomposes the same primitives with the
standard rules (the NVIDIA `BEGIN_OP` names, audited against `clb197.h`) and
compares the decoded indices, primitive type and count with Hydra's output.

```sh
ninja -C build/swiftui-release -j 6 hydra-index-decode-reference-tests
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/clb197.h -o /tmp/hydra-nvidia-clb197.h
python3 tests/index_decode_reference.py build/swiftui-release/bin/hydra-index-decode-reference-tests \
  --header /tmp/hydra-nvidia-clb197.h
```

## Covered (20 October 2026: PASS, 2500 cases)

Points, lines, line loop, line strip, triangles, triangle strip, triangle fan,
quads, quad strip and polygon, each without indices and with 8-, 16- and 32-bit
indices, with the restart off, enabled at a small index that occurs in the data,
or enabled at the maximum index of the type; 0-29 vertices, so incomplete
primitives and empty draws occur. Decoded draws must match the standard
decomposition vertex for vertex (including the winding of strips, fans and
quads); draws Hydra leaves alone must be equivalent for Metal (lists without
restart, strips without restart or restarted at the maximum index).

Controls: reversing the fan winding in the model fails the fan and polygon cases;
declaring list draws never equivalent fails every passthrough case.

## Conventions

Standard OpenGL / D3D assembly rules, which is also what the hardware does for
these primitives: the vertex order of strips alternates, quads split into
(0,1,2) and (0,2,3), a quad strip quad `i` uses vertices `2i, 2i+1, 2i+3, 2i+2`,
a restart index ends the primitive being assembled and discards an incomplete
list primitive. The provoking vertex of flat shading is not modelled.

Not covered: adjacency and patch primitives (geometry and tessellation are not
supported), the base vertex and instancing of the draw itself, and
the memory side (`BufferCache` coherence for passed-through indices).
