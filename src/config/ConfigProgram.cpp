#include "config/ConfigProgram.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tao/pegtl.hpp>
#include <xkbcommon/xkbcommon.h>

namespace {
namespace p = tao::pegtl;
constexpr size_t max_text = 65536;
// These bits are the standard wlroots modifier mask, independent of keysyms.
constexpr uint32_t shift = 1, ctrl = 4, alt = 8, mod3 = 32, meta = 64,
                   mod5 = 128;
constexpr uint32_t binding_modifiers = shift | ctrl | alt | mod3 | meta | mod5;
struct Token {
  std::string text;
  size_t line, column;
};
namespace lex {
struct identifier : p::seq<p::sor<p::alpha, p::one<'_'>>,
                           p::star<p::sor<p::alnum, p::one<'_'>>>> {};
struct number
    : p::seq<p::plus<p::digit>, p::opt<p::one<'.'>, p::plus<p::digit>>,
             p::opt<p::one<'e', 'E'>, p::opt<p::one<'+', '-'>>,
                    p::plus<p::digit>>> {};
struct string
    : p::seq<
          p::one<'"'>,
          p::star<p::sor<p::seq<p::one<'\\'>, p::one<'"', '\\', 'n', 'r', 't'>>,
                         p::not_one<'"', '\\', '\n', '\r'>>>,
          p::one<'"'>> {};
struct op
    : p::sor<p::string<'=', '='>, p::string<'!', '='>, p::string<'<', '='>,
             p::string<'>', '='>, p::string<'&', '&'>, p::string<'|', '|'>,
             p::one<'(', ')', ',', ';', '=', '+', '-', '*', '/', '%', '!', '<',
                    '>', '?', ':'>> {};
struct comment : p::seq<p::one<'#'>, p::until<p::eolf>> {};
struct grammar
    : p::must<
          p::star<p::sor<p::space, comment, string, number, identifier, op>>,
          p::eof> {};
template <class Rule> struct action : p::nothing<Rule> {};
template <class Input> void token(const Input &in, std::vector<Token> &tokens) {
  if (tokens.size() >= 32768)
    throw std::runtime_error("configuration program exceeds 32768 tokens");
  const auto pos = in.position();
  tokens.push_back({in.string(), pos.line, pos.column});
}
template <> struct action<identifier> {
  template <class I> static void apply(const I &i, std::vector<Token> &t) {
    token(i, t);
  }
};
template <> struct action<number> {
  template <class I> static void apply(const I &i, std::vector<Token> &t) {
    token(i, t);
  }
};
template <> struct action<string> {
  template <class I> static void apply(const I &i, std::vector<Token> &t) {
    token(i, t);
  }
};
template <> struct action<op> {
  template <class I> static void apply(const I &i, std::vector<Token> &t) {
    token(i, t);
  }
};
} // namespace lex
struct Node;
using Expr = std::shared_ptr<const Node>;
struct Node {
  enum Kind { literal, variable, call, unary, binary, conditional } kind;
  Token token;
  ConfigValue value = 0.;
  std::vector<Expr> children;
  unsigned height = 1;
};
struct Definition {
  bool macro;
  std::vector<std::string> parameters;
  Expr body;
  Token token;
};
struct Statement {
  enum Kind { let, definition, binding, eval } kind;
  Token name;
  Expr expression;
  Definition def{};
};

class Parser {
  std::vector<Token> tokens_;
  size_t pos_ = 0;
  unsigned depth_ = 0;
  std::string name_;
  const Token &peek() const { return tokens_[pos_]; }
  Token take() {
    if (peek().text.empty())
      fail("unexpected end of program");
    return tokens_[pos_++];
  }
  [[noreturn]] void fail(const std::string &message) const {
    throw std::runtime_error(name_ + ":" + std::to_string(peek().line) + ":" +
                             std::to_string(peek().column) + ": " + message);
  }
  bool accept(const std::string &s) {
    if (peek().text != s)
      return false;
    ++pos_;
    return true;
  }
  void expect(const std::string &s) {
    if (!accept(s))
      fail("expected '" + s + "'");
  }
  Token identifier() {
    if (peek().text.empty() ||
        !(std::isalpha(static_cast<unsigned char>(peek().text[0])) ||
          peek().text[0] == '_'))
      fail("expected identifier");
    return take();
  }
  static int precedence(const std::string &s) {
    if (s == "||")
      return 1;
    if (s == "&&")
      return 2;
    if (s == "==" || s == "!=")
      return 3;
    if (s == "<" || s == ">" || s == "<=" || s == ">=")
      return 4;
    if (s == "+" || s == "-")
      return 5;
    if (s == "*" || s == "/" || s == "%")
      return 6;
    return 0;
  }
  Expr checked(std::shared_ptr<Node> node) {
    for (const auto &child : node->children)
      node->height = std::max(node->height, child->height + 1);
    if (node->height > 64)
      fail("expression tree exceeds 64 levels");
    return node;
  }
  Expr atom() {
    Token token = take();
    if (token.text == "(") {
      auto value = expression();
      expect(")");
      return value;
    }
    if (token.text == "-" || token.text == "+" || token.text == "!")
      return checked(std::make_shared<Node>(
          Node{Node::unary, token, 0., {expression(7)}}));
    auto node = std::make_shared<Node>();
    node->token = token;
    node->kind = Node::literal;
    if (token.text[0] == '"') {
      std::string value;
      for (size_t i = 1; i + 1 < token.text.size(); ++i) {
        char c = token.text[i];
        if (c == '\\') {
          c = token.text[++i];
          if (c == 'n')
            c = '\n';
          else if (c == 'r')
            c = '\r';
          else if (c == 't')
            c = '\t';
        }
        value += c;
      }
      if (value.size() > max_text)
        fail("string exceeds 64 KiB");
      node->value = std::move(value);
    } else if (std::isdigit(static_cast<unsigned char>(token.text[0]))) {
      double value;
      const auto result = std::from_chars(
          token.text.data(), token.text.data() + token.text.size(), value);
      if (result.ec != std::errc() || !std::isfinite(value))
        fail("number out of range");
      node->value = value;
    } else if (token.text == "true" || token.text == "false")
      node->value = token.text == "true";
    else if (std::isalpha(static_cast<unsigned char>(token.text[0])) ||
             token.text[0] == '_') {
      node->kind = Node::variable;
      if (accept("(")) {
        node->kind = Node::call;
        if (!accept(")"))
          do {
            if (node->children.size() >= 64)
              fail("too many arguments");
            node->children.push_back(expression());
            if (accept(")"))
              break;
            expect(",");
          } while (true);
      }
    } else
      fail("expected expression");
    return checked(node);
  }

public:
  Parser(const std::string &source, std::string name) : name_(std::move(name)) {
    if (source.size() > 262144)
      throw std::runtime_error(name_ + ": program exceeds 256 KiB");
    p::memory_input input(source, name_);
    p::parse<lex::grammar, lex::action>(input, tokens_);
    auto pos = input.position();
    tokens_.push_back({"", pos.line, pos.column});
  }
  Expr expression(int minimum = 1) {
    if (++depth_ > 64)
      fail("expression nesting exceeds 64");
    auto left = atom();
    while (int level = precedence(peek().text)) {
      if (level < minimum)
        break;
      auto op = take();
      auto right = expression(level + 1);
      left = checked(
          std::make_shared<Node>(Node{Node::binary, op, 0., {left, right}}));
    }
    if (minimum == 1 && accept("?")) {
      auto yes = expression();
      expect(":");
      auto no = expression();
      left = checked(std::make_shared<Node>(
          Node{Node::conditional, left->token, 0., {left, yes, no}}));
    }
    --depth_;
    return left;
  }
  Expr single() {
    auto result = expression();
    if (!peek().text.empty())
      fail("unexpected token");
    return result;
  }
  std::vector<Statement> program() {
    std::vector<Statement> statements;
    while (!peek().text.empty()) {
      if (statements.size() >= 1024)
        fail("program exceeds 1024 statements");
      auto token = identifier();
      Statement s{};
      if (token.text == "let") {
        s.kind = Statement::let;
        s.name = identifier();
        expect("=");
        s.expression = expression();
      } else if (token.text == "fn" || token.text == "macro") {
        s.kind = Statement::definition;
        s.name = identifier();
        s.def.macro = token.text == "macro";
        s.def.token = s.name;
        expect("(");
        if (!accept(")"))
          do {
            auto name = identifier().text;
            if (s.def.parameters.size() >= 32 ||
                std::find(s.def.parameters.begin(), s.def.parameters.end(),
                          name) != s.def.parameters.end())
              fail("duplicate or excessive parameters");
            s.def.parameters.push_back(name);
            if (accept(")"))
              break;
            expect(",");
          } while (true);
        expect("=");
        s.def.body = expression();
      } else if (token.text == "bind") {
        --pos_;
        s.kind = Statement::binding;
        s.name = token;
        s.expression = expression();
        if (s.expression->kind != Node::call ||
            s.expression->token.text != "bind")
          fail("expected bind(...) statement");
      } else if (token.text == "eval") {
        s.kind = Statement::eval;
        s.name = token;
        s.expression = expression();
      } else
        fail("expected let, fn, macro, bind or eval");
      expect(";");
      statements.push_back(std::move(s));
    }
    return statements;
  }
};

const std::set<std::string> reserved{"keysym", "keycode", "str",  "commands",
                                     "Composite", "bind",   "let",     "fn",   "macro",
                                     "eval",   "true",    "false"};
using Environment = std::map<std::string, std::function<ConfigValue()>>;
uint32_t parse_modifiers(const std::string &text) {
  uint32_t result = 0;
  if (text.empty() || text == "None")
    return 0;
  std::istringstream input(text);
  std::string part;
  while (std::getline(input, part, '+')) {
    uint32_t bit = part == "Meta" || part == "Super" ? meta
                   : part == "Ctrl"                  ? ctrl
                   : part == "Shift"                 ? shift
                   : part == "Alt"                   ? alt
                   : part == "Mod3"                  ? mod3
                   : part == "Mod5"                  ? mod5
                                                     : 0;
    if (!bit || (result & bit))
      throw std::runtime_error("invalid or duplicate modifier: " + part);
    result |= bit;
  }
  if (text.back() == '+')
    throw std::runtime_error("trailing '+' in modifiers");
  return result;
}
std::string scalar_text(const ConfigValue &value) {
  if (auto s = std::get_if<std::string>(&value))
    return *s;
  if (auto n = std::get_if<double>(&value)) {
    char buffer[64];
    auto r = std::to_chars(buffer, buffer + sizeof(buffer), *n);
    return std::string(buffer, r.ptr);
  }
  if (auto b = std::get_if<bool>(&value))
    return *b ? "true" : "false";
  throw std::runtime_error("expected a string, number or boolean; typed keys "
                           "and command lists are not scalar settings");
}
} // namespace

