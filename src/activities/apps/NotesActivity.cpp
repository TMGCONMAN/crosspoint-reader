#include "NotesActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <utility>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

using notes::NoteDocument;

namespace {

constexpr char DIR_PATH[] = "/notes";
constexpr char TEMP_PATH[] = "/notes/.saving.tmp";
constexpr int MAX_NOTES = 200;
constexpr int FIXED_ROWS = 2;  // "New note" and "New checklist" head the list
constexpr int BODY_FONT = NOTOSANS_14_FONT_ID;
constexpr int BOX = 20;           // checkbox size
constexpr int BOX_GAP = 10;       // checkbox to text
constexpr int PARAGRAPH_GAP = 8;  // extra space between paragraphs
constexpr int FULL_REFRESH_EVERY = 20;
constexpr unsigned long MENU_HOLD_MS = 700;

bool isNoteFile(const String& name) {
  const int n = name.length();
  if (n < 5 || name[0] == '.') return false;
  const String ext = name.substring(n - 4);
  return ext.equalsIgnoreCase(".txt");
}

std::string pathOf(const std::string& file) { return std::string(DIR_PATH) + "/" + file; }

size_t utf8Next(const std::string& s, size_t i) {
  i++;
  while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
  return i;
}

}  // namespace

NotesActivity::NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Notes", renderer, mappedInput) {}

void NotesActivity::onEnter() {
  Activity::onEnter();
  loadList();
  fullRefresh = true;
  requestUpdate();
}

// ---- Files ----

void NotesActivity::loadList() {
  std::vector<Entry> found;
  for (const String& name : Storage.listFiles(DIR_PATH, MAX_NOTES)) {
    if (!isNoteFile(name)) continue;
    Entry entry;
    entry.file = name.c_str();
    char start[257];
    const size_t got = Storage.readFileToBuffer(pathOf(entry.file).c_str(), start, sizeof(start), 256);
    entry.title = notes::titleFromStart(start, got);
    if (entry.title.empty()) entry.title = tr(STR_NOTES_UNTITLED);
    found.push_back(std::move(entry));
  }
  // note-012 before note-011: the newest numbered notes first.
  std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.file > b.file; });
  RenderLock lock(*this);
  entries = std::move(found);
  listSelected = std::min(listSelected, static_cast<int>(entries.size()) + FIXED_ROWS - 1);
}

void NotesActivity::openNote(const std::string& file) {
  const std::string path = pathOf(file);
  HalFile f;
  if (!Storage.openFileForRead("NOTES", path.c_str(), f)) {
    showMessage(tr(STR_NOTES_OPEN_FAILED));
    return;
  }
  const size_t size = f.fileSize();
  if (size > NoteDocument::MAX_BYTES) {
    f.close();
    showMessage(tr(STR_NOTES_TOO_BIG));
    return;
  }
  std::string text(size, '\0');
  const int got = size ? f.read(text.data(), size) : 0;
  f.close();
  if (got != static_cast<int>(size)) {
    showMessage(tr(STR_NOTES_OPEN_FAILED));
    return;
  }
  RenderLock lock(*this);
  doc.parse(text.data(), text.size());
  currentFile = file;
  mode = Mode::View;
  rowsDirty = true;
  topRow = 0;
  selectedParagraph = -1;
  fullRefresh = true;
}

bool NotesActivity::saveNote() {
  if (currentFile.empty() || !Storage.ensureDirectoryExists(DIR_PATH)) return false;
  const std::string text = doc.serialize();
  HalFile f;
  if (!Storage.openFileForWrite("NOTES", TEMP_PATH, f)) return false;
  const size_t wrote = text.empty() ? 0 : f.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  f.close();
  // Written in full first, so a failed save never cuts the note short.
  if (wrote != text.size() || !Storage.replaceFile(TEMP_PATH, pathOf(currentFile).c_str())) {
    LOG_ERR("NOTES", "Saving %s failed", currentFile.c_str());
    showMessage(tr(STR_NOTES_SAVE_FAILED));
    return false;
  }
  return true;
}

