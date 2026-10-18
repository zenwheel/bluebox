# ChicagoFLF

ChicagoFLF is a free recreation of Susan Kare's Chicago, made by Robin
Casady (Casady & Greene). Its author placed it in the public domain:

> I created ChicagoFLF and CheckboxFLF. You are free to use them as you wish.
> I consider those two in the public domain.
>
> Robin Casady, quoted in the pkgsrc package `wip/casady-fonts`
> (<https://pkgsrc.se/wip/casady-fonts>)

Some font sites list it as "free for personal use"; those labels are the
sites' own and are not from the author. The font file's name table carries
"(c)1990-92 by Richard A. Ware", which predates the release as ChicagoFLF.

`ChicagoFLF-12.bdf` is version 1.0 of `ChicagoFLF.ttf` drawn in black and
white at 12 pixels (see `../README.md`). Later builds (2.100, from FontForge)
move symbols to Unicode positions and are not the one used here.

Changes from that drawing, so that text lines up as Chicago's did (glyph
positions and the strike's metrics only; the shapes are ChicagoFLF's):

- 'A', 'f', '%', '£', '‰' and the letters ä ã å ë ï ñ ö õ ü Ø ø œ Â Ê Î Ô Û
  stood a row above the baseline; they now sit on it.
- 'g', 'j' and 'y' have a tail three rows deep, a row of stem longer; 'g'
  had also sat a row high.
- '$', '[' and ']' reach a row below the baseline; `\`, '}', '_', '¿' and
  '¡' and the quotation marks, '°', '¢', '®', '©', '™', 'ª', 'º', '«' and
  '»' moved a row to where Chicago had them.
- One pixel of leading between lines (X_CLASSIC_LEADING), as Chicago 12 has.

The real Chicago is Apple's and is not part of Blue Box; `bluebox fonts
import` brings it in from the user's own System disk image instead.
