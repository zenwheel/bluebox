# Bitmap fonts

The compatibility System file's fonts are built from these faces by
`Generators/fonts.py`. None of them is Apple's: they are free or open
look-alikes with the classic fonts' metrics, under the classic family
numbers (0 system, 2 New York, 3 Geneva, 4 Monaco) and our own names
(Classic System, Classic Serif, Classic Sans, Classic Mono).

| Stands in for | Sizes | Face | Source | Licence |
|---|---|---|---|---|
| Chicago | 12 | ChicagoFLF 1.0 | Robin Casady | public domain (see `chicagoflf/README.md`) |
| Chicago | 9 | Jarrah | Systemless | SIL OFL 1.1 (`systemless/OFL.txt`) |
| Monaco | 9, 10, 12 | ProFont 2.2 (Mac bitmaps with €) | Tobias Jung's ProFont distribution | MIT (`profont/LICENSE`) |
| Geneva | 9 | Geneva 9, with characters it lacks from Geneva 9.1 | Kelsey Higham (FontStruct) | CC BY 3.0 and SIL OFL 1.1 (`geneva9/`) |
| Geneva | 10, 12, 14, 18, 24 | Kurrajong | Systemless | SIL OFL 1.1 |
| New York | 12, 14, 18 | Ironbark | Systemless | SIL OFL 1.1 |
| menu symbols | | | Systemless | SIL OFL 1.1 |

Each third-party face is kept as a BDF strike whose `ENCODING` values are
MacRoman codes, made once with `Generators/import_bitmap_font.py` from the
original files (which are not kept here):

- `profont/ProFont-9.bdf`, `-10.bdf`, `-12.bdf`: the NFNT strikes 15089,
  21828 and 27905 of the `ProFont` suitcase in `ProFont-MacEuro.zip` from
  <https://tobiasjung.name/profont/> (zip SHA-256
  `2c79cb62b0b2a27f6af303acf5e68c8411bacd0175897e3d5ef68386e2285135`),
  converted as they are.
- `chicagoflf/ChicagoFLF-12.bdf`: `ChicagoFLF.ttf` version 1.0 (SHA-256
  `549a8d52b6e01614ac0319bb545b372779d5c7995611b453a56886996af22525`) drawn
  in black and white at 12 pixels, where its outlines fall on Chicago 12's
  pixels.
- `geneva9/Geneva-9.bdf`: Kelsey Higham's "Geneva 9"
  (<https://fontstruct.com/fontstructions/show/349277>, CC BY 3.0), a clone
  of the original Macintosh Geneva 9 made from MacPaint 1.4; its FontStruct
  OpenType file (SHA-256 of the downloaded archive
  `06a63e27454ee74de484213bec172e2dda74017a61d04cc8b6285b38eef23640`) drawn
  at 16 pixels, where each FontStruct square is one pixel.
- `geneva9/Geneva-9.1.bdf`: Kelsey Higham's "Geneva 9.1"
  (<https://fontstruct.com/fontstructions/show/1304775>, SIL OFL 1.1), the
  same clone with more accented letters (archive SHA-256
  `4b96b6998e1daffcd61d726660074ba6b54b1d69a7c4dd52d5280299e76838f0`),
  drawn the same way. Geneva 9's own glyphs come first; 9.1 supplies the
  characters Geneva 9 lacks. The archives' readme and licence files are kept
  beside them, as FontStruct asks.

Characters a face lacks among MacRoman 0x80-0xFF are synthesised by the
generator from the face's own letters (accents, ligatures, dashes).
