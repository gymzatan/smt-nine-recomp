# Legacy texture-coordinate and fog inputs

The four NV097 TEXCOORD 2F/4F/2S/4S families write standing attributes 9 through 12. Their bases are listed explicitly because texture coordinate 3 has a different gap before 4F. The existing generic immediate consumers perform the writes and the existing position consumer emits vertices.

2F supplies z=0 and w=1 on every component write; 2S decodes signed raw shorts and supplies the same defaults. 4F and 4S preserve the components not written, with 4S using raw signed shorts rather than normal-vector normalization. FOG_COORD copies its float into all four components of standing attribute 5.

Contracts are the pinned xemu handlers (d6cb7536a2acf0dcc41934a627a0d63a5700ec76) and original pbkit++ producers (72600fab67a9972fe2d3a1c6937f64e99dfa5290). This connects existing entry points; it does not change texture sampling, DOT shader stages or depth replacement.
