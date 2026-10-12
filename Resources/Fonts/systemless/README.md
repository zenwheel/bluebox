# Bitmap font artwork (SIL Open Font License 1.1)

These files are the hand-drawn bitmap faces from the Systemless project
(https://github.com/benletchford/systemless), taken at commit
`0540ef827d800fbdb00eafc67922cef3245b81e5` from `src/quickdraw/fonts/pixel_font/`.
That project's `OFL.txt` (copied here) states that the bitmap font artwork in
that directory is licensed under the SIL Open Font License 1.1, separately from
the rest of the project. Copyright (c) 2026 Ben Letchford, with Reserved Font
Name "Systemless".

Only the glyph data is used: `Generators/fonts.py` reads these files and builds
`NFNT`/`FOND` resources for the compatibility System file. The derived fonts do
not use the reserved name; they are called Classic System, Classic Sans,
Classic Mono and Classic Serif. Their metrics match the classic Mac system fonts
so text lays out as it did; the letterforms are original.

| File | Face | Stands in for |
|---|---|---|
| chicago9.rs, chicago12.rs | Jarrah | Chicago (system font) |
| geneva9/10/12/14/18/24.rs | Kurrajong | Geneva |
| monaco9/10/12.rs | Mallee | Monaco |
| newyork12/14/18.rs | Ironbark | New York |
| menu_symbols.rs | | Command key, check mark, trademark glyphs |
