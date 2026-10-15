#include <gtest/gtest.h>

#include <string>

#include "Calculator.h"

using calc::Calculator;
using calc::Error;
using calc::Key;

namespace {
void type(Calculator& c, const char* keys) {
  for (const char* p = keys; *p; p++) c.press(static_cast<Key>(*p));
}
std::string value(const char* text) { return calc::format(calc::evaluate(text).value); }
}  // namespace

TEST(Calculator, OrderOfOperations) {
  EXPECT_EQ(value("2+3*4"), "14");
  EXPECT_EQ(value("(2+3)*4"), "20");
  EXPECT_EQ(value("10-4-3"), "3");
  EXPECT_EQ(value("100/4/5"), "5");
  EXPECT_EQ(value("-3*-2"), "6");
  EXPECT_EQ(value("50%"), "0.5");
  EXPECT_EQ(value("200*15%"), "30");
  EXPECT_EQ(value("(1+2"), "3");  // closed for you
}

TEST(Calculator, Errors) {
  EXPECT_EQ(calc::evaluate("5/0").error, Error::DivideByZero);
  EXPECT_EQ(calc::evaluate("5+").error, Error::Syntax);
  EXPECT_EQ(calc::evaluate("").error, Error::Syntax);
  EXPECT_EQ(calc::evaluate("()").error, Error::Syntax);
}

TEST(Calculator, Formatting) {
  EXPECT_EQ(calc::format(0.1 + 0.2), "0.3");
  EXPECT_EQ(calc::format(1.0 / 3), "0.333333333333");
  EXPECT_EQ(calc::format(-0.0), "0");
  EXPECT_EQ(calc::format(1.5e20), "1.5e+20");
  EXPECT_EQ(calc::format(123456789012.0), "123456789012");
}

TEST(Calculator, Typing) {
  Calculator c;
  type(c, "12+3=");
  EXPECT_TRUE(c.showingResult());
  EXPECT_EQ(c.expression(), "15");
  EXPECT_EQ(c.historyResult(0), "15");
  type(c, "*2=");  // goes on from the result
  EXPECT_EQ(c.expression(), "30");
  type(c, "7");  // a digit starts afresh
  EXPECT_EQ(c.expression(), "7");
  type(c, "+*");  // the second operator replaces the first
  EXPECT_EQ(c.expression(), "7*");
  type(c, "-2=");  // but a minus after × is a negative number
  EXPECT_EQ(c.expression(), "-14");
  type(c, "C..5.");  // one point a number
  EXPECT_EQ(c.expression(), "0.5");
  type(c, "C2(3)");  // implied multiplication
  EXPECT_EQ(c.expression(), "2*(3)");
  type(c, "D");
  EXPECT_EQ(c.expression(), "2*(3");
  type(c, "C9/0=");
  EXPECT_EQ(c.lastError(), Error::DivideByZero);
  EXPECT_EQ(c.expression(), "9/0");
}

TEST(Calculator, SignAndAnswer) {
  Calculator c;
  type(c, "5S");
  EXPECT_EQ(c.expression(), "-5");
  type(c, "S");
  EXPECT_EQ(c.expression(), "5");
  type(c, "*3S");
  EXPECT_EQ(c.expression(), "5*-3");
  type(c, "=");
  EXPECT_EQ(c.expression(), "-15");
  type(c, "C2+A=");
  EXPECT_EQ(c.expression(), "-13");
  type(c, "C8-2S");
  EXPECT_EQ(c.expression(), "8+2");
}
