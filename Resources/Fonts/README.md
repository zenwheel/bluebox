# Bitmap fonts

The compatibility System file's fonts are built from these faces by
`Generators/fonts.py`. None of them is Apple's: they are free or open
look-alikes with the classic fonts' metrics, under the classic family
numbers (0 system, 2 New York, 3 Geneva, 4 Monaco, 2002 Mac OS 8's
Charcoal) and our own names (Classic System, Classic Serif, Classic Sans,
Classic Mono, Classic Heavy).

| Stands in for | Sizes | Face | Source | Licence |
|---|---|---|---|---|
| Chicago | 12 | ChicagoFLF 1.0 | Robin Casady | public domain (see `chicagoflf/README.md`) |
| Chicago | 9 | Jarrah | Systemless | SIL OFL 1.1 (`systemless/OFL.txt`) |
| Monaco | 9, 10, 12 | ProFont 2.2 (Mac bitmaps with €) | Tobias Jung's ProFont distribution | MIT (`profont/LICENSE`) |
| Geneva | 9 | Geneva 9, with characters it lacks from Geneva 9.1 | Kelsey Higham (FontStruct) | CC BY 3.0 and SIL OFL 1.1 (`geneva9/`) |
| Geneva | 12 | bmpGeneva12, with characters it lacks from Kurrajong | uniqueidentity (FontStruct) | CC BY-SA 3.0 (`geneva12/`) |
| Geneva | 10, 14, 18, 24 | Kurrajong | Systemless | SIL OFL 1.1 |
| Charcoal | 12 | Charcoal 12 | Jeremy Sachs (FontStruct) | CC BY-SA 3.0 (`charcoal12/`) |
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
- `geneva12/bmpGeneva12.bdf`: "bmpGeneva12" by uniqueidentity
  (<https://fontstruct.com/fontstructions/show/455928>, CC BY-SA 3.0;
  archive SHA-256
  `491952949906018e789a9f62b2fb6dc1b5daf7a2cdba14c5aba7bd1d4300f293`),
  drawn at 16 pixels with Geneva 12's metrics (ascent 12, descent 3,
  leading 1, a 4-pixel space). The Systemless Geneva 12 supplies the
  characters it lacks.
- `charcoal12/Charcoal-12.bdf`: "Charcoal 12" by Jeremy Sachs
  (<https://fontstruct.com/fontstructions/show/2813611>, CC BY-SA 3.0;
  archive SHA-256
  `bdef9b2c035fae5cc623baa0f7f84058c8a33a2c8eb51293e3c7379f687ac2f7`),
  drawn at 16 pixels with Charcoal 12's metrics (ascent 12, descent 3,
  leading 1, as Mac OS 8's Charcoal has at 12 points). It covers Basic Latin;
  the generator synthesises the rest.

The CC BY-SA faces' derived strikes (the BDFs and the NFNTs built from them)
are under CC BY-SA 3.0 too, crediting their authors as above.

Characters a face lacks among MacRoman 0x80-0xFF are synthesised by the
generator from the face's own letters (accents, ligatures, dashes).
