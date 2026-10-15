# MeshCore companion support (X4 Pro)

This fork adds a **Mesh** screen that links an Xteink X4 Pro to a
[MeshCore](https://github.com/meshcore-dev/MeshCore) companion radio over
Bluetooth LE. The X4 Pro has no LoRa radio; it acts as the app, the way the
MeshCore phone app does, and the companion radio stays the mesh node.

The feature is built only by the `x4pro-mesh` environment. Every change in
shared code is behind `CROSSPOINT_MESH`, so the other environments build the
reader as before. The one exception is the UI string table, which grows by
about 4 KB in every build.

> **Status: tested on hardware.** The fork's owner has run the mesh
> features on an X4 Pro with a LoRa companion radio: pairing and
> reconnecting, channel chat, direct messages, contacts and favourites,
> message history, radio settings, servers, discovery and the Mesh Carousel
> home banner. The [hardware test plan](#hardware-test-plan) below is kept
> for checking future changes.

Checked so far: the 113 host tests pass, and `x4pro-mesh` and `x4pro` build
and link. Not checked: the ESP32-C3 environments (`default` and the release
profiles) were not built after the shared-code changes, and `pio check` was
not run.

## What this build does

- Scans for companion radios and pairs with one, asking for its PIN.
- Remembers the radio and reconnects when the Mesh screen is opened.
- Runs the protocol handshake: radio details, clock, contacts, channels and
  any messages the radio has queued.
- Lists the radio's channels and the direct-message conversations, each with
  its newest message and a count of unread messages. The count starts when
  the Mesh screen is opened and is cleared when the chat is opened.
- Opens a channel or a conversation as a chat: the last 40 messages, new
  ones as they arrive, and a **Write a Message** row that opens the keyboard.
- Sends direct messages and follows them to **Delivered**, trying up to
  three times.
- Lists the radio's contacts, favourites first, and starts a conversation
  with any person among them.
- Marks and unmarks favourites. The mark is the radio's own, so favourites
  set in the MeshCore phone app show here, and the other way round.
- Keeps channel and direct messages in history files on the SD card, and
  pages back through them from any chat.
- Shows the radio's name, model, firmware, frequency, bandwidth, spreading
  factor, TX power and battery voltage.
- Sends a zero-hop advert.

Not built yet:

- Reading the history while no radio is linked.
- Rooms and repeaters: they are listed as contacts, but logging in to them
  is not built.
- Adding, removing or sharing contacts, and changing radio settings.
- Favourites on the Mesh screen: they lead the contact list only.
- Keeping the link up outside the Mesh screens. Messages that arrive while
  another screen is open wait on the radio, which holds 16.

The radio does not keep a chat history; the phone app keeps its own. This
device shows only what it received or sent itself. Conversations from the
phone do not appear here.

## Build and flash

```sh
pio run -e x4pro-mesh -t upload
```

The device must already run CrossPoint. See the main README for the first
install and for the pinned PlatformIO core.

`platformio.ini` pins `h2zero/NimBLE-Arduino @ 2.5.1`, the version this was
compiled against (taken from the library's Git tag). If the PlatformIO
registry does not offer that exact version, pin the nearest 2.x release it
has and rebuild.

`x4pro-mesh` builds the UI in English only (`custom_i18n_builtin_langs = en`
in `platformio.ini`). The other 34 languages take about 395 KB, and with
them mesh support fills the app slot to 94.6%. To build more languages in,
list their codes there. A device whose saved language is not built in shows
English. Hyphenation of book text is separate and still covers every
language.

Measured on the commit that made the mesh build English only:

| Environment | Flash used | App slot | Static RAM |
| --- | --- | --- | --- |
| `x4pro`, all languages | 5,947,626 B (90.8%) | 6,553,600 B | 103,696 B |
| `x4pro-mesh`, all languages | 6,196,702 B (94.6%) | 6,553,600 B | 113,484 B |
| `x4pro-mesh`, English only | 5,802,166 B (88.5%) | 6,553,600 B | 113,484 B |

Mesh support costs about 249 KB of flash and 10 KB of static RAM. The
English-only build leaves about 751 KB free in the app slot. Upstream
`x4pro` at the fork point was 5,943,738 B, so the shared strings add about
4 KB to builds without mesh.

Estimated from the sizes of the structures, not measured. While the Mesh
screen is open the heap holds about 12 KB for the service, 72 bytes for each
contact the radio can hold (25 KB for 350), and about 16 KB for the Mesh
screen. An open chat adds about 9 KB, the history reader 9 KB more, and the
contact list 43 bytes for each contact. Heap use of the BLE stack has not
been measured either.

## Using it

1. Put a MeshCore radio with **BLE companion** firmware nearby and close the
   MeshCore phone app. The radio serves one app at a time.
2. Open **Mesh** from the home menu and choose **Scan for Radios**.
3. Tap the radio. Enter its PIN when asked. Radios without a screen use
   `123456`, which is prefilled. Radios with a screen show a PIN that changes
   each session unless a fixed one was set.
4. The screen shows **Connecting**, then **Reading radio**, then the radio's
   channels and details.
5. Tap a channel to open it. The last row, **Write a Message**, opens the
   keyboard. A channel message can be as long as the radio allows: 160 bytes
   minus the node name and two more.
6. To write to a person, tap **Contacts** under **Direct Messages** and
   choose them. A direct message can be 160 bytes long. Once a conversation
   has a message, it is listed on the Mesh screen, most recent first.

In a chat, each message shows its sender and the time the sender's radio
stamped on it; messages from another day also show the month and day. Your
own messages show their state:

| State | Meaning |
| --- | --- |
| **Sending...** | The radio has not taken the message yet. |
| **Not sent** | The radio refused it, or the link was down. |
| **Sent** | Direct messages only: on the air, no answer yet. |
| **Delivered** | Direct messages only: the recipient's radio acknowledged it. |
| **Not confirmed** | Sent, but no acknowledgement came back. It may still have arrived. |

A direct message is tried up to three times. If the earlier tries used a
stored route, the last one clears that route so the radio floods the
message. Channel messages have no acknowledgement in MeshCore: once the
radio takes one, it shows no state at all.

Tap a **Not sent** or **Not confirmed** message to open the keyboard with
its text.

### Favourites

The contact list shows **Favorites** first and **Other Contacts** after
them; in each group people come before rooms and repeaters. To change a
contact's mark, press and hold its row, or hold Confirm on it, and choose
**Add to Favorites** or **Remove from Favorites**. The list moves the
contact once the radio has saved the change.

The radio keeps the mark, in the first bit of the contact's flags, which is
where the MeshCore apps keep it. The radio also never drops a favourite to
make room for a new contact when its table is full.

Leaving the Mesh screen drops the link and shuts Bluetooth down a few
seconds later; the chat, contact and history screens opened from it keep the
link, and so does Home with the **Mesh Carousel** theme (below). While the link is
down the radio keeps up to 16 messages. Beyond that it drops the oldest
channel message, or the new message when only direct messages are queued.

### Chat tools

- **Quick Replies**, the last row of a chat, lists canned answers; choosing
  one sends it at once. They are read from
  `/.crosspoint/mesh/quick_replies.txt`, one per line (up to 8, 95 bytes
  each). The file is written with a default set the first time a chat asks
  for it; edit it on a computer. A reply longer than the conversation allows
  is left out.
- **Message details**: press and hold a message, or hold Confirm on it, for
  its sender, full date and time, route (direct route, heard directly, or the
  hop count of a flood) and SNR. An outgoing message shows its state, and
  **Send Again** when it did not get through.
- On the Mesh screen, press and hold a channel or conversation (or hold
  Confirm) for **Open**, **Mark as Read** and **Clear History**, which deletes
  its file from the card. Channels also offer **Share as QR Code** and
  **Leave Channel**. **Mark All as Read** appears above the channels while
  anything is unread.

### Channels

**Join a Channel**, under the channels while the radio has a free slot,
offers:

- **Public**: the channel every MeshCore app knows.
- **Hashtag Channel**: type a name such as `#hiking`. Its secret is the first
  16 bytes of SHA-256 of the name, so anyone who knows the name can read it.
  Names are case sensitive.
- **Private Channel**: its secret as 32 hex digits (spaces, dashes or colons
  between pairs are fine), then a name of up to 31 characters; an empty name
  gives "Private Channel". Leave the secret empty for a new random one, then
  use **Share as QR Code** so others can scan it into the MeshCore app.

**Leave Channel** clears the slot on the radio; the history stays on the card.

### Radio settings

**Radio Settings** on the Mesh screen changes the node name, transmit power,
frequency, bandwidth, spreading factor, coding rate, this radio's position
(`latitude, longitude`, empty to clear) and whether adverts carry it. Each
change goes to the radio at once and shows once the radio reports itself
again. The four LoRa settings ask first: only nodes with the same settings
can hear the radio. The frequency must stay in the radio's band. A radio
that repeats as a client (firmware 9 and later) refuses frequencies its
region does not allow for repeating; that shows as "did not take the change".

### Contacts, servers and nearby repeaters

- A contact's popup (press and hold) adds **Details**, read from the radio:
  type, last advert, route, position, and the distance and direction from
  this radio when both positions are known; and **Show QR Code**, the link
  the MeshCore apps scan to add it. **My Contact QR Code** on the Mesh screen
  does the same for this node.
- Choosing a repeater or room server opens its screen: **Log In** (an empty
  password logs in as a guest), **Get Status** (battery, up time, noise floor,
  last signal, packets, air time, queue) once logged in, and for a room
  **Open Room**, its message board as a chat with each post's author. Logins
  last until the link drops; passwords are not stored.
- **Nearby Repeaters** (radio firmware 8 and later) asks the repeaters in
  direct range to answer and lists them with the signal both ways: how this
  radio heard them and how they heard it.

### Speed

The Bluetooth link uses a short connection interval while the radio sends
its contacts and channels, then a longer one to save power. The receive queue
holds 32 frames, so a long contact list is not cut short while the device is
busy with the card. Sending a message costs one e-ink refresh: the chat waits
for the radio to take it before repainting.

A cover thumbnail that was cut short (a power loss while it was written) is
detected by its size, deleted and made again.

### Mesh Carousel home

**Settings > Display > UI Theme > Mesh Carousel** replaces the home screen
with three parts:

- **The book carousel** at the top: the last read books (up to 10, topped up
  from the library), with the selected book large in the middle and its
  neighbours on either side. The title is above it; the author, reading
  progress and one dot per book are below. Swipe left or right, or tap a
  side cover, to move a book to the middle; tap the middle cover to open
  it. The side page buttons walk the books too.
- **The Mesh banner** above the tabs: the radio's name and state, or, when
  messages have arrived, a dark banner with the unread count, the names of
  up to three waiting conversations (newest first) and the newest message.
  Tap it to open Mesh.
- **The tab row** of the Cover Grid theme: Files, Library, Plugins or OPDS
  (when set up), Mesh, File Transfer and Settings. The front Left and Right
  buttons walk them.

Covers are made the first time a book comes into view, at two sizes (middle
and side), under a short **Loading** popup. They are saved next to the
book's other cache files, so later visits draw them straight away.

With a radio paired, Home brings the link up once the first covers are on
screen, so the banner can show new messages. Opening a book, Wi-Fi, USB
Drive or any screen other than Mesh shuts Bluetooth down at once. Messages
that arrive meanwhile wait on the radio (up to 16) and show on the banner
when Home has linked again. Home does not start Bluetooth when no radio was
ever paired, nor after **Disconnect**, until a radio is connected again from
the Mesh screen. Unread counts start again from zero each time the link
comes up.

### Message history

Messages are stored in `/.crosspoint/mesh/threads/` on the SD card, one
append-only file per conversation. Channel files are named `g_` plus the
first 8 hex digits of the SHA-256 of the channel secret, so a channel keeps
its history when it moves to another slot, and the secret is not written to
the card. Direct messages go to `c_` plus the first 12 hex digits of the
sender's public key. Message text is stored as plain text: anyone with the
card can read it.

A chat shows the last 40 messages. Its first row, **Earlier Messages**,
opens the history reader, which shows 40 messages a page and steps back a
page each time its own first row is chosen. Back returns to the chat.

The Mesh screen lists the 24 conversations with the newest messages. Older
ones stay on the card and open from the contact list.

The files are never trimmed. A message takes at most 222 bytes. To clear a
conversation, delete its file.

## How it is put together

| Part | Location | Role |
| --- | --- | --- |
| `MeshFrames` | `lib/MeshCompanion/` | Builds and parses companion protocol frames. |
| `MeshCompanionClient` | `lib/MeshCompanion/` | Command queue, handshake and push handling. |
| `MeshSendTracker` | `lib/MeshCompanion/` | Follows a direct message through acknowledgement, retries and giving up. |
| `MeshContactBook` | `lib/MeshCompanion/` | The radio's contacts. |
| `MeshThreadLog` | `lib/MeshCompanion/` | The history file format. |
| `MeshLink` | `lib/MeshLink/` | NimBLE central: scan, pair, and move frames over the Nordic UART service. |
| `MeshService` | `src/mesh/` | Owns the link and the client, tracks channels, conversations and unread counts, sends and stores messages; polled from `loop()` in `main.cpp`. |
| `MeshHistory` | `src/mesh/` | Reads and writes the history files on the SD card. |
| `MeshRadioStore` | `src/mesh/` | The saved radio, in `/.crosspoint/mesh.json`. |
| `MeshActivity` | `src/activities/mesh/` | The Mesh screen: pairing, channels, conversations, radio details. |
| `MeshChatActivity` | `src/activities/mesh/` | One chat, with a channel or a contact. |
| `MeshHistoryActivity` | `src/activities/mesh/` | Pages back through one chat's history. |
| `MeshContactsActivity` | `src/activities/mesh/` | The contact list. |
| `MeshMessageRow` | `src/activities/mesh/` | Text and layout shared by those screens. |

Everything in `lib/MeshCompanion/` is free of hardware dependencies and is
covered by the host tests. The chat, history and contact screens are pushed
on top of `MeshActivity`, which stays on the activity stack and keeps the
link up.

The BLE bond is kept by NimBLE in NVS. **Forget Saved Radio** removes both
the saved radio and its bond.

Design notes that are easy to miss:

- **One command at a time.** The protocol has no sequence numbers, so the
  client matches a reply to the in-flight command by its code. A reply that
  arrives after its command timed out can be taken for the next command's
  reply when both expect the same code.
- **NimBLE configuration comes from the prebuilt Arduino core.** Its
  `sdkconfig.h` enables every role with 3 connections and 3 bonds. `-D`
  overrides of `CONFIG_BT_NIMBLE_*` are redefined by that header and ignored.
- **The CPU drops to 80 MHz when idle** on PSRAM boards, also while linked.
  `MeshService` forces normal frequency only around BLE start and stop.
  Whether the link is stable across those frequency changes is untested.
- **The radio's clock** is set from this device only when `trustedtime`
  reports a trusted time. The radio refuses a time earlier than its own.
- **Message times come from the radio's clock.** The handshake reads it, and
  outgoing messages are stamped from that reading plus the time passed since.
  If the radio's clock was not read, this device's clock is used when it is
  trusted.
- **A history record has its length at both ends.** The chat reads the
  newest messages from the end of the file without scanning it. A record cut
  short by a power loss fails that check; the next append then scans the
  file from the start and cuts it after the last record that passes. A file
  that cannot be read, or that this version did not write, is not changed.
  A file shorter than its 4-byte header is started again.
- **An outgoing message is written to the history when the radio takes or
  refuses it,** not when it is typed, so the file holds one record per
  message. A direct message is written as **Sent**; when its state changes
  later, one byte of that record is rewritten in place. Nothing else in a
  history file is ever rewritten.
- **A direct message that is waiting for its acknowledgement is settled as
  Not confirmed** when the link drops or the Mesh screen is left. An
  acknowledgement that arrives after that is ignored.
- **Four direct messages can wait for acknowledgements at once.** A fifth
  is sent once and stays **Sent**.
- **The radio passes on every copy of a direct message.** A sender that
  missed the acknowledgement sends the same message again. A copy with the
  same time and text as one of the last four received from that contact is
  dropped.
- **Unread counts live in RAM.** They cover the time from opening the Mesh
  screen to leaving it, across reconnects.
- **A contact's route** is the radio's stored path length: 255 for none, and
  otherwise the hop count in the low 6 bits.
- **A favourite is changed by reading the contact and writing it back.**
  The protocol has no command for the flags alone: writing a contact
  replaces every field the radio stores, and this device keeps only some of
  them. So it reads the whole record, changes one bit and sends the rest
  back as read. An answer for any other contact is ignored. A route the
  radio learns between the read and the write is lost until the contact's
  next advert. Radio firmware without the read command refuses the change,
  and nothing is written.
- **A screen that re-orders its rows ignores taps and Confirm until the
  panel shows the new order.** A refresh takes about a second, and during it
  the panel still shows the old rows; a tap would otherwise open a different
  conversation or contact than the one under the finger.
- **The Mesh screen repaints only when its rows changed.** The radio reports
  many updates, such as adverts, that change nothing on screen.
- **The channel list is complete or the handshake fails.** Messages are
  matched to channels by slot, so a channel read that gets no answer is
  tried three times and then ends the link with **The radio did not
  answer**.
- **A message for a slot the radio did not list** is kept under a channel
  named `#` plus the slot number, with its own history file (`s_` plus the
  slot in hex). The radio does not list a channel that has no name.
- **The render task draws the chat while the main loop receives.** The
  message list changes only under the render lock, and an incoming message
  never repaints directly: repaints are requested from `loop()`, at most one
  every 1.5 s for incoming messages, so a keyboard on top is not redrawn.
- **A radio that was reset or re-flashed** no longer holds the bond. The
  first reconnect then fails with **Pairing failed**, the stale bond is
  deleted, and the next **Connect** pairs again with the PIN.

## Host tests

```sh
pio run -t unit-tests
```

`test/mesh_companion/` holds 113 tests for the frame codec, the client, the
delivery tracker, the contact table and the history file format.
`RadioFrames.h` builds reply frames field by field from the layouts in the
MeshCore firmware source (`examples/companion_radio/MyMesh.cpp`), so the
parsers are checked against the real format and not against the builders.

The tests cannot show that a real radio behaves as the source reads. Capture
real frames during the hardware tests and add them as fixtures.

## Hardware test plan

Run with a serial monitor attached (`python3 scripts/debugging_monitor.py`).
The link logs under the `MLINK` tag, the service under `MESH`, the history
files under `MHIST`, and the chat, history and contact screens under
`MCHAT`, `MHISTV` and `MCONT`.

| # | Step | Expected | Watch for |
| --- | --- | --- | --- |
| 1 | Flash `x4pro-mesh`, boot, read a book for a minute | Reader works as before | Crash at boot; lower free heap in the `MEM` log line than on `x4pro` |
| 2 | Open **Mesh** | Status row shows **Not connected to a radio** | **Bluetooth could not start**; `NimBLE init failed` in the log |
| 3 | **Scan for Radios** with the radio on | The radio appears within 8 s with a name and signal strength | Empty list; a row showing an address instead of a name |
| 4 | Tap the radio | Keyboard opens with `123456` | No keyboard; **Pairing failed** before the keyboard appears |
| 5 | Confirm the right PIN | **Reading radio**, then the detail rows | `Pairing with ... failed` and its error number; stuck on **Connecting** |
| 6 | Check each detail row against the phone app | Name, frequency, bandwidth, SF, CR and TX power match | Any mismatch means a frame offset is wrong |
| 7 | Send a channel message from another node while the Mesh screen is open | The channel's row shows the text and a count of 1 | Row unchanged; garbled text |
| 8 | **Send Advert**, then check the other node | Status shows **Advert sent**; the other node hears it | No advert heard |
| 9 | Leave Mesh, wait 10 s, open it again | Reconnects with no PIN prompt | PIN prompt on every connect means the bond is not stored |
| 10 | Power the radio off while connected | **Link lost. Reconnecting...**, then **Radio not reachable**. Retries stop after about 2 minutes | Retries that never stop; the device not sleeping afterwards |
| 11 | Power the radio on, tap **Connect** | Links again without a PIN | |
| 12 | Enter a wrong PIN on a fresh pairing | **Pairing failed. Check the PIN** | Crash or hang |
| 13 | Let the device auto-sleep on the Mesh screen, then wake it | Sleeps and wakes normally | `BLE still up at sleep` in the log; failure to wake |
| 14 | Open Mesh, go Home, open **File Transfer** and join Wi-Fi | Wi-Fi works | Wi-Fi failures after Bluetooth has been used |
| 15 | Repeat steps 2 to 9 in the cover-grid theme and in landscape | Mesh entry visible and usable in both home layouts | Menu items overlapping; wrong item opening |
| 16 | Try an nRF52 radio if you have one | Same as steps 3 to 9 | Different pairing behaviour from ESP32 radios |

Channel chat, added after the first hardware run:

| # | Step | Expected | Watch for |
| --- | --- | --- | --- |
| 17 | Open **Mesh** and wait for the details | A **Channels** section lists the radio's channels above a **Radio** section | No channels; a channel named `#` and a number; rows overlapping the section headings |
| 18 | Open a channel that has no history | **No earlier messages** and **Write a Message** are shown, the second selected | Crash; blank screen |
| 19 | Send a message to that channel from another node | It appears within a few seconds with the sender's name and the time | Nothing appears; the sender name stays inside the text; wrong time or wrong day |
| 20 | Tap **Write a Message**, type, confirm | The message appears as **You** with **Sending...**, which then goes away. The other node receives it | Stuck on **Sending...** for more than 5 s; **Not sent**; two messages; the other node receives nothing |
| 21 | Type a message of the full allowed length | The keyboard stops taking characters at the limit; the other node receives the whole text | Text cut short on the other node |
| 22 | Send a long message from another node | The row grows to fit the text | Text cut with an ellipsis; rows overlapping |
| 23 | Receive more messages than fit on the screen | The view follows the newest message. After scrolling back to older messages it stays where it is | View jumping while reading older messages |
| 24 | Go back to the Mesh screen | The channel row shows the newest message and no unread count | Stale preview |
| 25 | Stay on the Mesh screen while a message arrives | The channel row shows a count of 1 and the new text. Opening the channel clears the count | Count missing or never cleared |
| 26 | Leave Mesh, open it again and open the channel | Earlier messages are there, in order | Empty chat; messages missing or repeated |
| 27 | Reboot, then repeat step 26 | Same as step 26 | History lost |
| 28 | Put the card in a computer | `/.crosspoint/mesh/threads/` holds one `g_*.log` file per used channel | Files missing; files of 4 bytes after messages were received |
| 29 | Switch the radio off while a chat is open, then on again within a minute | The last row changes to **Not connected to a radio**, and back once the link is up. Retries stop after about 2 minutes; then go back and tap **Connect** | Crash; a last row that still opens the keyboard while offline |
| 30 | Open the keyboard, switch the radio off, wait 10 s, then confirm a message | The message appears as **Not sent** | Stuck on **Sending...** |
| 31 | Switch the radio on, wait for the last row to change back, tap the **Not sent** message | The keyboard opens with that text; confirming sends it as a new message | |
| 32 | Have several messages sent while Mesh is closed, then open Mesh | The channel shows the count of waiting messages; the chat shows them in order | More than a few screen flashes while they arrive; messages lost |
| 33 | Send a direct message to the radio from another node while the Mesh screen is open | The sender appears under **Direct Messages** with the text and a count of 1; a `c_*.log` file appears on the card | No row; a row named with hex digits instead of the contact's name |
| 34 | Use the chat with only the buttons | Up and Down move through the messages, Confirm on the last row opens the keyboard | Selection that cannot reach the last row |
| 35 | Let the device auto-sleep with a chat open, then wake it | Wakes to the home screen; Mesh and the chat open again with the history | Failure to wake; `BLE still up at sleep` in the log |

Direct messages, contacts and history:

| # | Step | Expected | Watch for |
| --- | --- | --- | --- |
| 36 | Open **Mesh** and wait for the details | A **Direct Messages** section sits between **Channels** and **Radio**, ending in a **Contacts** row with the number of contacts | Section missing; a count of 0 on a radio that has contacts |
| 37 | Tap **Contacts** | People are listed first, by name, each with **In direct range**, **Hops:** and a number, or **No route yet**. Rooms and repeaters follow | Empty list; names out of order; the count differing from the phone app's |
| 38 | Tap a repeater | Nothing happens | A chat opening for it |
| 39 | Tap a person | A chat opens with their name as its title | Another person's name in the title |
| 40 | Send them a message while their radio is on and in range | **Sending...**, then **Sent**, then **Delivered**. They receive it once | Stuck on **Sent**; **Not confirmed** although it arrived; the message arriving twice |
| 41 | Send a message to a contact whose radio is off | **Sent**, then after up to a few minutes **Not confirmed** | Stuck on **Sent**; **Delivered** |
| 42 | Tap the **Not confirmed** message | The keyboard opens with its text | |
| 43 | Have the contact answer | The answer appears with their name and the time | Their name missing; the answer appearing twice |
| 44 | Go back to the Mesh screen | The conversation is the first row under **Direct Messages**, with the newest text and no count | Row missing; stale text |
| 45 | Stay on the Mesh screen while they send again | The row shows a count of 1. Tapping it opens that conversation | A different conversation opening |
| 46 | Send a direct message, press Back before **Delivered**, open the conversation again | The message is there once, and still changes to **Delivered** | The message shown twice; stuck on **Sent** after the other side received it |
| 47 | Send a direct message and leave Mesh before **Delivered**, then come back | The message shows **Not confirmed**, or **Not sent** if you left within a moment of sending | **Sending...**; the message missing |
| 48 | In a chat with more than 40 stored messages, go to the first row, **Earlier Messages** | The history reader opens with the messages before those in the chat, the newest of them selected | The same messages as in the chat; an empty page |
| 49 | Choose **Earlier Messages** in the reader until it reads **No earlier messages** | Each step shows an older page, down to the first message of the conversation | A page skipped or repeated; messages out of order |
| 50 | Press Back in the reader | The chat is back at its newest messages | |
| 51 | Reboot and open Mesh | Once the radio's details show, the conversations are listed with the contacts' names, newest first | Conversations missing; hex digits in place of the name of a contact the radio still has |
| 52 | Tap a row on the Mesh screen in the second after a new message re-ordered the list | Nothing opens until the screen has finished refreshing | The wrong conversation opening |

Favourites:

| # | Step | Expected | Watch for |
| --- | --- | --- | --- |
| 53 | Mark a contact as a favourite in the MeshCore phone app, then link the X4 Pro and open **Contacts** | The contact is under **Favorites** at the top; the rest follow under **Other Contacts** | No **Favorites** heading; the contact among the others |
| 54 | Press and hold another contact's row | A popup with the contact's name offers **Open Chat** and **Add to Favorites** | A chat opening; no popup |
| 55 | Choose **Add to Favorites** | Within about two seconds the contact moves under **Favorites**, still selected | **The radio did not save the change**; the wrong contact moving |
| 56 | Link the phone app again and look at that contact | It is a favourite there, and its name, type and route are unchanged | Name, route or position changed; the contact twice in the list |
| 57 | Hold Confirm on a favourite and choose **Remove from Favorites** | It moves back under **Other Contacts** | |
| 58 | Press and hold a repeater | The popup offers only **Add to Favorites** | **Open Chat** offered |
| 59 | Open the popup, switch the radio off, wait 10 s, choose **Add to Favorites** | **The radio did not save the change** | The contact moving; no message |

Mesh Carousel home (Settings > Display > UI Theme > **Mesh Carousel**):

| # | Step | Expected | Watch for |
| --- | --- | --- | --- |
| 60 | Choose the theme with several books on the card, then go Home | A large middle cover between two smaller ones, the title above, author and progress below, a dot per book; the banner above the six tabs | Covers overlapping the banner or tabs; text cut off at the screen edge |
| 61 | Swipe left, then right | The next book comes to the middle, then the previous one; the first time a book shows, **Loading** appears briefly | The same book staying; the placeholder art flashing before the cover |
| 62 | Tap a side cover, then the middle one | The side book moves to the middle; the second tap opens it | A side tap opening the book |
| 63 | Press the side page buttons, then the front Left/Right | The page buttons walk the books with a heavy ring on the middle cover; Left/Right walk the tabs | |
| 64 | With only one book, then two | One cover and no dots; with two, swiping goes between them and stops at the ends | A crash; the same book on both sides |
| 65 | With a radio paired and switched on, go Home | Within a few seconds the banner goes from **Connecting to the radio...** to **Connected. No new messages** | The banner staying on connecting; covers missing after the link comes up |
| 66 | Send yourself a direct message and a channel message from another node | The banner turns dark with the sender (or channel), the message and the count 2 | No change; the count wrong |
| 67 | Tap the banner, open the conversation, go back Home | The banner is light again; the link stays up the whole time (no **Connecting**) | A reconnect; the count still showing |
| 68 | Open a book, read, go back Home | Bluetooth goes off while reading (`MEM` free heap rises); back Home the banner reconnects and shows messages sent meanwhile | The link staying up while reading |
| 69 | Choose **Disconnect** in Mesh, go Home | The banner says **Not connected to a radio** and does not reconnect | A reconnect |
| 70 | With no radio paired (Forget Saved Radio) | The banner says **No radio yet. Tap to set one up**; tapping opens Mesh | Bluetooth starting on Home |
| 71 | Rotate to landscape | Everything fits, with smaller covers | Overlaps; a crash |

Chat tools, channels, settings, servers:

| # | Step | Expected | Watch for |
| --- | --- | --- | --- |
| 72 | Open a chat and choose **Quick Replies** | Eight default replies; choosing one sends it, with one refresh showing it as sent | Two refreshes per send; nothing sent |
| 73 | Edit `/.crosspoint/mesh/quick_replies.txt` on a computer, open the chat again | Your replies, in file order, blank lines skipped | Garbled text; old replies |
| 74 | Press and hold an incoming message | Sender, date and time, route and SNR, one per line | All on one line; wrong hop count |
| 75 | Press and hold a **Not sent** message and choose **Send Again** | The keyboard opens with its text | |
| 76 | Press and hold a channel on the Mesh screen, choose **Mark as Read** | Its count goes away | Another channel's count cleared |
| 77 | **Clear History** on a conversation, then open it | Asks first; afterwards the chat is empty and the conversation leaves the list | The file still there |
| 78 | **Join a Channel > Hashtag Channel**, type `#test` | "Channel added"; the channel appears; messages from a phone app on `#test` arrive | A second copy of a channel you had |
| 79 | **Private Channel** with an empty secret, then **Share as QR Code** | A QR code the MeshCore app scans to join the same channel | The app joining a different channel |
| 80 | **Leave Channel** on it | Asks first; "Channel removed"; gone after a reconnect too | Another channel removed |
| 81 | **Radio Settings**: change the name | "Saved on the radio"; the new name in the list and in the phone app | |
| 82 | Change the transmit power to an out-of-range value | A message giving the allowed range | The radio taking it |
| 83 | Change the spreading factor, cancel at the warning; then confirm | Cancel changes nothing; confirming changes it (change it back after) | No warning |
| 84 | Set a position, turn **Share Position in Adverts** on | Both shown; the phone app sees the position after an advert | |
| 85 | **My Contact QR Code**, scan with the phone app | The app offers to add this node | Wrong name or key |
| 86 | **Details** on a contact that shares its position | Type, last advert, route, position, distance and direction | Nonsense distance |
| 87 | Open a repeater, **Get Status** before logging in | "Log in first" | A 40 s wait |
| 88 | **Log In** with an empty password, then **Get Status** | Guest; status rows with battery, up time and the rest | "Waiting" forever |
| 89 | Open a room server, log in, **Open Room** | Its posts with each author's name; a post you send appears for others | Hex names for known contacts |
| 90 | **Nearby Repeaters** | "Searching...", then the repeaters in range with two SNR values | Your own node listed |
| 91 | Choose a nearby repeater that is a contact, then **Log In** | Works at once | "Waiting" |
| 92 | Turn the radio off during a login | "Waiting" ends with a failure within about a minute; after reconnecting, logins are gone | Stuck on "Waiting" |
| 93 | With messages in three conversations, go Home (Mesh Carousel) | The dark banner lists the three names and the newest message | The banner or covers changing size |
| 94 | Pull the card while a thumbnail is being made (or cut a thumb file short on a computer), then go Home | That cover is made again | A black box for that book |

Record the `MEM` log line before opening Mesh, while linked, and after
leaving. The difference is the heap cost of the BLE stack, and the last value
shows whether teardown returns it.
