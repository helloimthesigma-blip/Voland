# Fonts

`NotoSans-Regular.ttf` is Noto Sans by the Noto Project Authors, under the
SIL Open Font License 1.1 (`OFL.txt`). It is what `pl:u` serves as every
system font (Voland never ships Nintendo's fonts).

This is a **modified version**: `tools/fonts/add-fallback-glyphs.py` adds
U+25A1 (□) and U+25A0 (■), drawn by Voland, because titles use □ as their
missing-glyph box and expect the system font to have it.
