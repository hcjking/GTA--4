# Next test run (built 10-27, not run yet)

One ~60 s run of `index.html?arg=--mnk_controller_emulation=false` answers all of these.

1. **Cutscene speed.** Log line `web timing: N updates in X s real, game time Y s (Z x)`.
   `Z` near 1.0 means game time is right and the 2x cutscene is elsewhere (audio).
   `Z` near 2 confirms game time runs fast. (Web vblanks are now forced to 60 Hz.)
2. **Stream decode rates.** `web xma contexts: ctxN RATE Hz xCH decoded S/s`: a context
   decoding about twice its rate is a stream consumed too fast.
3. **Black geometry spikes.** SHORT2/SHORT4/UBYTE4 inputs are now unnormalized integers.
   Capture 16 consecutive frames (flick-sheet method) and compare with the earlier sheet.
4. **Stencil, cull, scissor, depth bias, blend constant** now applied. Watch for lights
   (deferred light volumes) and any new pipeline errors in `scene3dErrors`.
5. **Boot screens.** At 5-15 s run `Scene3D.describeFrame()` a few times: which draws
   exist, their shaders, targets and texture formats, and the front buffer.

6. **PS5 hub (new, built):** the page should open on the PS5 launcher (Home / Display /
   Game / Episodes / Saves / Trophies / About) with its picture, logo and menu music.
   Keys: arrows move, Enter = Cross, Esc = Circle, Q / E = L1 / R1 (tabs), O = Options;
   a gamepad works too. START GAME should load the game as before.
   `?arg=--web_hub=false` skips the hub (old behaviour). Things to check: `webUiStats`
   (draws, textures), the log for "PS5 hub:" lines (fonts, pictures, music), and
   `webUiError`. The server must be the updated serve.py (serves /app0 from ps5/).
7. **Replay speed:** the page now caches pipelines by a numeric key and uses explicit bind
   group layouts (constants by dynamic offset, texture/sampler groups cached). Measure page
   fps as before (wrap `Scene3D.render`); it was ~21 fps / 42 ms per frame. If something
   breaks, `scene3d-v2.js` in the link folder is the previous version.
8. **Game-thread cost:** constants are now stored once per distinct block (was ~37 MB of
   copying a frame), only the texture stages a shader uses are looked up, and the old crash
   probes are off (`--web_crash_probe=true` brings them back). The `web: frame N ... game X ms
   per frame` log line was ~40 ms; it should drop a lot.
9. **Textures:** DXT1/3/5 and DXN now upload as BC compressed when the device has
   `texture-compression-bc` (log: "compressed (BC) textures on"; `?nobc` turns it off), with
   full mip chains for power-of-two textures (RGBA ones too), per-stage sampler state (wrap /
   clamp / mirror, point / linear, mip filter, anisotropy), and small textures' packed-mip
   offset fixed. Look for sharper distant textures, no shimmer, and any texture that came out
   wrong (compare with `?nobc`).
