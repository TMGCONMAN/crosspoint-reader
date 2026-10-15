# Third-party notices

This fork of CrossPoint Reader (MIT, see [LICENSE](LICENSE)) adds the mesh
features, the Mesh Carousel home and the Apps. Their code was written for this
fork. It builds on, or credits, the following.

## Libraries built into the mesh firmware

- **NimBLE-Arduino** by h2zero, Apache License 2.0 —
  <https://github.com/h2zero/NimBLE-Arduino>. Bluetooth LE link to the
  companion radio. Fetched by PlatformIO; its license and NOTICE travel with
  the library sources.

## Protocols and references

- **MeshCore** companion radio protocol, MIT License —
  <https://github.com/meshcore-dev/MeshCore>. The firmware speaks the
  companion protocol over Bluetooth LE; the client in `lib/MeshCompanion` and
  `lib/MeshLink` is an independent implementation.
- **2048** was created by Gabriele Cirulli (MIT License,
  <https://github.com/gabrielecirulli/2048>). This fork's version is a new
  implementation of the game's rules; no code or art was copied.
- **Chess piece-square tables** in `lib/Games/Chess.cpp` follow Tomasz
  Michniewski's "Simplified Evaluation Function", published on the Chess
  Programming Wiki. Move-generation tests compare against the wiki's published
  perft counts.
- **Checkers** move-generation tests compare against published perft counts
  for the American checkers starting position.

## Fonts

The chess pieces (`src/components/icons/chess_pieces.h`) and the Chess app
tile are bitmaps rendered from **DejaVu Sans** glyphs. DejaVu fonts are based
on Bitstream Vera; DejaVu changes are in the public domain.

> Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera
> is a trademark of Bitstream, Inc.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of the fonts accompanying this license ("Fonts") and associated
> documentation files (the "Font Software"), to reproduce and distribute the
> Font Software, including without limitation the rights to use, copy, merge,
> publish, distribute, and/or sell copies of the Font Software, and to permit
> persons to whom the Font Software is furnished to do so, subject to the
> following conditions:
>
> The above copyright and trademark notices and this permission notice shall
> be included in all copies of one or more of the Font Software typefaces.
>
> The Font Software may be modified, altered, or added to, and in particular
> the designs of glyphs or characters in the Fonts may be modified and
> additional glyphs or characters may be added to the Fonts, only if the fonts
> are renamed to names not containing either the words "Bitstream" or the word
> "Vera".
>
> This License becomes null and void to the extent applicable to Fonts or Font
> Software that has been modified and is distributed under the "Bitstream
> Vera" names.
>
> The Font Software may be sold as part of a larger software package but no
> copy of one or more of the Font Software typefaces may be sold by itself.
>
> THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
> OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
> TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
> FOUNDATION BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
> ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
> WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
> THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE
> FONT SOFTWARE.
>
> Except as contained in this notice, the names of Gnome, the Gnome
> Foundation, and Bitstream Inc., shall not be used in advertising or
> otherwise to promote the sale, use or other dealings in this Font Software
> without prior written authorization from the Gnome Foundation or Bitstream
> Inc., respectively. For further information, contact: fonts at gnome dot
> org.

## Trademarks

Xteink, MeshCore and the names of other products are used only to describe
compatibility. This fork is not affiliated with or endorsed by their owners.
