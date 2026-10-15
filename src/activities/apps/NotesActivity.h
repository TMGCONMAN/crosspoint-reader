#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <NoteDocument.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Notes: plain text files in /notes on the SD card, one paragraph per line,
// so they can also be written or read on a computer (or through File
// Transfer). The list shows each note by its first line. In a note, tap a
// paragraph for Edit, Add below, a checkbox or Delete; tap a checkbox to
// tick it. Each paragraph is typed on the on-screen keyboard. Every change
// is saved at once.
class NotesActivity final : public Activity {
 public:
  NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Mode : uint8_t { List, View };
  // Work a popup asks for, done on the next loop pass (a popup callback may
  // not open another popup or the keyboard).
  enum class Pending : uint8_t {
    None,
    NewNote,
    NewChecklist,
    Edit,
    EditAll,
    AddBelow,
    AddEnd,
    ConfirmDelete,
    DeleteNote
  };
  struct Entry {
    std::string file;
    std::string title;
  };
  // One wrapped line of a note's text.
  struct Row {
    uint16_t paragraph;
    uint16_t start;  // bytes into the paragraph's text after any checkbox
    uint16_t end;
    bool first;
  };
  // Where a drawn paragraph line is, for taps.
  struct Hit {
    int y0;
    int y1;
    int paragraph;
    bool first;
  };
  struct Rect4 {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  struct Frame {
    Rect4 body;
    Rect4 add;
    Rect4 menu;
  };
  static constexpr int MAX_HITS = 64;

  Frame frame() const;
  int lineHeight() const;
  int listRowHeight() const;
  int listRowsPerPage() const;

  void loadList();
  void openNote(const std::string& file);
  bool saveNote();
  void closeNote();
  void runPending();
  void openKeyboard(const char* title, const std::string& initial, Pending purpose, int index);
  void paragraphMenu(int index);
  void noteMenu();
  void showMessage(const char* text);
  void layoutRows();  // with the render lock held
  void ensureVisible(int paragraph);
  void pageBy(int direction);

  void listLoop();
  void viewLoop();
  void renderList();
  void renderView();
  void present();

  Mode mode = Mode::List;
  std::vector<Entry> entries;
  int listSelected = 0;  // 0 is "New note", 1 "New checklist"
  int listTop = 0;
  bool showCursor = false;

  std::string currentFile;
  notes::NoteDocument doc;
  std::vector<Row> rows;
  bool rowsDirty = true;
  int topRow = 0;
  int rowsPerPage = 1;
  int selectedParagraph = -1;
  Hit hits[MAX_HITS] = {};
  int hitCount = 0;

  Pending pending = Pending::None;
  int pendingIndex = -1;
  OptionPopup optionPopup;
  char messageText[160] = {0};
  bool fullRefresh = true;
  int fastRefreshes = 0;
};

#endif  // CROSSPOINT_MESH
