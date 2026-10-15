#include "CalculatorActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <string>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

using calc::Key;

namespace {

constexpr int FULL_REFRESH_EVERY = 30;
constexpr int GAP = 8;
constexpr int RADIUS = 12;
constexpr int KEY_FONT = NOTOSANS_18_FONT_ID;
constexpr int BIG_FONT = NOTOSANS_18_FONT_ID;

enum class Style : uint8_t { Digit, Operator, Function, Equals, None };
struct KeyDef {
  const char* label;  // nullptr: the label comes from a string id
  Key key;
  Style style;
  int span;  // 1, or 2 for a wide key; 0 for the cell a wide key covers
};

// Row by row; "=" is two cells wide.
constexpr KeyDef KEYS[6][4] = {
    {{"C", Key::Clear, Style::Function, 1},
     {"(", Key::Open, Style::Function, 1},
     {")", Key::Close, Style::Function, 1},
     {nullptr, Key::Delete, Style::Function, 1}},
    {{"7", Key::D7, Style::Digit, 1},
     {"8", Key::D8, Style::Digit, 1},
     {"9", Key::D9, Style::Digit, 1},
     {"\xC3\xB7", Key::Divide, Style::Operator, 1}},
    {{"4", Key::D4, Style::Digit, 1},
     {"5", Key::D5, Style::Digit, 1},
     {"6", Key::D6, Style::Digit, 1},
     {"\xC3\x97", Key::Multiply, Style::Operator, 1}},
    {{"1", Key::D1, Style::Digit, 1},
     {"2", Key::D2, Style::Digit, 1},
     {"3", Key::D3, Style::Digit, 1},
     {"\xE2\x88\x92", Key::Subtract, Style::Operator, 1}},
    {{"\xC2\xB1", Key::Sign, Style::Function, 1},
     {"0", Key::D0, Style::Digit, 1},
     {".", Key::Point, Style::Digit, 1},
     {"+", Key::Add, Style::Operator, 1}},
    {{"%", Key::Percent, Style::Function, 1},
     {nullptr, Key::Answer, Style::Function, 1},
     {"=", Key::Equals, Style::Equals, 2},
     {"", Key::Equals, Style::None, 0}},
};

// The expression as it is shown: × ÷ and a true minus sign (fonts without
// it get a hyphen).
std::string shown(const std::string& text, const bool trueMinus) {
  std::string out;
  for (const char c : text) {
    if (c == '*') {
      out += "\xC3\x97";
    } else if (c == '/') {
      out += "\xC3\xB7";
    } else if (c == '-' && trueMinus && (out.empty() || out.back() != 'e')) {
      out += "\xE2\x88\x92";
    } else {
      out += c;
    }
  }
  return out;
}

const char* errorText(const calc::Error error) {
  switch (error) {
    case calc::Error::DivideByZero:
      return tr(STR_CALC_DIVIDE_BY_ZERO);
    case calc::Error::Overflow:
      return tr(STR_CALC_TOO_BIG);
    case calc::Error::Syntax:
      return tr(STR_CALC_INCOMPLETE);
    case calc::Error::None:
    default:
      return "";
  }
}

}  // namespace

CalculatorActivity::CalculatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Calculator", renderer, mappedInput) {}

void CalculatorActivity::onEnter() {
  Activity::onEnter();
  fullRefresh = true;
  requestUpdate();
}

CalculatorActivity::Layout CalculatorActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  const int side = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int left = content.x + side;
  const int width = content.width - 2 * side;
  const int displayH = renderer.getLineHeight(SMALL_FONT_ID) * 2 + renderer.getLineHeight(UI_12_FONT_ID) +
                       renderer.getLineHeight(BIG_FONT) + 28;
  Layout out;
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  int padX = left;
  int padY = top + displayH + GAP;
  int padW = width;
  if (landscape) {
    // The display on the left, the keypad on the right.
    out.display = Rect4{left, top, width * 2 / 5, bottom - top};
    padX = left + out.display.w + GAP * 2;
    padY = top;
    padW = left + width - padX;
  } else {
    out.display = Rect4{left, top, width, displayH};
  }
  const int keyW = (padW - GAP * (COLUMNS - 1)) / COLUMNS;
  const int keyH = std::min(keyW, (bottom - padY - GAP * (ROWS - 1)) / ROWS);
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLUMNS; c++) {
      const int span = std::max(1, KEYS[r][c].span);
      out.keys[r][c] = Rect4{padX + c * (keyW + GAP), padY + r * (keyH + GAP), keyW * span + GAP * (span - 1), keyH};
    }
  }
  return out;
}

void CalculatorActivity::press(const int row, const int column) {
  const KeyDef& def = KEYS[row][column];
  if (def.style == Style::None) return;
  {
    RenderLock lock(*this);
    calculator.press(def.key);
  }
  if (++fastRefreshes >= FULL_REFRESH_EVERY) fullRefresh = true;
  requestUpdate();
}

void CalculatorActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    showCursor = false;
    const Layout grid = layout();
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLUMNS; c++) {
        if (KEYS[r][c].span == 0 || !grid.keys[r][c].contains(tx, ty)) continue;
        haptic_feedback::touchAction();
        press(r, c);
        return;
      }
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!showCursor) {
      showCursor = true;
      requestUpdate();
      return;
    }
    press(cursorRow, cursorColumn);
    return;
  }
  // Front Left/Right move along a row, the side buttons between rows.
  int row = cursorRow;
  int column = cursorColumn;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    column = (column + KEYS[row][column].span) % COLUMNS;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    column = (column + COLUMNS - 1) % COLUMNS;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    row = (row + 1) % ROWS;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    row = (row + ROWS - 1) % ROWS;
  } else {
    return;
  }
  // The covered half of a wide key belongs to the key on its left.
  if (KEYS[row][column].span == 0) column--;
  cursorRow = row;
  cursorColumn = column;
  showCursor = true;
  requestUpdate();
}

void CalculatorActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_CALCULATOR), nullptr);
  const Layout grid = layout();

  // The display: recent results, then the sum and its value.
  const Rect4& d = grid.display;
  renderer.drawRoundedRect(d.x, d.y, d.w, d.h, 2, RADIUS, true);
  const int pad = 12;
  const int innerW = d.w - 2 * pad;
  const int right = d.x + d.w - pad;
  const auto drawRight = [&](int font, int y, const std::string& text, EpdFontFamily::Style style) {
    // A long line keeps its end, where the typing is.
    std::string t = text;
    while (t.size() > 1 && renderer.getTextWidth(font, t.c_str(), style) > innerW) {
      size_t cut = 1;
      while (cut < t.size() && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) cut++;
      t = t.substr(cut);
    }
    renderer.drawText(font, right - renderer.getTextWidth(font, t.c_str(), style), y, t.c_str(), true, style);
  };
  const int small = renderer.getLineHeight(SMALL_FONT_ID);
  int y = d.y + pad;
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  const int historyLines = landscape ? calc::Calculator::HISTORY : 2;
  for (int i = std::min(historyLines, calculator.historyCount()) - 1; i >= 0; i--) {
    const std::string line =
        shown(calculator.historyExpression(i), false) + " = " + shown(calculator.historyResult(i), false);
    drawRight(SMALL_FONT_ID, y, line, EpdFontFamily::REGULAR);
    y += small;
  }
  const int bigH = renderer.getLineHeight(BIG_FONT);
  const int exprH = renderer.getLineHeight(UI_12_FONT_ID);
  const int bottomY = d.y + d.h - pad - bigH;
  const std::string expression = calculator.expression();
  if (calculator.showingResult()) {
    drawRight(BIG_FONT, bottomY, shown(expression, true), EpdFontFamily::BOLD);
  } else {
    drawRight(UI_12_FONT_ID, bottomY - exprH - 4, shown(expression.empty() ? "0" : expression, false),
              EpdFontFamily::REGULAR);
    std::string value;
    if (calculator.lastError() != calc::Error::None) {
      drawRight(UI_12_FONT_ID, bottomY + (bigH - exprH) / 2, errorText(calculator.lastError()), EpdFontFamily::BOLD);
    } else if (!expression.empty()) {
      const calc::Result r = calculator.preview();
      if (r.ok()) value = "= " + shown(calc::format(r.value), true);
    }
    if (!value.empty()) drawRight(BIG_FONT, bottomY, value, EpdFontFamily::BOLD);
  }

  // The keypad.
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLUMNS; c++) {
      const KeyDef& def = KEYS[r][c];
      if (def.span == 0) continue;
      const Rect4& k = grid.keys[r][c];
      const bool filled = def.style == Style::Equals;
      if (filled) {
        renderer.fillRoundedRect(k.x, k.y, k.w, k.h, RADIUS, Color::Black);
      } else {
        renderer.drawRoundedRect(k.x, k.y, k.w, k.h, def.style == Style::Operator ? 3 : 2, RADIUS, true);
      }
      if (showCursor && r == cursorRow && c == cursorColumn) {
        renderer.drawRoundedRect(k.x + 4, k.y + 4, k.w - 8, k.h - 8, 3, RADIUS - 4, !filled);
      }
      const char* label = def.label ? def.label : (def.key == Key::Delete ? tr(STR_CALC_DELETE) : tr(STR_CALC_ANSWER));
      const int font = def.label ? KEY_FONT : UI_12_FONT_ID;
      const auto style = def.style == Style::Digit ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
      const int w = renderer.getTextWidth(font, label, style);
      renderer.drawText(font, k.x + (k.w - w) / 2, k.y + (k.h - renderer.getLineHeight(font)) / 2, label, !filled,
                        style);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (fullRefresh) {
    fullRefresh = false;
    fastRefreshes = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

#endif  // CROSSPOINT_MESH