struct ConfigProgram::Impl {
  std::string name;
  std::map<std::string, ConfigValue> variables;
  std::map<std::string, Definition> definitions;
  std::vector<ConfigBinding> bindings;
  struct Evaluator {
    const Impl &program;
    size_t steps = 0;
    unsigned depth = 0;
    [[noreturn]] void fail(const Expr &e, const std::string &message) const {
      throw std::runtime_error(
          program.name + ":" + std::to_string(e->token.line) + ":" +
          std::to_string(e->token.column) + ": " + message);
    }
    template <class T>
    const T &as(const ConfigValue &v, const Expr &e, const char *type) const {
      if (const auto *p = std::get_if<T>(&v))
        return *p;
      fail(e, std::string("expected ") + type);
    }
    ConfigValue eval(const Expr &e, const Environment &locals = {}) {
      if (++steps > 10000 || ++depth > 64)
        fail(e, "evaluation budget exceeded (10000 steps / 64 calls)");
      struct Guard {
        unsigned &depth;
        ~Guard() { --depth; }
      } guard{depth};
      const auto &name = e->token.text;
      if (e->kind == Node::literal)
        return e->value;
      if (e->kind == Node::variable) {
        if (auto i = locals.find(name); i != locals.end())
          return i->second();
        if (auto i = program.variables.find(name); i != program.variables.end())
          return i->second;
        fail(e, "undefined variable: " + name);
      }
      if (e->kind == Node::conditional) {
        auto condition = eval(e->children[0], locals);
        return eval(e->children[as<bool>(condition, e, "boolean") ? 1 : 2],
                    locals);
      }
      if (e->kind == Node::unary) {
        auto value = eval(e->children[0], locals);
        if (name == "!")
          return !as<bool>(value, e, "boolean");
        return as<double>(value, e, "number") * (name == "-" ? -1 : 1);
      }
      if (e->kind == Node::binary) {
        auto lhs = eval(e->children[0], locals);
        if (name == "&&" && !as<bool>(lhs, e, "boolean"))
          return false;
        if (name == "||" && as<bool>(lhs, e, "boolean"))
          return true;
        auto rhs = eval(e->children[1], locals);
        if (name == "&&" || name == "||")
          return as<bool>(rhs, e, "boolean");
        if (name == "==" || name == "!=")
          return (lhs == rhs) == (name == "==");
        if (name == "+" && std::holds_alternative<std::string>(lhs)) {
          auto result = as<std::string>(lhs, e, "string") +
                        as<std::string>(rhs, e, "string");
          if (result.size() > max_text)
            fail(e, "string exceeds 64 KiB");
          return result;
        }
        const double a = as<double>(lhs, e, "number"),
                     b = as<double>(rhs, e, "number");
        if (name == "<")
          return a < b;
        if (name == ">")
          return a > b;
        if (name == "<=")
          return a <= b;
        if (name == ">=")
          return a >= b;
        if ((name == "/" || name == "%") && b == 0)
          fail(e, "division by zero");
        double result = name == "+"   ? a + b
                        : name == "-" ? a - b
                        : name == "*" ? a * b
                        : name == "/" ? a / b
                                      : std::fmod(a, b);
        if (!std::isfinite(result))
          fail(e, "non-finite arithmetic result");
        return result;
      }
      if (auto it = program.definitions.find(name);
          it != program.definitions.end()) {
        const auto &def = it->second;
        if (def.parameters.size() != e->children.size())
          fail(e, "wrong argument count for " + name);
        Environment arguments;
        for (size_t i = 0; i < def.parameters.size(); ++i) {
          if (def.macro) {
            auto expression = e->children[i];
            arguments.emplace(def.parameters[i], [this, expression, &locals] {
              return eval(expression, locals);
            });
          } else {
            auto value = eval(e->children[i], locals);
            arguments.emplace(def.parameters[i], [value] { return value; });
          }
        }
        // Lexical global scope; macro parameters are expression thunks from the
        // caller.
        return eval(def.body, arguments);
      }
      std::vector<ConfigValue> args;
      for (const auto &child : e->children)
        args.push_back(eval(child, locals));
      if (name == "commands") {
        if (args.empty() || args.size() > 64)
          fail(e, "commands() requires 1..64 arguments");
        ConfigCommands commands;
        for (const auto &arg : args) {
          if (const auto *list = std::get_if<ConfigCommands>(&arg))
            commands.values.insert(commands.values.end(), list->values.begin(),
                                   list->values.end());
          else
            commands.values.push_back(
                as<std::string>(arg, e, "command string"));
        }
        std::string error;
        std::vector<std::string> checked;
        if (!ConfigProgram::commands(commands, checked, error))
          fail(e, error);
        return commands;
      }
      if (name == "Composite") {
        if (args.empty() || args.size() > 64)
          fail(e, "Composite() requires 1..64 arguments");
        std::string result;
        for (const auto &arg : args) {
          const auto &part = as<std::string>(arg, e, "key or modifier string");
          if (part.empty() || part.find('+') != std::string::npos)
            fail(e, "Composite() names must be nonempty and must not "
                    "contain '+'");
          if (!result.empty())
            result.push_back('+');
          result += part;
          if (result.size() > max_text)
            fail(e, "string exceeds 64 KiB");
        }
        return result;
      }
      if (name != "keycode" && name != "keysym" && name != "str")
        fail(e, "undefined function or macro: " + name);
      if (args.size() != 1)
        fail(e, name + "() requires one argument");
      if (name == "str")
        return scalar_text(args[0]);
      if (name == "keycode") {
        double code = as<double>(args[0], e, "numeric evdev keycode");
        if (code < 1 || code > 767 || std::trunc(code) != code)
          fail(e, "keycode must be an integer from 1 to 767 (Linux evdev, "
                  "without XKB's +8 offset)");
        return ConfigKeycode{static_cast<uint32_t>(code)};
      }
      const auto &key = as<std::string>(args[0], e, "keysym name string");
      auto sym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_NO_FLAGS);
      if (sym == XKB_KEY_NoSymbol)
        fail(e, "unknown keysym: " + key);
      return ConfigKeysym{sym};
    }
  };
};

