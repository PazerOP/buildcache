//--------------------------------------------------------------------------------------------------
// Copyright (c) 2018 Marcus Geelnard
//
// This software is provided 'as-is', without any express or implied warranty. In no event will the
// authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose, including commercial
// applications, and to alter it and redistribute it freely, subject to the following restrictions:
//
//  1. The origin of this software must not be misrepresented; you must not claim that you wrote
//     the original software. If you use this software in a product, an acknowledgment in the
//     product documentation would be appreciated but is not required.
//
//  2. Altered source versions must be plainly marked as such, and must not be misrepresented as
//     being the original software.
//
//  3. This notice may not be removed or altered from any source distribution.
//--------------------------------------------------------------------------------------------------

#include <base/string_list.hpp>

#include <base/unicode_utils.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// Include order matters.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

namespace bcache {

string_list_t::string_list_t(const std::string& str, const std::string& delimiter) {
  std::string::size_type current_str_start = 0U;
  std::string substr;
  while (current_str_start < str.size()) {
    const auto pos = str.find(delimiter, current_str_start);
    if (pos == std::string::npos) {
      substr = str.substr(current_str_start);
      current_str_start = str.size();
    } else {
      substr = str.substr(current_str_start, pos - current_str_start);
      current_str_start = pos + 1;
    }

    if (!substr.empty()) {
      m_strings.emplace_back(std::move(substr));
    }
  }
}

string_list_t string_list_t::split_args(const std::string& cmd) {
  string_list_t args;

#ifdef _WIN32
  std::wstring wcmd = utf8_to_ucs2(cmd);
  int argc;
  wchar_t** argv = CommandLineToArgvW(wcmd.data(), &argc);
  for (int i = 0; i < argc; ++i) {
    args += ucs2_to_utf8(argv[i]);
  }

#else
  std::string arg;
  auto is_inside_quote = false;
  auto has_arg = false;
  char last_char = 0;
  for (const auto& chr : cmd) {
    const auto is_space = (chr == ' ');
    const auto is_quote = (chr == '\"') && (last_char != '\\');

    if (is_quote) {
      is_inside_quote = !is_inside_quote;
    }

    // Start of a new argument?
    if ((!has_arg) && (!is_space)) {
      has_arg = true;
    }

    // Append this char to the argument string?
    if ((is_inside_quote || !is_space) && !is_quote) {
      arg += chr;
    }

    // End of argument?
    if (has_arg && is_space && !is_inside_quote) {
      args += unescape_arg(arg);
      arg.clear();
      has_arg = false;
    }

    last_char = chr;
  }

  if (has_arg) {
    args += unescape_arg(arg);
  }

#endif
  return args;
}

std::string string_list_t::join(const std::string& separator, const bool escape) const {
  std::string result;
  for (const auto& arg : m_strings) {
    const auto escaped_arg = escape ? escape_arg(arg) : arg;
    if (result.empty()) {
      result += escaped_arg;
    } else {
      result += (separator + escaped_arg);
    }
  }
  return result;
}

inline std::string bcache::string_list_t::escape_arg(const std::string& arg) {
  std::string escaped_arg;
  bool needs_quotes = false;

#ifdef _WIN32
  // These escaping rules try to match parsing rules of Windows programs, e.g. as outlined here:
  // http://www.windowsinspired.com/how-a-windows-programs-splits-its-command-line-into-individual-arguments/
  // "[..] backslashes are interpreted literally (except when they precede a double quote
  // character)".
  int backslashes = 0;
  for (auto c : arg) {
    if (c == '\\') {
      ++backslashes;
    } else {
      if (c == '\"') {
        for (int i = 0; i <= backslashes; ++i) {
          escaped_arg += '\\';
        }
      }
      backslashes = 0;
    }

    escaped_arg += c;

    if (c == ' ' || c == '\t') {
      needs_quotes = true;
    }
  }

  if (needs_quotes) {
    for (int i = 0; i < backslashes; ++i) {
      escaped_arg += '\\';
    }
  }

#else
  // These escaping rules try to match the most common Un*x shell conventions, e.g. as outlined
  // here: http://faculty.salina.k-state.edu/tim/unix_sg/shell/metachar.html
  for (auto c : arg) {
    if (c == '"') {
      escaped_arg += "\\\"";
    } else if (c == '\\') {
      escaped_arg += "\\\\";
    } else if (c == '$') {
      escaped_arg += "\\$";
      needs_quotes = true;
    } else if (c == '`') {
      escaped_arg += "\\`";
      needs_quotes = true;
    } else {
      if (c == ' ' || c == '&' || c == ';' || c == '>' || c == '<' || c == '|' || c == '(' ||
          c == ')' || c == '*' || c == '#') {
        needs_quotes = true;
      }
      escaped_arg += c;
    }
  }
#endif

  // Do we need to surround with quotes?
  if (needs_quotes) {
    escaped_arg = std::string("\"") + escaped_arg + std::string("\"");
  }

  return escaped_arg;
}

inline std::string bcache::string_list_t::unescape_arg(const std::string& arg) {
  std::string unescaped_arg;

  auto is_escaped = false;
  for (auto c : arg) {
    if ((c == '\\') && !is_escaped) {
      is_escaped = true;
    } else {
      // TODO(m): We should handle different escape codes. The current solution at least works for
      // converting \\ -> \ and \" -> ".
      unescaped_arg += c;
      is_escaped = false;
    }
  }

  return unescaped_arg;
}

}  // namespace bcache
