# Changelog (X4 Pro mesh fork)

Releases of this fork are tagged `<CrossPoint version>-mesh.<n>`, for example
`1.6.5-mesh.1`, and carry the X4 Pro firmware as
`crosspoint-<tag>-x4pro.bin` with its SHA-256.

## 1.6.5-mesh.1

Based on CrossPoint Reader 1.6.5 (`develop` at d7fcc7c). X4 Pro only, English
UI. Tested on hardware with a MeshCore companion radio.

### Mesh (MeshCore companion over Bluetooth LE)

- Pair with a MeshCore companion radio; reconnects on its own.
- Channel chat and direct messages, with message history on the SD card.
- Contacts, favourites, quick replies and contact/channel QR codes.
- Public, hashtag and private channels; join, share and leave.
- Radio settings (name, power, frequency, position), repeater and room
  server login and status, and node discovery.
- See [docs/mesh.md](docs/mesh.md).

### Mesh Carousel home

- A home screen with a book carousel, a mesh status and unread-message
  banner, and the Cover Grid's tab row with an Apps icon.

### Apps

- Notes (multi-line editor, checklists), Calculator, Chess and Checkers
  against the computer, Sudoku and 2048. See [docs/apps.md](docs/apps.md).

### Covers

- Covers are found in books that do not declare one (cover-named images and
  pages, the first pages), a `Book.jpg` next to `Book.epub` is used as its
  cover, and books without any image get a placeholder with their title. A
  report of covers that could not be made is written to
  `/.crosspoint/cover-report.txt`.

### Updates

- The mesh firmware only offers updates from this fork's own releases, never
  the official CrossPoint ones (which do not have the mesh features).
