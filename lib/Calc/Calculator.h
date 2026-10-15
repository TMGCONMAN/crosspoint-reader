#pragma once

#include <cstddef>
#include <string>

// A pocket calculator: the expression is typed key by key and evaluated
// with the usual order (× ÷ before + −), parentheses and unary minus.
// x% is x / 100. Kept as ASCII ('*', '/'); the screen draws × and ÷.
namespace calc {

enum class Key : char {
  D0 = '0',
  D1 = '1',
  D2 = '2',
  D3 = '3',
  D4 = '4',
  D5 = '5',
  D6 = '6',
  D7 = '7',
  D8 = '8',
  D9 = '9',
  Point = '.',
  Add = '+',
  Subtract = '-',
  Multiply = '*',
  Divide = '/',
  Open = '(',
  Close = ')',
  Percent = '%',
  Equals = '=',
  Clear = 'C',
  Delete = 'D',
  Sign = 'S',
  Answer = 'A'
};

enum class Error : unsigned char { None, Syntax, DivideByZero, Overflow };

struct Result {
  double value = 0;
  Error error = Error::None;
  bool ok() const { return error == Error::None; }
};

// Evaluates text; open parentheses are closed at the end.
Result evaluate(const std::string& text);
// At most 12 significant digits; scientific notation for very large or
// very small numbers.
std::string format(double value);

class Calculator {
 public:
  static constexpr size_t MAX_LENGTH = 64;
  static constexpr int HISTORY = 4;

  void press(Key key);
  const std::string& expression() const { return text; }
  // The value of the expression so far, if it has one.
  Result preview() const;
  // After '=': the result is shown until the next key.
  bool showingResult() const { return justEvaluated; }
  Error lastError() const { return error; }
  int historyCount() const { return historyUsed; }
  // 0 is the newest: "12+3" and "15".
  const std::string& historyExpression(int i) const { return history[i].expression; }
  const std::string& historyResult(int i) const { return history[i].result; }

 private:
  struct Entry {
    std::string expression;
    std::string result;
  };
  void append(char c);
  void toggleSign();
  // Start of the number (or parenthesis group) that ends the text.
  size_t lastOperandStart() const;

  std::string text;
  std::string answer = "0";
  bool justEvaluated = false;
  Error error = Error::None;
  Entry history[HISTORY];
  int historyUsed = 0;
};

}  // namespace calc
