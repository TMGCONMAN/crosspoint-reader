# Apps (X4 Pro mesh build)

The `x4pro-mesh` build adds an **Apps** screen. With the **Mesh Carousel**
home (Settings > Display > UI Theme > Mesh Carousel), the Apps icon takes the
Library's place on the bottom row; the Library is the first tile inside.
Tap a tile, or move with the buttons and press Confirm. Back returns home.

Each app keeps its state on the SD card, so a game survives leaving it and
sleep.

| App | Saved to |
| --- | --- |
| Notes | `/notes/*.txt` |
| Chess | `/.crosspoint/games/chess.bin` |
| Checkers | `/.crosspoint/games/checkers.bin` |
| Sudoku | `/.crosspoint/games/sudoku.bin` |
| 2048 | `/.crosspoint/games/2048.bin` |
| Calculator | nothing (the sum is kept until you leave) |

## Notes

Notes are plain text files in `/notes`, one paragraph per line, so they can
also be written on a computer or moved with File Transfer. The list shows
each note by its first line, newest first.

- **New note** opens a full-screen editor: Enter starts a new line, words
  wrap, and **Done** or **Back** saves.
- **New checklist** starts with a title and a first `[ ] ` item; Enter on an
  item starts the next one, and Enter on an empty item ends the list.
- In a note, tap a line for **Edit**, **Add below**, a checkbox, **Tick** or
  **Delete line**; tap a checkbox to tick it. **Menu** has **Add at the end**,
  **Edit the whole note** (up to 8 KB) and **Delete note**.
- A line starting with `[ ] ` or `[x] ` (or `- [ ] ` from Markdown) is a
  checklist item. Notes over 48 KB are not opened.

Every change is saved at once.

## Calculator

C, ( ), Del, the digits, + − × ÷, ±, decimal point, % and Ans. × and ÷ come
before + and −; open parentheses are closed for you; x% is x ÷ 100. The
display shows the value as you type and the last results.

## Chess

Play against the computer as White or Black. Tap a piece to see where it can
go (a dot, or a ring around a capture), then tap the square. All rules are in,
including castling, en passant, promotion (you choose the piece), and the
draw rules. **Undo** takes back your move and the computer's reply.

| Level | How it plays |
| --- | --- |
| Beginner | Looks one move ahead, misses hanging pieces, often plays at random |
| Easy | Looks one move ahead and sees captures through |
| Medium | Searches four half-moves, up to 3 s |
| Hard | Searches as deep as 6 s allows |

## Checkers

American checkers (English draughts) against the computer. Jumps are
compulsory and the pieces that must jump are framed. For a multiple jump, tap
the last square, or each hop when two routes meet; Back steps back a hop.
Draws come after 40 moves each without a jump or a man moving, or a position
repeated three times.

## Sudoku

Easy, Medium and Hard puzzles with one solution each. Tap a cell, then a
number; **Notes** pencils in candidates; **Undo** steps back. The time and
your best time for each level are kept.

## 2048

Swipe (or use the buttons) to slide the tiles; equal tiles merge. The best
score is kept.