void NotesActivity::closeNote() {
  loadList();
  RenderLock lock(*this);
  mode = Mode::List;
  // Keep the closed note selected.
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].file == currentFile) listSelected = static_cast<int>(i) + FIXED_ROWS;
  }
  currentFile.clear();
  doc = NoteDocument();
  rows.clear();
  fullRefresh = true;
}

// ---- Popups and the keyboard ----

void NotesActivity::showMessage(const char* text) {
  snprintf(messageText, sizeof(messageText), "%s", text);
  const char* options[1] = {tr(STR_OK_BUTTON)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage("", messageText, options, 1, 0, [](int) {});
  }
  requestUpdate();
}

void NotesActivity::openKeyboard(const char* title, const std::string& initial, const Pending purpose,
                                 const int index) {
  const size_t limit = purpose == Pending::EditAll ? NoteDocument::MAX_EDIT_ALL : NoteDocument::MAX_ENTRY;
  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, title, initial, limit, InputType::Text);
  if (!keyboard) {
    LOG_ERR("NOTES", "OOM: keyboard");
    return;
  }
  keyboard->setMultiline(true);
  startActivityForResult(std::move(keyboard), [this, purpose, index](const ActivityResult& result) {
    fullRefresh = true;
    if (result.isCancelled) return;
    const std::string& text = std::get<KeyboardResult>(result.data).text;
    if (purpose == Pending::NewNote || purpose == Pending::NewChecklist) {
      NoteDocument fresh;
      fresh.insertText(0, text);
      // Nothing typed (or only an empty checkbox): no note.
      if (fresh.title().empty()) return;
      std::vector<std::string> names;
      for (const auto& e : entries) names.push_back(e.file);
      {
        RenderLock lock(*this);
        doc = std::move(fresh);
        currentFile = notes::nextNoteName(names);
        mode = Mode::View;
        rowsDirty = true;
        topRow = 0;
        selectedParagraph = -1;
      }
      saveNote();
      return;
    }
    int added = -1;
    int first = -1;
    {
      RenderLock lock(*this);
      if (purpose == Pending::EditAll) {
        doc.parse(text.data(), text.size());
        added = static_cast<int>(doc.count());
        selectedParagraph = -1;
      } else if (purpose == Pending::Edit) {
        added = doc.replaceWithText(static_cast<size_t>(index), text);
        first = added > 0 ? index : -1;
      } else if (!text.empty()) {
        const size_t at = purpose == Pending::AddEnd ? doc.count() : static_cast<size_t>(index) + 1;
        added = doc.insertText(at, text);
        first = added > 0 ? static_cast<int>(at) : -1;
      } else {
        return;
      }
      if (added >= 0) selectedParagraph = first;
      rowsDirty = true;
    }
    if (added < 0) {
      showMessage(tr(STR_NOTES_TOO_BIG));
      return;
    }
    saveNote();
    if (first >= 0) ensureVisible(first);
  });
}

