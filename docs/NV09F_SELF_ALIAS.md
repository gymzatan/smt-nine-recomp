# Exact pixel self-alias in NV09F

The existing opaque X8 ROP_AND and SRCCOPY_AND consumers accept identical physical source and destination starts with equal row pitches. The original row limit ensures each row contains no more pixels than that pitch, so each output depends only on its own prior pixel. Neither traversal order nor temporary image storage is required. The original copy and shared color_logic functions remain the consumers.

For a pattern-independent ROP nibble, S=D means only truth inputs 00 and 11 occur. Those two bits select zero, the previous pixel, its inverse, or ones. The existing opaque X8 output alpha rule still applies. SRCCOPY_AND retains the same RGB and writes the established opaque alpha.

The ordinary ROP CC source-copy subset also supports shifted same-pitch linear overlap, using the original copy path and choosing the row direction from resolved RAM addresses. See [BLIT_LOGIC_CONSUMERS.md](BLIT_LOGIC_CONSUMERS.md). Shifted overlap for other ROP functions or SRCCOPY_AND, different pitches, nonempty key/pattern, alpha-bearing formats and partial-pixel completion retain their explicit boundaries. This is not a general hardware overlap-order claim, a new tile decoder or an observed whole-title self-blit claim.