ConfigProgram::ConfigProgram() : impl_(std::make_unique<Impl>()) {}
ConfigProgram::~ConfigProgram() = default;
std::shared_ptr<const ConfigProgram>
ConfigProgram::compile(const std::string &source, const std::string &name,
                       std::string &error) {
  try {
    auto result = std::shared_ptr<ConfigProgram>(new ConfigProgram);
    auto &p = *result->impl_;
    p.name = name;
    const auto statements = Parser(source, name).program();
    std::set<std::string> names = reserved;
    for (const auto &s : statements) {
      if (s.kind == Statement::definition || s.kind == Statement::let) {
        if (!names.insert(s.name.text).second)
          throw std::runtime_error(
              name + ":" + std::to_string(s.name.line) +
              ": duplicate or reserved name: " + s.name.text);
      }
      if (s.kind == Statement::definition) {
        for (const auto &parameter : s.def.parameters)
          if (reserved.count(parameter))
            throw std::runtime_error("reserved parameter: " + parameter);
        p.definitions.emplace(s.name.text, s.def);
      }
    }
    Impl::Evaluator evaluator{p};
    // Declarations are available throughout the program; variable initializers
    // are evaluated in source order, then bindings are compiled in a second
    // pass.
    for (const auto &s : statements) {
      if (s.kind == Statement::let)
        p.variables.emplace(s.name.text, evaluator.eval(s.expression));
      else if (s.kind == Statement::eval)
        (void)evaluator.eval(s.expression);
    }
    for (const auto &s : statements) {
      if (s.kind != Statement::binding)
        continue;
      const auto &args = s.expression->children;
      if (args.size() < 3 || args.size() > 4)
        throw std::runtime_error(
            "bind(modifiers, key, command[, scope]) requires 3 or 4 arguments");
      auto modifiers = evaluator.eval(args[0]);
      auto key = evaluator.eval(args[1]);
      auto action = evaluator.eval(args[2]);
      ConfigBinding binding;
      binding.modifiers = parse_modifiers(evaluator.as<std::string>(
          modifiers, s.expression, "modifier string"));
      if (auto sym = std::get_if<ConfigKeysym>(&key))
        binding.key = *sym;
      else if (auto code = std::get_if<ConfigKeycode>(&key))
        binding.key = *code;
      else
        throw std::runtime_error("binding key must be keysym(...) or "
                                 "keycode(...), not a plain string or number");
      if (args.size() == 4) {
        auto scope = evaluator.eval(args[3]);
        auto text =
            evaluator.as<std::string>(scope, s.expression, "scope string");
        if (text != "cell" && text != "global")
          throw std::runtime_error("binding scope must be cell or global");
        binding.global = text == "global";
      }
      if (!commands(action, binding.commands, error))
        throw std::runtime_error(error);
      if (p.bindings.size() >= 256)
        throw std::runtime_error("at most 256 bindings are allowed");
      for (const auto &previous : p.bindings)
        if (previous.modifiers == binding.modifiers &&
            previous.key == binding.key)
          throw std::runtime_error("duplicate binding");
      p.bindings.push_back(std::move(binding));
    }
    error.clear();
    return result;
  } catch (const std::exception &exception) {
    error = exception.what();
    return {};
  }
}
bool ConfigProgram::evaluate(const std::string &expression, ConfigValue &out,
                             std::string &error) const {
  try {
    auto ast = Parser(expression, impl_->name + ":eval").single();
    Impl::Evaluator evaluator{*impl_};
    auto result = evaluator.eval(ast);
    out = std::move(result);
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    error = exception.what();
    return false;
  }
}
bool ConfigProgram::expand(const std::string &input, std::string &out,
                           std::string &error) const {
  if (!input.starts_with("$(")) {
    out = input;
    return true;
  }
  if (input.size() < 3 || input.back() != ')') {
    error = "setting expression must end with ')'";
    return false;
  }
  ConfigValue value;
  if (!evaluate(input.substr(2, input.size() - 3), value, error))
    return false;
  try {
    out = scalar_text(value);
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
const std::vector<ConfigBinding> &ConfigProgram::bindings() const {
  return impl_->bindings;
}
const std::map<std::string, ConfigValue> &ConfigProgram::variables() const {
  return impl_->variables;
}
bool ConfigProgram::commands(const ConfigValue &value,
                             std::vector<std::string> &out,
                             std::string &error) {
  std::vector<std::string> result;
  if (auto s = std::get_if<std::string>(&value))
    result.push_back(*s);
  else if (auto list = std::get_if<ConfigCommands>(&value))
    result = list->values;
  else {
    error = "action must return a command string or commands(...)";
    return false;
  }
  size_t total = 0;
  if (result.empty() || result.size() > 64) {
    error = "action must contain 1..64 commands";
    return false;
  }
  for (const auto &text : result) {
    total += text.size();
    if (text.empty() || text.size() > 4096 || total > max_text ||
        text.find('\0') != std::string::npos) {
      error = "commands must be nonempty, at most 4096 bytes each and 64 KiB "
              "in total";
      return false;
    }
  }
  out = std::move(result);
  return true;
}
std::string ConfigProgram::describe(const ConfigValue &value) {
  if (auto sym = std::get_if<ConfigKeysym>(&value)) {
    char name[128];
    xkb_keysym_get_name(sym->value, name, sizeof(name));
    return "keysym(\"" + std::string(name) + "\")";
  }
  if (auto code = std::get_if<ConfigKeycode>(&value))
    return "keycode(" + std::to_string(code->value) + ")";
  if (auto list = std::get_if<ConfigCommands>(&value)) {
    std::string result = "commands(";
    for (const auto &s : list->values) {
      if (result.size() > 9)
        result += ", ";
      result += '"' + s + '"';
    }
    return result + ")";
  }
  return scalar_text(value);
}
bool ConfigBinding::matches(uint32_t keysym, uint32_t keycode,
                            uint32_t mods) const {
  if ((mods & binding_modifiers) != modifiers)
    return false;
  if (auto sym = std::get_if<ConfigKeysym>(&key))
    return sym->value == keysym;
  return std::get<ConfigKeycode>(key).value == keycode;
}