void NotesActivity::paragraphMenu(const int index) {
  if (index < 0 || index >= static_cast<int>(doc.count())) return;
  enum Action { EDIT, ADD_BELOW, TICK, CHECKBOX, DELETE_LINE, CANCEL };
  const auto task = NoteDocument::taskOf(doc.at(index));
  const char* options[6];
  int actions[6];
  int n = 0;
  options[n] = tr(STR_NOTES_EDIT);
  actions[n++] = EDIT;
  options[n] = tr(STR_NOTES_ADD_BELOW);
  actions[n++] = ADD_BELOW;
  if (task != NoteDocument::Task::None) {
    options[n] = task == NoteDocument::Task::Done ? tr(STR_NOTES_UNTICK) : tr(STR_NOTES_TICK);
    actions[n++] = TICK;
  }
  options[n] = task == NoteDocument::Task::None ? tr(STR_NOTES_ADD_CHECKBOX) : tr(STR_NOTES_REMOVE_CHECKBOX);
  actions[n++] = CHECKBOX;
  options[n] = tr(STR_NOTES_DELETE_LINE);
  actions[n++] = DELETE_LINE;
  options[n] = tr(STR_CANCEL);
  actions[n++] = CANCEL;
  {
    RenderLock lock(*this);
    selectedParagraph = index;
    optionPopup.show(tr(STR_NOTES_LINE), options, n, 0, [this, index, actions, n](const int choice) {
      if (choice < 0 || choice >= n) return;
      bool changed = false;
      switch (actions[choice]) {
        case EDIT:
          pending = Pending::Edit;
          pendingIndex = index;
          return;
        case ADD_BELOW:
          pending = Pending::AddBelow;
          pendingIndex = index;
          return;
        case TICK: {
          RenderLock lock(*this);
          changed = doc.toggleDone(static_cast<size_t>(index));
          break;
        }
        case CHECKBOX: {
          RenderLock lock(*this);
          changed =
              doc.setTask(static_cast<size_t>(index), NoteDocument::taskOf(doc.at(index)) == NoteDocument::Task::None);
          rowsDirty = true;
          break;
        }
        case DELETE_LINE: {
          RenderLock lock(*this);
          changed = doc.remove(static_cast<size_t>(index));
          selectedParagraph = -1;
          rowsDirty = true;
          break;
        }
        default:
          return;
      }
      if (changed) saveNote();
    });
  }
  requestUpdate();
}

void NotesActivity::noteMenu() {
  const char* options[4] = {tr(STR_NOTES_ADD_END), tr(STR_NOTES_EDIT_ALL), tr(STR_NOTES_DELETE_NOTE), tr(STR_CANCEL)};
  {
    RenderLock lock(*this);
    optionPopup.show(tr(STR_NOTES_NOTE), options, 4, 0, [this](const int choice) {
      if (choice == 0) pending = Pending::AddEnd;
      if (choice == 1) pending = Pending::EditAll;
      if (choice == 2) pending = Pending::ConfirmDelete;
    });
  }
  requestUpdate();
}

void NotesActivity::runPending() {
  const Pending what = pending;
  const int index = pendingIndex;
  pending = Pending::None;
  switch (what) {
    case Pending::NewNote:
      openKeyboard(tr(STR_NOTES_NEW), "", Pending::NewNote, -1);
      break;
    case Pending::NewChecklist: {
      // A title line, then the first item; Enter adds the next one.
      const std::string start = std::string(tr(STR_NOTES_CHECKLIST_TITLE)) + "\n[ ] ";
      openKeyboard(tr(STR_NOTES_NEW_CHECKLIST), start, Pending::NewChecklist, -1);
      break;
    }
    case Pending::EditAll:
      if (doc.bytes() > NoteDocument::MAX_EDIT_ALL) {
        showMessage(tr(STR_NOTES_TOO_LONG_TO_EDIT_ALL));
      } else {
        std::string all = doc.serialize();
        if (!all.empty() && all.back() == '\n') all.pop_back();
        openKeyboard(tr(STR_NOTES_EDIT_ALL), all, Pending::EditAll, -1);
      }
      break;
    case Pending::Edit:
      if (index >= 0 && index < static_cast<int>(doc.count())) {
        openKeyboard(tr(STR_NOTES_EDIT), doc.at(index), Pending::Edit, index);
      }
      break;
    case Pending::AddBelow: {
      const bool task = index >= 0 && index < static_cast<int>(doc.count()) &&
                        NoteDocument::taskOf(doc.at(index)) != NoteDocument::Task::None;
      openKeyboard(tr(STR_NOTES_ADD_BELOW), task ? "[ ] " : "", Pending::AddBelow, index);
      break;
    }
    case Pending::AddEnd: {
      const bool task = doc.count() > 0 && NoteDocument::taskOf(doc.at(doc.count() - 1)) != NoteDocument::Task::None;
      openKeyboard(tr(STR_NOTES_ADD_END), task ? "[ ] " : "", Pending::AddEnd, -1);
      break;
    }
    case Pending::ConfirmDelete: {
      snprintf(messageText, sizeof(messageText), tr(STR_NOTES_DELETE_CONFIRM_FORMAT), doc.title(40).c_str());
      const char* options[2] = {tr(STR_NOTES_DELETE_NOTE), tr(STR_CANCEL)};
      {
        RenderLock lock(*this);
        optionPopup.showMessage("", messageText, options, 2, 1, [this](const int choice) {
          if (choice == 0) pending = Pending::DeleteNote;
        });
      }
      requestUpdate();
      break;
    }
    case Pending::DeleteNote:
      if (!Storage.remove(pathOf(currentFile).c_str())) LOG_ERR("NOTES", "Delete %s failed", currentFile.c_str());
      currentFile.clear();
      closeNote();
      requestUpdate();
      break;
    case Pending::None:
    default:
      break;
  }
}

