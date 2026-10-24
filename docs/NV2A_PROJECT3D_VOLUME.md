# Projective 3D texture coordinates

NV097 texture mode 2 with a non-cube dim=3 color texture consumes interpolated XYZ/Q. The original per-stage coordinate path interpolates all four components; software passes the divided XYZ to the existing volume sampler and W addressing. Native draws select the existing Texture3D resource and projected shader identity, dividing XYZ by W in the original combiner emitter.

The existing volume extent, DMA checks, Morton decoder, filter, cache identity, upload and sampler are reused. The original mode 2 integer shadow path and 2D behavior retain their consumers. No constant-buffer or persistent state size changes.

Checks cover all four stages and eight distinct XYZ corners, positive and negative Q, and varying positive Q across triangle vertices. Zero/nonfinite Q behavior is not defined by this connection or certified to reject identically on all GPU fragments; no substitute Q, clamp or discard is introduced for 3D.

Source: cached xemu glsl_psh.c PROJECT3D uses textureProj(pT.xyzw). This implementation validates legal original requests, without claiming an observed whole-title dim=3 mode2 draw.
