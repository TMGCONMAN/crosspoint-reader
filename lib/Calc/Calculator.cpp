#include "Calculator.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace calc {

namespace {

bool isOperator(char c) { return c == '+' || c == '-' || c == '*' || c == '/'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Recursive descent: expr = term {(+|-) term}; term = factor {(*|/) factor};
// factor = [-] primary [%]; primary = number | '(' expr [')'].
class Parser {
 public:
  explicit Parser(const std::string& s) : s(s) {}

  Result run() {
    Result r;
    if (s.empty()) {
      r.error = Error::Syntax;
      return r;
    }
    r.value = expr();
    if (error == Error::None && pos != s.size()) error = Error::Syntax;
    if (error == Error::None && !std::isfinite(r.value)) error = Error::Overflow;
    r.error = error;
    return r;
  }

 private:
  double expr() {
    double v = term();
    while (error == Error::None && pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
      const char op = s[pos++];
      const double rhs = term();
      v = op == '+' ? v + rhs : v - rhs;
    }
    return v;
  }
  double term() {
    double v = factor();
    while (error == Error::None && pos < s.size() && (s[pos] == '*' || s[pos] == '/')) {
      const char op = s[pos++];
      const double rhs = factor();
      if (op == '*') {
        v *= rhs;
      } else if (rhs == 0) {
        error = Error::DivideByZero;
      } else {
        v /= rhs;
      }
    }
    return v;
  }
  double factor() {
    bool negative = false;
    while (pos < s.size() && s[pos] == '-') {
      negative = !negative;
      pos++;
    }
    double v = primary();
    while (error == Error::None && pos < s.size() && s[pos] == '%') {
      v /= 100;
      pos++;
    }
    return negative ? -v : v;
  }
  double primary() {
    if (pos >= s.size()) {
      error = Error::Syntax;
      return 0;
    }
    if (s[pos] == '(') {
      pos++;
      const double v = expr();
      if (pos < s.size() && s[pos] == ')') {
        pos++;
      } else if (pos < s.size()) {
        error = Error::Syntax;
      }
      // Missing ')' at the end: closed for you.
      return v;
    }
    const size_t start = pos;
    bool point = false;
    while (pos < s.size() && (isDigit(s[pos]) || (s[pos] == '.' && !point))) {
      if (s[pos] == '.') point = true;
      pos++;
    }
    // Scientific results such as 1.5e+20 can be typed on with.
    if (pos < s.size() && s[pos] == 'e' && pos > start) {
      size_t p = pos + 1;
      if (p < s.size() && (s[p] == '+' || s[p] == '-')) p++;
      if (p < s.size() && isDigit(s[p])) {
        pos = p;
        while (pos < s.size() && isDigit(s[pos])) pos++;
      }
    }
    if (pos == start || (pos == start + 1 && s[start] == '.')) {
      error = Error::Syntax;
      return 0;
    }
    return strtod(s.substr(start, pos - start).c_str(), nullptr);
  }

  const std::string& s;
  size_t pos = 0;
  Error error = Error::None;
};

}  // namespace

Result evaluate(const std::string& text) { return Parser(text).run(); }

std::string format(double value) {
  if (value == 0) return "0";  // also -0
  char buf[40];
  const double magnitude = std::fabs(value);
  if (magnitude >= 1e12 || magnitude < 1e-9) {
    snprintf(buf, sizeof(buf), "%.6e", value);
    // Trim zeros from the mantissa: 1.500000e+20 -> 1.5e+20.
    std::string s = buf;
    const size_t e = s.find('e');
    std::string mantissa = s.substr(0, e);
    if (mantissa.find('.') != std::string::npos) {
      while (!mantissa.empty() && mantissa.back() == '0') mantissa.pop_back();
      if (!mantissa.empty() && mantissa.back() == '.') mantissa.pop_back();
    }
    return mantissa + s.substr(e);
  }
  snprintf(buf, sizeof(buf), "%.12g", value);
  return buf;
}

// ---- Calculator ----

Result Calculator::preview() const { return evaluate(text); }

size_t Calculator::lastOperandStart() const {
  size_t i = text.size();
  if (i > 0 && text[i - 1] == ')') {
    // Back to the matching '('.
    int depth = 0;
    while (i > 0) {
      i--;
      if (text[i] == ')') depth++;
      if (text[i] == '(' && --depth == 0) return i;
    }
    return 0;
  }
  while (i > 0 && (isDigit(text[i - 1]) || text[i - 1] == '.' || text[i - 1] == '%' || text[i - 1] == 'e' ||
                   ((text[i - 1] == '+' || text[i - 1] == '-') && i >= 2 && text[i - 2] == 'e'))) {
    i--;
  }
  return i;
}

void Calculator::append(const char c) {
  if (text.size() >= MAX_LENGTH) return;
  const char last = text.empty() ? '\0' : text.back();
  if (isDigit(c)) {
    if (last == ')' || last == '%') text += '*';
    // A lone leading zero is replaced: "0" then "5" is "5".
    const size_t start = lastOperandStart();
    if (text.size() - start == 1 && text[start] == '0') text.pop_back();
    text += c;
    return;
  }
  if (c == '.') {
    const size_t start = lastOperandStart();
    if (text.find('.', start) != std::string::npos) return;  // one point a number
    if (last == ')' || last == '%') text += '*';
    if (text.empty() || !isDigit(text.back())) text += '0';
    text += '.';
    return;
  }
  if (isOperator(c)) {
    if (text.empty() || last == '(') {
      if (c == '-') text += '-';  // a leading minus
      return;
    }
    // Typing a new operator replaces the last one ("5+" then "×" is "5×"),
    // but "5×-" keeps a minus for the next number.
    if (isOperator(last)) {
      if (c == '-' && (last == '*' || last == '/')) {
        text += '-';
        return;
      }
      text.pop_back();
      while (!text.empty() && isOperator(text.back())) text.pop_back();
      if (text.empty()) return;
    }
    if (text.back() == '.') text += '0';
    text += c;
    return;
  }
  if (c == '(') {
    if (!text.empty() && (isDigit(last) || last == ')' || last == '%' || last == '.')) text += '*';
    text += '(';
    return;
  }
  if (c == ')') {
    int open = 0;
    for (const char ch : text) open += ch == '(' ? 1 : (ch == ')' ? -1 : 0);
    if (open <= 0 || last == '(' || isOperator(last)) return;
    text += ')';
    return;
  }
  if (c == '%') {
    if (isDigit(last) || last == ')' || last == '.') text += '%';
  }
}

void Calculator::toggleSign() {
  if (text.empty()) {
    text = "-";
    return;
  }
  if (text == "-") {
    text.clear();
    return;
  }
  size_t start = lastOperandStart();
  if (start == text.size()) {
    // Nothing typed after an operator yet: a minus for the next number.
    if (text.back() == '-' && (text.size() == 1 || isOperator(text[text.size() - 2]) || text[text.size() - 2] == '(')) {
      text.pop_back();
    } else if (text.back() != ')') {
      text += '-';
    }
    return;
  }
  // A unary minus before the operand: remove it; otherwise add one.
  if (start > 0 && text[start - 1] == '-' && (start == 1 || isOperator(text[start - 2]) || text[start - 2] == '(')) {
    text.erase(start - 1, 1);
  } else if (start > 0 && text[start - 1] == '+') {
    text[start - 1] = '-';
  } else if (start > 0 && text[start - 1] == '-') {
    text[start - 1] = '+';
  } else if (text.size() < MAX_LENGTH) {
    text.insert(start, 1, '-');
  }
}

void Calculator::press(const Key key) {
  const char c = static_cast<char>(key);
  error = Error::None;
  if (key == Key::Clear) {
    text.clear();
    justEvaluated = false;
    return;
  }
  if (key == Key::Equals) {
    const Result r = evaluate(text);
    if (text.empty()) return;
    if (!r.ok()) {
      error = r.error;
      return;
    }
    // Close what was left open, for the history line.
    std::string shown = text;
    int open = 0;
    for (const char ch : shown) open += ch == '(' ? 1 : (ch == ')' ? -1 : 0);
    while (open-- > 0) shown += ')';
    for (int i = HISTORY - 1; i > 0; i--) history[i] = history[i - 1];
    answer = format(r.value);
    history[0] = Entry{shown, answer};
    if (historyUsed < HISTORY) historyUsed++;
    text = answer;
    justEvaluated = true;
    return;
  }
  // After a result: a digit or point starts afresh, an operator goes on.
  if (justEvaluated) {
    justEvaluated = false;
    if (isDigit(c) || key == Key::Point || key == Key::Answer || key == Key::Open) text.clear();
  }
  if (key == Key::Delete) {
    if (!text.empty()) text.pop_back();
    return;
  }
  if (key == Key::Sign) {
    toggleSign();
    return;
  }
  if (key == Key::Answer) {
    const char last = text.empty() ? '\0' : text.back();
    if (isDigit(last) || last == '.' || last == ')' || last == '%') text += '*';
    if (text.size() + answer.size() <= MAX_LENGTH) text += answer;
    return;
  }
  append(c);
}

}  // namespace calc
