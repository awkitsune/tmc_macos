# Touch-control skin prototypes

Prototypes of an optional skin drawn around the iOS touch controls, in the spirit of a handheld's plastic shell
(see `port/notes/improvements.md`). They are original drawings: no logos, no trademarked names, no copied artwork.

| Design    | Look                                                          |
|-----------|---------------------------------------------------------------|
| `indigo`  | purple plastic, dark caps, lavender print                     |
| `outline` | strokes only, one colour, over the black screen               |

Each comes as `<design>-portrait.svg` (393 × 852 pt) and `<design>-landscape.svg` (852 × 393 pt): an iPhone 15
with its safe area, 3 pixels per point. Every cap sits on the touch target `port/src/ios/touchlayout.m` gives that
control for that screen. `generate.py` holds a copy of that layout and writes all four files; run it again
(`python3 port/design/skins/generate.py`) when the layout changes.

The app draws the buttons of `indigo` and `outline` (Settings > Controls > Touch > Button style, in
`port/src/ios/touchcontrols.m`): the wells, caps and labels, not the shell, grille or lens.

## Layers

Each SVG has these top-level groups, drawn in this order. An implementation would draw them the same way with
Core Graphics, or export each group as a PDF asset, and place them from the live `TouchLayout`.

| Group           | What it is                                                                                     | Placed from                    |
|-----------------|------------------------------------------------------------------------------------------------|--------------------------------|
| `shell`         | the plastic over the whole screen: body, edge, seam, the hinge in portrait | the view's bounds          |
| `grille`        | the speaker holes                                                                              | the free space under A and B   |
| `lens`          | the dark glass around the picture, with the power light                                        | `picture`'s fitted image, + 14 pt |
| `cutout`        | the game picture's area (240 × 160 at a whole scale); drawn black here, left clear in the app  | the fitted image rect          |
| `wells`         | recesses under the D-pad, A + B (a pill in portrait, rings in landscape), Start and Select                     | the controls' rects            |
| `caps`          | one group per control: `cap-dpad`, `cap-a`, `cap-b`, `cap-l`, `cap-r`, `cap-start`, `cap-select`, `cap-menu` | `controls[...]`  |
| `labels`        | the print: START, SELECT, L, R                                                                 | the controls' rects            |
| `touch-targets` | hidden (`display="none"`); the layout's touch targets, to check the caps against               | —                              |

Notes for an implementation:

- The shell, lens and wells are static per layout and can be drawn once into a layer; only the caps change, for
  the pressed state (darker fill, no drop shadow, moved down 1–2 pt).
- The cutout's text marks only the prototype; the app shows the game there.
- Colours are plain hex values per design in `generate.py` (`DESIGNS`), so they can become a small table in code.