// ---- Layout ----

int NotesActivity::lineHeight() const { return renderer.getLineHeight(BODY_FONT); }
int NotesActivity::listRowHeight() const {
  return renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 18;
}

NotesActivity::Frame NotesActivity::frame() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  const int side = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int toolH = renderer.getLineHeight(UI_12_FONT_ID) + 14;
  Frame f;
  f.body = Rect4{content.x + side, top, content.width - 2 * side, bottom - top};
  if (mode == Mode::View) {
    const int toolW = std::max(96, f.body.w / 4);
    f.add = Rect4{f.body.x, bottom - toolH, toolW, toolH};
    f.menu = Rect4{f.body.x + f.body.w - toolW, bottom - toolH, toolW, toolH};
    f.body.h -= toolH + 8;
  }
  return f;
}

int NotesActivity::listRowsPerPage() const { return std::max(1, frame().body.h / listRowHeight()); }

void NotesActivity::layoutRows() {
  if (!rowsDirty) return;
  rowsDirty = false;
  rows.clear();
  const int width = frame().body.w - 6;  // room for the selection bar
  const int space = renderer.getTextWidth(BODY_FONT, " ");
  for (size_t p = 0; p < doc.count(); p++) {
    const std::string& raw = doc.at(p);
    const bool task = NoteDocument::taskOf(raw) != NoteDocument::Task::None;
    const std::string body = NoteDocument::bodyOf(raw);
    const int avail = std::max(40, width - (task ? BOX + BOX_GAP : 0));
    const size_t n = body.size();
    if (n == 0) {
      rows.push_back(Row{static_cast<uint16_t>(p), 0, 0, true});
      continue;
    }
    size_t pos = 0;
    bool first = true;
    while (pos < n) {
      // Whole words while they fit.
      size_t end = pos;
      int used = 0;
      size_t i = pos;
      while (i < n) {
        size_t wordEnd = body.find(' ', i);
        if (wordEnd == std::string::npos) wordEnd = n;
        const std::string word = body.substr(i, wordEnd - i);
        const int w = renderer.getTextWidth(BODY_FONT, word.c_str()) + (end > pos ? space : 0);
        if (used + w > avail) break;
        used += w;
        end = wordEnd;
        i = wordEnd + 1;
      }
      if (end == pos) {
        // A word wider than the line: as many characters as fit.
        end = utf8Next(body, pos);
        while (end < n) {
          const size_t next = utf8Next(body, end);
          if (renderer.getTextWidth(BODY_FONT, body.substr(pos, next - pos).c_str()) > avail) break;
          end = next;
        }
      }
      rows.push_back(Row{static_cast<uint16_t>(p), static_cast<uint16_t>(pos), static_cast<uint16_t>(end), first});
      first = false;
      pos = end;
      while (pos < n && body[pos] == ' ') pos++;
    }
  }
  topRow = std::min(topRow, std::max(0, static_cast<int>(rows.size()) - 1));
}

