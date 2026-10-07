# lisa-cache

The LISA library compiled with `--tt07-cache`: the same sources as
`../lisa` (configure writes `../lisa/Makefile.in` into this directory
too, and the Makefile tells the two apart by its directory name), with
the TT07 data-cache workaround - a `cmp` of each byte before an `inx` /
`dcx` - in the compiled C.  `sdcc -mlisa --tt07-cache` links
`lib/lisa-cache/lisa.lib` (the port's `get_model`), plain `-mlisa`
`lib/lisa/lisa.lib`.

This directory only has to exist for the sources to be found through
`../lisa`.
