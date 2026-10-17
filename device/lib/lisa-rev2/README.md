# lisa-rev2

The LISA library compiled with `--rev2`, for the second chip
(`ttsky26c-um-lisa-ttlc`, whose `RESPIN.md` lists what its silicon fixes):
the same sources as `../lisa` (configure writes `../lisa/Makefile.in` into
this directory too, and the Makefile tells the three apart by its
directory name).  What differs: `bf16_add` is the chip's adder (`fadd`,
correct on this chip: `bf16add.s` under `LISA_REV2` from `lisamodel.inc`)
instead of the software addition the TT07 chip needs, so `--bf16-float`'s
`__fsadd`/`__fssub` run on it too.  `sdcc -mlisa --rev2` links
`lib/lisa-rev2/lisa.lib` (the port's `get_model`), plain `-mlisa`
`lib/lisa/lisa.lib`, `--tt07-cache` `lib/lisa-cache/lisa.lib`.

This directory only has to exist for the sources to be found through
`../lisa`.