void NotesActivity::ensureVisible(const int paragraph) {
  RenderLock lock(*this);
  layoutRows();
  int firstRow = -1;
  int lastRow = -1;
  for (size_t r = 0; r < rows.size(); r++) {
    if (rows[r].paragraph != paragraph) continue;
    if (firstRow < 0) firstRow = static_cast<int>(r);
    lastRow = static_cast<int>(r);
  }
  if (firstRow < 0) return;
  if (firstRow < topRow) topRow = firstRow;
  if (lastRow >= topRow + rowsPerPage) topRow = std::max(firstRow, lastRow - rowsPerPage + 1);
}

void NotesActivity::pageBy(const int direction) {
  bool moved = false;
  {
    RenderLock lock(*this);
    layoutRows();
    const int count = static_cast<int>(rows.size());
    const int step = std::max(1, rowsPerPage - 1);  // keep a line of context
    const int next = std::max(0, std::min(topRow + direction * step, std::max(0, count - 1)));
    moved = next != topRow;
    topRow = next;
  }
  if (moved) requestUpdate();
}

// ---- Input ----

void NotesActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (pending != Pending::None) {
    runPending();
    return;
  }
  if (mode == Mode::List) {
    listLoop();
  } else {
    viewLoop();
  }
}

void NotesActivity::listLoop() {
  const int count = static_cast<int>(entries.size()) + FIXED_ROWS;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    showCursor = false;
    const Rect4 body = frame().body;
    if (!body.contains(tx, ty)) return;
    const int row = listTop + (ty - body.y) / listRowHeight();
    if (row >= count) return;
    haptic_feedback::touchAction();
    listSelected = row;
    if (row == 0) {
      pending = Pending::NewNote;
    } else if (row == 1) {
      pending = Pending::NewChecklist;
    } else {
      openNote(entries[row - FIXED_ROWS].file);
    }
    requestUpdate();
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  const int perPage = listRowsPerPage();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int dir = swipe == MappedInputManager::SwipeDir::Up ? 1 : -1;
    const int next = std::max(0, std::min(listTop + dir * perPage, count - 1));
    if (next != listTop) {
      listTop = next;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!showCursor) {
      showCursor = true;
      requestUpdate();
      return;
    }
    if (listSelected == 0) {
      pending = Pending::NewNote;
    } else if (listSelected == 1) {
      pending = Pending::NewChecklist;
    } else {
      openNote(entries[listSelected - FIXED_ROWS].file);
    }
    requestUpdate();
    return;
  }
  int next = listSelected;
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    next = (listSelected + 1) % count;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
             mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    next = (listSelected + count - 1) % count;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    next = std::min(count - 1, listSelected + perPage);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    next = std::max(0, listSelected - perPage);
  } else {
    return;
  }
  listSelected = next;
  if (listSelected < listTop) listTop = listSelected;
  if (listSelected >= listTop + perPage) listTop = listSelected - perPage + 1;
  showCursor = true;
  requestUpdate();
}

void NotesActivity::viewLoop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (selectedParagraph >= 0) {
      selectedParagraph = -1;
      requestUpdate();
    } else {
      closeNote();
      requestUpdate();
    }
    return;
  }
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Frame f = frame();
    if (f.add.contains(tx, ty)) {
      haptic_feedback::touchAction();
      pending = Pending::AddEnd;
      return;
    }
    if (f.menu.contains(tx, ty)) {
      haptic_feedback::touchAction();
      noteMenu();
      return;
    }
    int paragraph = -1;
    bool first = false;
    {
      RenderLock lock(*this);
      for (int i = 0; i < hitCount; i++) {
        if (ty >= hits[i].y0 && ty < hits[i].y1) {
          paragraph = hits[i].paragraph;
          first = hits[i].first;
        }
      }
    }
    if (paragraph < 0) {
      // Below the last line: add a paragraph at the end.
      bool lastPage = false;
      {
        RenderLock lock(*this);
        lastPage = topRow + rowsPerPage >= static_cast<int>(rows.size());
      }
      if (f.body.contains(tx, ty) && lastPage) pending = Pending::AddEnd;
      return;
    }
    haptic_feedback::touchAction();
    // A tap on a checkbox ticks it.
    if (first && tx < f.body.x + 6 + BOX + BOX_GAP / 2 &&
        NoteDocument::taskOf(doc.at(paragraph)) != NoteDocument::Task::None) {
      {
        RenderLock lock(*this);
        doc.toggleDone(static_cast<size_t>(paragraph));
      }
      saveNote();
      requestUpdate();
      return;
    }
    paragraphMenu(paragraph);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    pageBy(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    pageBy(-1);
    return;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, MENU_HOLD_MS)) {
    noteMenu();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedParagraph >= 0) {
      paragraphMenu(selectedParagraph);
    } else {
      pending = Pending::AddEnd;
    }
    return;
  }
  // The side buttons turn pages; the front Left/Right step through paragraphs.
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    pageBy(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    pageBy(-1);
    return;
  }
  const int count = static_cast<int>(doc.count());
  if (count == 0) return;
  int next = selectedParagraph;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (selectedParagraph < 0) {
      // Start at the first paragraph on this page.
      RenderLock lock(*this);
      next = topRow < static_cast<int>(rows.size()) ? rows[topRow].paragraph : 0;
    } else {
      next = std::min(count - 1, selectedParagraph + 1);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    next = selectedParagraph < 0 ? 0 : std::max(0, selectedParagraph - 1);
  } else {
    return;
  }
  selectedParagraph = next;
  ensureVisible(next);
  requestUpdate();
}

// ---- Drawing ----

void NotesActivity::present() {
  if (fullRefresh || ++fastRefreshes >= FULL_REFRESH_EVERY) {
    fullRefresh = false;
    fastRefreshes = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void NotesActivity::renderList() {
  const Rect4 body = frame().body;
  const int rowH = listRowHeight();
  const int perPage = listRowsPerPage();
  const int count = static_cast<int>(entries.size()) + FIXED_ROWS;
  for (int i = 0; i < perPage && listTop + i < count; i++) {
    const int index = listTop + i;
    const int y = body.y + i * rowH;
    if (showCursor && index == listSelected) renderer.drawRoundedRect(body.x, y + 2, body.w, rowH - 4, 2, 8, true);
    const int textY = y + 8;
    if (index < FIXED_ROWS) {
      const char* label = index == 0 ? tr(STR_NOTES_NEW) : tr(STR_NOTES_NEW_CHECKLIST);
      const char* hint = index == 0 ? tr(STR_NOTES_NEW_HINT) : tr(STR_NOTES_NEW_CHECKLIST_HINT);
      renderer.drawText(UI_12_FONT_ID, body.x + 12, textY, label, true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, body.x + 12, textY + renderer.getLineHeight(UI_12_FONT_ID) + 2, hint);
    } else {
      const Entry& e = entries[index - FIXED_ROWS];
      const std::string title =
          renderer.truncatedText(UI_12_FONT_ID, e.title.c_str(), body.w - 24, EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, body.x + 12, textY, title.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, body.x + 12, textY + renderer.getLineHeight(UI_12_FONT_ID) + 2, e.file.c_str());
    }
    if (index >= FIXED_ROWS - 1) renderer.fillRect(body.x + 12, y + rowH - 1, body.w - 24, 1, true);
  }
  if (entries.empty()) {
    renderer.drawText(UI_10_FONT_ID, body.x + 12, body.y + FIXED_ROWS * rowH + 16, tr(STR_NOTES_EMPTY));
  }
  const int pages = (count + perPage - 1) / perPage;
  if (pages > 1) {
    char pageText[16];
    snprintf(pageText, sizeof(pageText), "%d/%d", listTop / perPage + 1, pages);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, pageText);
    renderer.drawText(SMALL_FONT_ID, body.x + body.w - w, body.y + body.h - renderer.getLineHeight(SMALL_FONT_ID),
                      pageText);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void NotesActivity::renderView() {
  layoutRows();
  const Frame f = frame();
  const int lh = lineHeight();
  const int textX = f.body.x + 6;
  hitCount = 0;
  int y = f.body.y;
  int r = topRow;
  int drawn = 0;
  while (r < static_cast<int>(rows.size())) {
    const Row& row = rows[r];
    const int gap = row.first && drawn > 0 ? PARAGRAPH_GAP : 0;
    if (y + gap + lh > f.body.y + f.body.h) break;
    y += gap;
    const std::string& raw = doc.at(row.paragraph);
    const auto task = NoteDocument::taskOf(raw);
    const int indent = task != NoteDocument::Task::None ? BOX + BOX_GAP : 0;
    if (task != NoteDocument::Task::None && row.first) {
      const int by = y + (lh - BOX) / 2;
      renderer.drawRect(textX, by, BOX, BOX, 2, true);
      if (task == NoteDocument::Task::Done) {
        renderer.drawLine(textX + 4, by + BOX / 2, textX + BOX / 2 - 1, by + BOX - 5, 3, true);
        renderer.drawLine(textX + BOX / 2 - 1, by + BOX - 5, textX + BOX - 4, by + 4, 3, true);
      }
    }
    const std::string text = NoteDocument::bodyOf(raw).substr(row.start, row.end - row.start);
    renderer.drawText(BODY_FONT, textX + indent, y, text.c_str());
    if (task == NoteDocument::Task::Done && !text.empty()) {
      const int w = renderer.getTextWidth(BODY_FONT, text.c_str());
      renderer.fillRect(textX + indent, y + lh / 2, w, 2, true);
    }
    if (row.paragraph == selectedParagraph) renderer.fillRect(f.body.x, y, 3, lh, true);
    if (hitCount < MAX_HITS) hits[hitCount++] = Hit{y - gap, y + lh, row.paragraph, row.first};
    y += lh;
    r++;
    drawn++;
  }
  rowsPerPage = std::max(1, drawn);
  if (doc.count() == 0) renderer.drawText(UI_10_FONT_ID, textX, f.body.y, tr(STR_NOTES_EMPTY_NOTE));

  // Add and Menu, with the page in between.
  const char* addLabel = tr(STR_NOTES_ADD);
  const char* menuLabel = tr(STR_NOTES_MENU);
  for (const auto& [rect, label] : {std::pair<Rect4, const char*>{f.add, addLabel}, {f.menu, menuLabel}}) {
    renderer.drawRoundedRect(rect.x, rect.y, rect.w, rect.h, 2, 10, true);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, rect.x + (rect.w - w) / 2,
                      rect.y + (rect.h - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label, true, EpdFontFamily::BOLD);
  }
  const int total = static_cast<int>(rows.size());
  if (total > rowsPerPage) {
    char pageText[16];
    const int pages = (total + rowsPerPage - 1) / rowsPerPage;
    snprintf(pageText, sizeof(pageText), "%d/%d", std::min(pages, topRow / rowsPerPage + 1), pages);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, pageText);
    renderer.drawText(SMALL_FONT_ID, f.body.x + (f.body.w - w) / 2,
                      f.add.y + (f.add.h - renderer.getLineHeight(SMALL_FONT_ID)) / 2, pageText);
  }
  const char* confirm = selectedParagraph >= 0 ? tr(STR_NOTES_LINE) : tr(STR_NOTES_ADD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void NotesActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  std::string title = tr(STR_NOTES);
  if (mode == Mode::View) {
    title = doc.title(48);
    if (title.empty()) title = tr(STR_NOTES_UNTITLED);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title.c_str(),
                 nullptr);
  if (mode == Mode::List) {
    renderList();
  } else {
    renderView();
  }
  present();
}

#endif  // CROSSPOINT_MESH
