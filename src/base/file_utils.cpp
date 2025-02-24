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

#include <base/debug_utils.hpp>
#include <base/env_utils.hpp>
#include <base/file_utils.hpp>
#include <base/string_list.hpp>
#include <base/unicode_utils.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <shlobj.h>
#include <userenv.h>
#include <windows.h>
#undef ERROR
#undef log
#else
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>

// S_ISDIR/S_ISREG are not defined by MSVC, but _S_IFDIR/_S_IFREG are.
#if defined(_WIN32) && !defined(S_ISDIR)
#define S_ISDIR(x) (((x)&_S_IFDIR) != 0)
#endif
#if defined(_WIN32) && !defined(S_ISREG)
#define S_ISREG(x) (((x)&_S_IFREG) != 0)
#endif

namespace bcache {
namespace file {
namespace {
// Directory separator for paths.
#ifdef _WIN32
const char PATH_SEPARATOR_CHR = '\\';
#else
const char PATH_SEPARATOR_CHR = '/';
#endif
const auto PATH_SEPARATOR = std::string(1, PATH_SEPARATOR_CHR);

// Delimiter character for the PATH environment variable.
#ifdef _WIN32
const char PATH_DELIMITER_CHR = ';';
#else
const char PATH_DELIMITER_CHR = ':';
#endif
const auto PATH_DELIMITER = std::string(1, PATH_DELIMITER_CHR);

// This is a static variable that holds a strictly incrementing number used for generating unique
// temporary file names.
std::atomic_uint_fast32_t s_tmp_name_number;

int get_process_id() {
#ifdef _WIN32
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}

std::string::size_type get_last_path_separator_pos(const std::string& path) {
#if defined(_WIN32)
  const auto pos1 = path.rfind('/');
  const auto pos2 = path.rfind('\\');
  std::string::size_type pos;
  if (pos1 == std::string::npos) {
    pos = pos2;
  } else if (pos2 == std::string::npos) {
    pos = pos1;
  } else {
    pos = std::max(pos1, pos2);
  }
#else
  const auto pos = path.rfind(PATH_SEPARATOR_CHR);
#endif
  return pos;
}

#ifdef _WIN32
int64_t two_dwords_to_int64(const DWORD low, const DWORD high) {
  return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(low)) |
                              (static_cast<uint64_t>(static_cast<uint32_t>(high)) << 32));
}
#endif

void remove_dir_internal(const std::string& path, const bool ignore_errors) {
#ifdef _WIN32
  const auto success = (_wrmdir(utf8_to_ucs2(path).c_str()) == 0);
#else
  const auto success = (rmdir(path.c_str()) == 0);
#endif
  if ((!success) && (!ignore_errors)) {
    throw std::runtime_error("Unable to remove dir.");
  }
}

/// @brief Get a number based on a high resolution timer.
/// @note The time unit is unspecified, and taking the difference between two consecutive return
/// values is not supported as the time scale may be non-continuous.
uint64_t get_hires_time() {
#if defined(_WIN32)
  LARGE_INTEGER count;
  QueryPerformanceCounter(&count);
  return static_cast<uint64_t>(count.QuadPart);
#else
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (static_cast<uint64_t>(tv.tv_sec) << 20) | static_cast<uint64_t>(tv.tv_usec);
#endif
}

/// @brief Convert an integer to a human-readable string.
/// @param x The integer to convert.
/// @returns a string that consists of alphanumerical characters. The string is at least one
/// character long, and at most 13 characters long.
std::string to_id_part(const uint64_t x) {
  static const char CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  static const auto NUM_CHARS = static_cast<uint64_t>(sizeof(CHARS) / sizeof(CHARS[0]) - 1);

  std::string part;
  if (x == 0U) {
    part += 'u';
  } else {
    auto q = x;
    while (q != 0U) {
      part += CHARS[q % NUM_CHARS];
      q = q / NUM_CHARS;
    }
  }

  return part;
}

/// @brief Get implicit file extensions for executable files.
///
/// The list is on the form ["", ".foo", ".bar", ...]. The first item is an empty string
/// (representing "no extra extension"), and the list is guaranteed to contain at least one item.
///
/// @returns a list of valid extensions.
string_list_t get_exe_extensions() {
#if defined(_WIN32)
  // Use PATHEXT to determine valid executable file extensions. For more info, see:
  // https://docs.microsoft.com/en-us/windows-server/administration/windows-commands/start
  std::string path_ext_str = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC";
  const env_var_t path_ext_env("PATHEXT");
  if (path_ext_env) {
    path_ext_str = path_ext_env.as_string();
  }

  // Note: We use lower case since we want to do case insensitive string compares.
  return string_list_t({""}) + string_list_t(lower_case(path_ext_str), ";");
#else
  // On POSIX systems, there is no such thing as an exe extension that gets implicitly added when
  // invoking a command.
  return string_list_t({""});
#endif
}

bool is_absolute_path(const std::string& path) {
#ifdef _WIN32
  const bool is_abs_drive =
      (path.size() >= 3) && (path[1] == ':') && ((path[2] == '\\') || (path[2] == '/'));
  const bool is_abs_net = (path.size() >= 2) && (path[0] == '\\') && (path[1] == '\\');
  return is_abs_drive || is_abs_net;
#else
  return (!path.empty()) && (path[0] == PATH_SEPARATOR_CHR);
#endif
}

bool is_relateive_path(const std::string& path) {
  return get_last_path_separator_pos(path) != std::string::npos;
}

}  // namespace

tmp_file_t::tmp_file_t(const std::string& dir, const std::string& extension) {
  // Generate a file name based on a unique identifier.
  const auto file_name = std::string("bcache-") + get_unique_id();

  // Concatenate base dir, file name and extension into the full path.
  m_path = append_path(dir, file_name + extension);
}

tmp_file_t::~tmp_file_t() {
  try {
    if (file_exists(m_path)) {
      remove_file(m_path);
    } else if (dir_exists(m_path)) {
      remove_dir(m_path);
    }
  } catch (const std::exception& e) {
    debug::log(debug::ERROR) << e.what();
  }
}

scoped_work_dir_t::scoped_work_dir_t(const std::string& new_work_dir) {
  if (!new_work_dir.empty()) {
    m_old_work_dir = get_cwd();
    set_cwd(new_work_dir);
  }
}

scoped_work_dir_t::~scoped_work_dir_t() {
  if (!m_old_work_dir.empty()) {
    set_cwd(m_old_work_dir);
  }
}

bool filter_t::keep(const std::string& file_name) const {
  // Shall we keep all files?
  if (m_include == include_t::ALL) {
    return true;
  }

  // Perform string matching.
  bool match = false;
  switch (m_match) {
    case match_t::SUBSTRING:
      match = (file_name.find(m_string) != std::string::npos);
      break;
    case match_t::EXTENSION:
      if (file_name.size() >= m_string.size()) {
        match = std::equal(m_string.cbegin(), m_string.cend(), file_name.cend() - m_string.size());
      }
      break;
    default:
      throw std::runtime_error("Invalid filter_t match method.");
  }

  // Final include/exclude logic.
  return ((m_include == include_t::INCLUDE) && match) ||
         ((m_include == include_t::EXCLUDE) && !match);
}

std::string append_path(const std::string& path, const std::string& append) {
  if (path.empty() || append.empty() || path.back() == PATH_SEPARATOR_CHR) {
    return path + append;
  }
  return path + PATH_SEPARATOR + append;
}

std::string append_path(const std::string& path, const char* append) {
  return append_path(path, std::string(append));
}

std::string canonicalize_path(const std::string& path) {
#ifdef _WIN32
  std::string result;

  // Use a Win32 API function to resolve as much as possible. Unfortunately there does not seem to
  // be a single Win32 API function that can give sane results (hence the pre/post processing).
  {
    const auto ucs2 = utf8_to_ucs2(path);
    wchar_t buf[MAX_PATH + 1];
    const auto buf_size = static_cast<DWORD>(std::extent<decltype(buf)>::value);
    const auto full_path_size = GetFullPathNameW(ucs2.c_str(), buf_size, &buf[0], nullptr);
    if (full_path_size > 0 && full_path_size < buf_size) {
      result = ucs2_to_utf8(buf, buf + full_path_size);
    } else if (full_path_size >= buf_size) {
      // Buffer is too small, dynamically allocate bigger buffer.
      std::wstring final_path(full_path_size - 1, 0);  // terminating null ch is added automatically
      GetFullPathNameW(ucs2.c_str(), full_path_size, &final_path[0], nullptr);
      result = ucs2_to_utf8(final_path);
    } else {
      throw std::runtime_error("Unable to canonicalize the path " + result);
    }
  }

  if (!result.empty()) {
    // Drop trailing back slash.
    const auto last = result.size() - 1;
    if (last >= 2U && result[last] == '\\' && result[last - 1] != ':') {
      result.pop_back();
    }
    // Convert drive letters to uppercase.
    if (result.size() >= 2U && result[1] == ':') {
      result[0] = static_cast<char>(upper_case(static_cast<int>(result[0])));
    }
  }

#else
  std::string result = path;

  // Resolve relative paths.
  if (!is_absolute_path(result)) {
    result = append_path(get_cwd(), result);
  }

  // Simplify "//" and "/./" etc into "/", and resolve "..".
  string_list_t parts(result, PATH_SEPARATOR);
  string_list_t filtered_parts;
  for (const auto& part : parts) {
    if (part == "..") {
      if (filtered_parts.size() < 1U) {
        throw std::runtime_error("Unable to canonicalize the path " + path);
      }
      filtered_parts.pop_back();
    } else if (!part.empty() && part != ".") {
      filtered_parts += part;
    }
  }
  result = PATH_SEPARATOR + filtered_parts.join(PATH_SEPARATOR);
#endif
  return result;
}

std::string get_extension(const std::string& path) {
  const auto pos = path.rfind('.');

  // Check that we did not pick up an extension before the last path separator.
  const auto sep_pos = get_last_path_separator_pos(path);
  if ((pos != std::string::npos) && (sep_pos != std::string::npos) && (pos < sep_pos)) {
    return std::string();
  }

  return (pos != std::string::npos) ? path.substr(pos) : std::string();
}

std::string change_extension(const std::string& path, const std::string& new_ext) {
  const auto pos = path.rfind('.');

  // Check that we did not pick up an extension before the last path separator.
  const auto sep_pos = get_last_path_separator_pos(path);
  if ((pos != std::string::npos) && (sep_pos != std::string::npos) && (pos < sep_pos)) {
    return path;
  }

  return (pos != std::string::npos) ? (path.substr(0, pos) + new_ext) : path;
}

std::string get_file_part(const std::string& path, const bool include_ext) {
  const auto pos = get_last_path_separator_pos(path);
  const auto file_name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
  const auto ext_pos = file_name.rfind('.');
  return (include_ext || (ext_pos == std::string::npos) || (ext_pos == 0))
             ? file_name
             : file_name.substr(0, ext_pos);
}

std::string get_dir_part(const std::string& path) {
  const auto pos = get_last_path_separator_pos(path);
  return (pos != std::string::npos) ? path.substr(0, pos) : std::string();
}

std::string get_temp_dir() {
#if defined(_WIN32)
  WCHAR buf_arr[MAX_PATH + 1];
  const DWORD buf_arr_size = std::extent<decltype(buf_arr)>::value;
  const DWORD path_len = GetTempPathW(buf_arr_size, buf_arr);
  if (path_len > 0) {
    if (path_len < buf_arr_size) {
      return canonicalize_path(ucs2_to_utf8(buf_arr, buf_arr + path_len));
    }
    // Else buffer isn't enough to fit the path, dynamically allocate bigger buffer.
    std::wstring buf_str(path_len - 1, 0);  // terminating null character is added automatically
    GetTempPathW(path_len, &buf_str[0]);
    return canonicalize_path(ucs2_to_utf8(buf_str));
  }
  return std::string();
#else
  // 1. Try $XDG_RUNTIME_DIR. See:
  //    https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
  env_var_t xdg_runtime_dir("XDG_RUNTIME_DIR");
  if (xdg_runtime_dir && dir_exists(xdg_runtime_dir.as_string())) {
    return canonicalize_path(xdg_runtime_dir.as_string());
  }

  // 2. Try $TMPDIR. See:
  //    https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html#tag_08_03
  env_var_t tmpdir("TMPDIR");
  if (tmpdir && dir_exists(tmpdir.as_string())) {
    return canonicalize_path(tmpdir.as_string());
  }

  // 3. Fall back to /tmp. See:
  //    http://refspecs.linuxfoundation.org/FHS_3.0/fhs/ch03s18.html
  return std::string("/tmp");
#endif
}

std::string get_user_home_dir() {
#if defined(_WIN32)
#if 0
  // TODO(m): We should use SHGetKnownFolderPath() for Vista and later, but this fails to build in
  // older MinGW so we skip it a.t.m.
  std::string local_app_data;
  PWSTR path = nullptr;
  try {
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path))) {
      local_app_data = ucs2_to_utf8(std::wstring(path));
    }
  } finally {
    if (path != nullptr) {
      CoTaskMemFree(path);
    }
  }
  return local_app_data;
#else
  std::string user_home;
  HANDLE token = nullptr;
  if (SUCCEEDED(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))) {
    // For the most cases MAX_PATH + 1 is enough.
    WCHAR buf_array[MAX_PATH + 1];
    DWORD buf_size = std::extent<decltype(buf_array)>::value;
    if (SUCCEEDED(GetUserProfileDirectoryW(token, buf_array, &buf_size))) {
      user_home = ucs2_to_utf8(buf_array, buf_array + buf_size - 1);  // minus terminating null char
    } else {
      // Array is too small, allocate bigger buffer
      std::wstring buf_str(buf_size - 1, 0);  // terminating null character is added automatically
      if (SUCCEEDED(GetUserProfileDirectoryW(token, &buf_str[0], &buf_size))) {
        user_home = ucs2_to_utf8(buf_str);
      }
    }

    CloseHandle(token);
  }
  return user_home;
#endif
#else
  return get_env("HOME");
#endif
}

std::string get_cwd() {
#if defined(_WIN32)
  WCHAR buf[MAX_PATH + 1] = {0};
  DWORD path_len = GetCurrentDirectoryW(MAX_PATH + 1, buf);
  if (path_len > 0) {
    return ucs2_to_utf8(std::wstring(buf, path_len));
  }
#else
  size_t size = 512;
  for (; size <= 65536U; size *= 2) {
    std::vector<char> buf(size);
    auto* ptr = ::getcwd(buf.data(), size);
    if (ptr != nullptr) {
      return std::string(ptr);
    }
  }
#endif
  throw std::runtime_error("Unable to determine the current working directory.");
}

void set_cwd(const std::string& path) {
#if defined(_WIN32)
  const auto success = (SetCurrentDirectoryW(utf8_to_ucs2(path).c_str()) != 0);
#else
  const auto success = (chdir(path.c_str()) == 0);
#endif
  if (!success) {
    throw std::runtime_error("Could not change the current working directory to " + path);
  }
}

std::string resolve_path(const std::string& path) {
#if defined(_WIN32)
  auto* handle = CreateFileW(utf8_to_ucs2(path).c_str(),
                             0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
  if (INVALID_HANDLE_VALUE != handle) {
    std::wstring resolved_path;
    WCHAR buf_arr[MAX_PATH + 1];
    const DWORD buf_size = std::extent<decltype(buf_arr)>::value;
    auto resolved_size = GetFinalPathNameByHandleW(handle, buf_arr, buf_size, FILE_NAME_NORMALIZED);
    if (resolved_size < buf_size) {
      // In this case the resolved_size doesn't include terminating null character.
      resolved_path.assign(buf_arr, resolved_size);
    } else {
      // Array is too small to fit the path, allocate buffer dynamically to fit the path.
      resolved_path.resize(resolved_size - 1);  // terminating null character is added automatically
      GetFinalPathNameByHandleW(handle, &resolved_path[0], resolved_size, FILE_NAME_NORMALIZED);
    }
    CloseHandle(handle);
    const std::wstring prefix(LR"(\\?\)");
    const bool may_has_prefix = resolved_path.size() >= prefix.size();
    if (may_has_prefix && resolved_path.compare(0, prefix.size(), prefix) == 0) {
      resolved_path = resolved_path.substr(prefix.size());
    }
    return ucs2_to_utf8(resolved_path);
  }
  return std::string();
#else
  auto* char_ptr = realpath(path.c_str(), nullptr);
  if (char_ptr != nullptr) {
    auto result = std::string(char_ptr);
    std::free(char_ptr);
    return result;
  }
  return std::string();
#endif
}

exe_path_t find_executable(const std::string& program, const std::string& exclude) {
  const auto extensions = get_exe_extensions();

  std::string file_to_find;

  // Handle absolute and relative paths. Examples:
  //  - "C:\Foo\foo.exe"
  //  - "somedir/../mysubdir/foo"
  if (is_absolute_path(program) || is_relateive_path(program)) {
    for (const auto& ext : extensions) {
      auto path_with_ext = program + ext;

      // Return the full path unless it points to the excluded executable.
      auto true_path = resolve_path(path_with_ext);
      if (true_path.empty()) {
        // Unable to resolve. Try next ext.
        continue;
      }
      if (lower_case(get_file_part(true_path, false)) != exclude) {
        const auto& virtual_path = canonicalize_path(path_with_ext);
        debug::log(debug::DEBUG) << "Found exe: " << true_path << " (" << program << ", "
                                 << virtual_path << ")";
        return exe_path_t(true_path, virtual_path, program);
      }

      // ...otherwise search for the named file (which should be a symlink) in the PATH.
      // This handles invokations of programs via symbolic links to the buildcache executable.
      file_to_find = get_file_part(path_with_ext);
      break;
    }
  } else {
    // The path is just a file name without a path.
    file_to_find = program;
  }

  if (!file_to_find.empty()) {
    // Get the PATH environment variable.
    string_list_t search_path;
#if defined(_WIN32)
    {
      // For Windows we prepend the current working directory to the PATH, as Windows searches the
      // CWD before searching the PATH.
      const auto cwd = get_cwd();
      if (!cwd.empty()) {
        search_path += cwd;
      }
    }
#endif
    search_path += string_list_t(get_env("PATH"), PATH_DELIMITER);

    // Iterate the path from start to end and see if we can find the executable file.
    for (const auto& base_path : search_path) {
      for (const auto& ext : extensions) {
        const auto file_name = file_to_find + ext;
        auto virtual_path = append_path(base_path, file_name);
        auto true_path = resolve_path(virtual_path);
        if ((!true_path.empty()) && file_exists(true_path)) {
          // Check that this is not the excluded file name.
          if (lower_case(get_file_part(true_path, false)) != exclude) {
            debug::log(debug::DEBUG)
                << "Found exe: " << true_path << " (" << program << ", " << virtual_path << ")";
            return exe_path_t(true_path, virtual_path, program);
          }
        }
      }
    }
  }

  throw std::runtime_error("Could not find the executable file.");
}

void create_dir(const std::string& path) {
#ifdef _WIN32
  const auto success = (_wmkdir(utf8_to_ucs2(path).c_str()) == 0);
#else
  const auto success = (mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == 0);
#endif
  if (!success) {
    throw std::runtime_error("Unable to create directory " + path);
  }
}

void create_dir_with_parents(const std::string& path) {
  // Recursively create parent directories if necessary.
  const auto parent = get_dir_part(path);
  if (parent.size() < path.size() && !parent.empty() && !dir_exists(parent)) {
    create_dir_with_parents(parent);
  }

  // Create the requested directory unless it already exists.
  if (!path.empty() && !dir_exists(path)) {
    create_dir(path);
  }
}

void remove_file(const std::string& path, const bool ignore_errors) {
#ifdef _WIN32
  const auto success = (_wunlink(utf8_to_ucs2(path).c_str()) == 0);
#else
  const auto success = (unlink(path.c_str()) == 0);
#endif
  if ((!success) && (!ignore_errors)) {
    throw std::runtime_error("Unable to remove file.");
  }
}

void remove_dir(const std::string& path, const bool ignore_errors) {
  const auto files = walk_directory(path);
  for (const auto& file : files) {
    if (file.is_dir()) {
      remove_dir_internal(file.path(), ignore_errors);
    } else {
      remove_file(file.path(), ignore_errors);
    }
  }
  remove_dir_internal(path, ignore_errors);
}

bool dir_exists(const std::string& path) {
#ifdef _WIN32
  // Do a quick check if this is a drive letter and assume it exists. For performance reasons
  // do not invoke Win32 APIs to verify the volume etc.
  if (path.size() == 2 && path[1] == ':') {
    return true;
  }

  struct __stat64 buffer;
  const auto success = (_wstat64(utf8_to_ucs2(path).c_str(), &buffer) == 0);
  return success && S_ISDIR(buffer.st_mode);
#else
  struct stat buffer;
  const auto success = (stat(path.c_str(), &buffer) == 0);
  return success && S_ISDIR(buffer.st_mode);
#endif
}

bool file_exists(const std::string& path) {
#ifdef _WIN32
  struct __stat64 buffer;
  const auto success = (_wstat64(utf8_to_ucs2(path).c_str(), &buffer) == 0);
  return success && S_ISREG(buffer.st_mode);
#else
  struct stat buffer;
  const auto success = (stat(path.c_str(), &buffer) == 0);
  return success && S_ISREG(buffer.st_mode);
#endif
}

void move(const std::string& from_path, const std::string& to_path) {
  // First remove the old target file, if any (otherwise the rename will fail).
  if (file_exists(to_path)) {
    remove_file(to_path);
  }

  // Rename the file.
#ifdef _WIN32
  const auto success =
      (_wrename(utf8_to_ucs2(from_path).c_str(), utf8_to_ucs2(to_path).c_str()) == 0);
#else
  const auto success = (std::rename(from_path.c_str(), to_path.c_str()) == 0);
#endif

  if (!success) {
    throw std::runtime_error("Unable to move file.");
  }
}

void copy(const std::string& from_path, const std::string& to_path) {
  // Copy to a temporary file first and once the copy has succeeded rename it to the target file.
  // This should prevent half-finished copies if the process is terminated prematurely (e.g.
  // CTRL+C).
  const auto base_path = get_dir_part(to_path);
  auto tmp_file = tmp_file_t(base_path, ".tmp");

#ifdef _WIN32
  // TODO(m): We could handle paths longer than MAX_PATH, e.g. by prepending strings with "\\?\"?
  bool success =
      (CopyFileW(utf8_to_ucs2(from_path).c_str(), utf8_to_ucs2(tmp_file.path()).c_str(), FALSE) !=
       0);
#else
  // For non-Windows systems we use a classic buffered read-write loop.
  bool success = false;
  auto* from_file = std::fopen(from_path.c_str(), "rb");
  if (from_file != nullptr) {
    auto* to_file = std::fopen(tmp_file.path().c_str(), "wb");
    if (to_file != nullptr) {
      // We use a buffer size that typically fits in an L1 cache.
      static const int BUFFER_SIZE = 8192;
      std::vector<std::uint8_t> buf(BUFFER_SIZE);
      success = true;
      while (std::feof(from_file) == 0) {
        const auto bytes_read = std::fread(buf.data(), 1, buf.size(), from_file);
        if (bytes_read == 0U) {
          break;
        }
        const auto bytes_written = std::fwrite(buf.data(), 1, bytes_read, to_file);
        if (bytes_written != bytes_read) {
          success = false;
          break;
        }
      }
      std::fclose(to_file);
    }
    std::fclose(from_file);
  }
#endif

  if (!success) {
    // Note: At this point the temporary file (if any) will be deleted.
    throw std::runtime_error("Unable to copy file.");
  }

  // Move the temporary file to its target name.
  move(tmp_file.path(), to_path);
}

void link_or_copy(const std::string& from_path, const std::string& to_path) {
  // First remove the old file, if any (otherwise the hard link will fail).
  if (file_exists(to_path)) {
    remove_file(to_path);
  }

  // First try to make a hard link. However this may fail if the files are on different volumes for
  // instance.
  bool success;
#ifdef _WIN32
  success = (CreateHardLinkW(
                 utf8_to_ucs2(to_path).c_str(), utf8_to_ucs2(from_path).c_str(), nullptr) != 0);
#else
  success = (link(from_path.c_str(), to_path.c_str()) == 0);
#endif

  // If the hard link failed, make a full copy instead.
  if (!success) {
    debug::log(debug::DEBUG) << "Hard link failed - copying instead.";
    copy(from_path, to_path);
  }
}

void touch(const std::string& path) {
#ifdef _WIN32
  bool success = false;
  HANDLE h = CreateFileW(utf8_to_ucs2(path).c_str(),
                         FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_WRITE,
                         nullptr,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS,
                         nullptr);
  if (h != nullptr) {
    FILETIME mtime;
    GetSystemTimeAsFileTime(&mtime);
    success = (SetFileTime(h, nullptr, nullptr, &mtime) != FALSE);
    CloseHandle(h);
  }
#else
  bool success = (utime(path.c_str(), nullptr) == 0);
#endif
  if (!success) {
    throw std::runtime_error("Unable to touch the file.");
  }
}

std::string read(const std::string& path) {
  FILE* f;

// Open the file.
#ifdef _WIN32
  const auto err = _wfopen_s(&f, utf8_to_ucs2(path).c_str(), L"rb");
  if (err != 0) {
    std::string errMsg = "Unable to open the file (";
    errMsg += path;
    errMsg += "): ";
    errMsg += std::to_string(err);
    errMsg += ": ";
    
    char buf[1024];
    strerror_s(buf, err);
    buf[sizeof(buf) - 1] = 0;
    errMsg += buf;

    throw std::runtime_error(errMsg);
  }
#else
  f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    throw std::runtime_error("Unable to open the file.");
  }
#endif

  // Get file size.
  std::fseek(f, 0, SEEK_END);
  const auto file_size = static_cast<size_t>(std::ftell(f));
  std::fseek(f, 0, SEEK_SET);

  // Read the data into a string.
  std::string str;
  str.resize(static_cast<std::string::size_type>(file_size));
  auto bytes_left = file_size;
  while ((bytes_left != 0U) && (std::feof(f) == 0)) {
    auto* ptr = &str[file_size - bytes_left];
    const auto bytes_read = std::fread(ptr, 1, bytes_left, f);
    bytes_left -= bytes_read;
  }

  // Close the file.
  std::fclose(f);

  if (bytes_left != 0U) {
    throw std::runtime_error("Unable to read the file.");
  }

  return str;
}

void write(const std::string& data, const std::string& path) {
  FILE* f;

// Open the file.
#ifdef _WIN32
  const auto err = _wfopen_s(&f, utf8_to_ucs2(path).c_str(), L"wb");
  if (err != 0) {
    throw std::runtime_error("Unable to open the file.");
  }
#else
  f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    throw std::runtime_error("Unable to open the file.");
  }
#endif

  // Write the data to the file.
  const auto file_size = data.size();
  auto bytes_left = file_size;
  while ((bytes_left != 0U) && (std::ferror(f) == 0)) {
    const auto* ptr = &data[file_size - bytes_left];
    const auto bytes_written = std::fwrite(ptr, 1, bytes_left, f);
    bytes_left -= bytes_written;
  }

  // Close the file.
  std::fclose(f);

  if (bytes_left != 0U) {
    throw std::runtime_error("Unable to write the file.");
  }
}

void write_atomic(const std::string& data, const std::string& path) {
  // 1) Write to a temporary file.
  const auto base_path = get_dir_part(path);
  auto tmp_file = tmp_file_t(base_path, ".tmp");
  write(data, tmp_file.path());

  // 2) Remove the target path if it already exists.
  remove_file(path, true);

  // 3) Move the temporary file to the target file name.
  move(tmp_file.path(), path);
}

void append(const std::string& data, const std::string& path) {
  if (path.empty()) {
    throw std::runtime_error("No file path given.");
  }

#ifdef _WIN32
  // Open the file.
  auto* handle = CreateFileW(utf8_to_ucs2(path).c_str(),
                             FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr,
                             OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Unable to open the file.");
  }

  // Move to the end of the file.
  const auto moved = SetFilePointer(handle, 0, nullptr, FILE_END);
  if (moved == INVALID_SET_FILE_POINTER) {
    CloseHandle(handle);
    throw std::runtime_error("Unable to set file pointer to end-of-file.");
  }

  // Write the data.
  const auto bytes_to_write = static_cast<DWORD>(data.size());
  DWORD bytes_written;
  const auto success =
      (WriteFile(handle, data.c_str(), bytes_to_write, &bytes_written, nullptr) != FALSE);
  if (!success || bytes_written != bytes_to_write) {
    CloseHandle(handle);
    throw std::runtime_error("Unable to write to the file.");
  }

  // Close the file.
  CloseHandle(handle);
#else
  // Open the file (write pointer is at the end of the file).
  auto* f = std::fopen(path.c_str(), "ab");
  if (f == nullptr) {
    throw std::runtime_error("Unable to open the file.");
  }

  // Write the data to the file.
  const auto file_size = data.size();
  auto bytes_left = file_size;
  while ((bytes_left != 0U) && (std::ferror(f) == 0)) {
    const auto* ptr = &data[file_size - bytes_left];
    const auto bytes_written = std::fwrite(ptr, 1, bytes_left, f);
    bytes_left -= bytes_written;
  }

  // Close the file.
  std::fclose(f);

  if (bytes_left != 0U) {
    throw std::runtime_error("Unable to write the file.");
  }
#endif
}

file_info_t get_file_info(const std::string& path) {
  // TODO(m): This is pretty much copy-paste from walk_directory(). Refactor.
  const std::string absPath = std::filesystem::absolute(path).string();

#ifdef _WIN32
  WIN32_FIND_DATAW find_data;
  auto* find_handle = FindFirstFileW(utf8_to_ucs2(absPath).c_str(), &find_data);
  if (find_handle != INVALID_HANDLE_VALUE) {
    const auto name = ucs2_to_utf8(std::wstring(&find_data.cFileName[0]));
    const auto file_path = append_path(path, name);
    time::seconds_t modify_time = 0;
    time::seconds_t access_time = 0;
    int64_t size = 0;
    bool is_dir = ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    if (!is_dir) {
      size = two_dwords_to_int64(find_data.nFileSizeLow, find_data.nFileSizeHigh);
      modify_time = time::win32_filetime_to_unix_epoch(find_data.ftLastWriteTime.dwLowDateTime,
                                                       find_data.ftLastWriteTime.dwHighDateTime);
      access_time = time::win32_filetime_to_unix_epoch(find_data.ftLastAccessTime.dwLowDateTime,
                                                       find_data.ftLastAccessTime.dwHighDateTime);
    }
    // Note: We pass inode = 0, since inode numbers are not supported on Windows (AFAIK).
    const uint64_t inode = 0U;
    return file_info_t(file_path, modify_time, access_time, size, inode, is_dir);
  }
  //const std::filesystem::path fsPath = path;
  //const auto absPath = std::filesystem::absolute(fsPath);

  //const auto size = std::filesystem::file_size(fsPath);
  //const auto modifyTime = std::filesystem::last_write_time(fsPath)

  //  return file_info_t(absPath.string(), ;

  //  std::filesystem::file_size()

#else
  struct stat file_stat;
  const bool stat_ok = (stat(path.c_str(), &file_stat) == 0);
  if (stat_ok) {
    time::seconds_t modify_time = 0;
    time::seconds_t access_time = 0;
    int64_t size = 0;
    const bool is_dir = S_ISDIR(file_stat.st_mode);
    const bool is_file = S_ISREG(file_stat.st_mode);
    if (is_file) {
      size = static_cast<int64_t>(file_stat.st_size);
#ifdef __APPLE__
      modify_time = static_cast<time::seconds_t>(file_stat.st_mtimespec.tv_sec);
      access_time = static_cast<time::seconds_t>(file_stat.st_atimespec.tv_sec);
#else
      modify_time = static_cast<time::seconds_t>(file_stat.st_mtim.tv_sec);
      access_time = static_cast<time::seconds_t>(file_stat.st_atim.tv_sec);
#endif
    }
    static_assert(sizeof(file_stat.st_ino) <= sizeof(uint64_t), "Unsupported st_ino size");
    const auto inode = static_cast<uint64_t>(file_stat.st_ino);
    return file_info_t(path, modify_time, access_time, size, inode, is_dir);
  }
#endif

  throw std::runtime_error(std::string("Unable to get file information from \"") + absPath + "\".");
}

std::string human_readable_size(const int64_t byte_size) {
  static const char* SUFFIX[6] = {"bytes", "KiB", "MiB", "GiB", "TiB", "PiB"};
  static const int MAX_SUFFIX_IDX = (sizeof(SUFFIX) / sizeof(SUFFIX[0])) - 1;

  auto scaled_size = static_cast<double>(byte_size);
  int suffix_idx = 0;
  for (; scaled_size >= 1024.0 && suffix_idx < MAX_SUFFIX_IDX; ++suffix_idx) {
    scaled_size /= 1024.0;
  }

  char buf[20];
  if (suffix_idx >= 1) {
    std::snprintf(buf, sizeof(buf), "%.1f %s", scaled_size, SUFFIX[suffix_idx]);
  } else {
    std::snprintf(buf, sizeof(buf), "%d %s", static_cast<int>(byte_size), SUFFIX[suffix_idx]);
  }
  return std::string(buf);
}

std::vector<file_info_t> walk_directory(const std::string& path, const filter_t& filter) {
  std::vector<file_info_t> files;

#ifdef _WIN32
  const auto search_str = utf8_to_ucs2(append_path(path, "*"));
  WIN32_FIND_DATAW find_data;
  auto* find_handle = FindFirstFileW(search_str.c_str(), &find_data);
  if (find_handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Unable to walk the directory.");
  }
  do {
    const auto name = ucs2_to_utf8(std::wstring(&find_data.cFileName[0]));
    if ((name != ".") && (name != "..") && filter.keep(name)) {
      const auto file_path = append_path(path, name);
      time::seconds_t modify_time = 0;
      time::seconds_t access_time = 0;
      int64_t size = 0;
      bool is_dir = false;
      if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        auto subdir_files = walk_directory(file_path, filter);
        for (const auto& entry : subdir_files) {
          files.emplace_back(entry);
          size += entry.size();
          modify_time = std::max(modify_time, entry.modify_time());
          access_time = std::max(access_time, entry.access_time());
        }
        is_dir = true;
      } else {
        size = two_dwords_to_int64(find_data.nFileSizeLow, find_data.nFileSizeHigh);
        modify_time = time::win32_filetime_to_unix_epoch(find_data.ftLastWriteTime.dwLowDateTime,
                                                         find_data.ftLastWriteTime.dwHighDateTime);
        access_time = time::win32_filetime_to_unix_epoch(find_data.ftLastAccessTime.dwLowDateTime,
                                                         find_data.ftLastAccessTime.dwHighDateTime);
      }
      // Note: We pass inode = 0, since inode numbers are not supported on Windows (AFAIK).
      const uint64_t inode = 0U;
      files.emplace_back(file_info_t(file_path, modify_time, access_time, size, inode, is_dir));
    }
  } while (FindNextFileW(find_handle, &find_data) != 0);

  FindClose(find_handle);

  const auto find_error = GetLastError();
  if (find_error != ERROR_NO_MORE_FILES) {
    throw std::runtime_error("Failed to walk the directory.");
  }

#else
  auto* dir = opendir(path.c_str());
  if (dir == nullptr) {
    throw std::runtime_error("Unable to walk the directory.");
  }

  auto* entity = readdir(dir);
  while (entity != nullptr) {
    const auto name = std::string(entity->d_name);
    if ((name != ".") && (name != "..") && filter.keep(name)) {
      const auto file_path = append_path(path, name);
      struct stat file_stat;
      if (stat(file_path.c_str(), &file_stat) == 0) {
        time::seconds_t modify_time = 0;
        time::seconds_t access_time = 0;
        int64_t size = 0;
        bool is_dir = false;
        if (S_ISDIR(file_stat.st_mode)) {
          auto subdir_files = walk_directory(file_path, filter);
          for (const auto& entry : subdir_files) {
            files.emplace_back(entry);
            size += entry.size();
            modify_time = std::max(modify_time, entry.modify_time());
            access_time = std::max(access_time, entry.access_time());
          }
          is_dir = true;
        } else if (S_ISREG(file_stat.st_mode)) {
          size = static_cast<int64_t>(file_stat.st_size);
#ifdef __APPLE__
          modify_time = static_cast<time::seconds_t>(file_stat.st_mtimespec.tv_sec);
          access_time = static_cast<time::seconds_t>(file_stat.st_atimespec.tv_sec);
#else
          modify_time = static_cast<time::seconds_t>(file_stat.st_mtim.tv_sec);
          access_time = static_cast<time::seconds_t>(file_stat.st_atim.tv_sec);
#endif
        }
        static_assert(sizeof(file_stat.st_ino) <= sizeof(uint64_t), "Unsupported st_ino size");
        const auto inode = static_cast<uint64_t>(file_stat.st_ino);
        files.emplace_back(file_info_t(file_path, modify_time, access_time, size, inode, is_dir));
      }
    }
    entity = readdir(dir);
  }

  closedir(dir);
#endif

  return files;
}

std::string get_unique_id() {
  // Gather entropy.
  const auto pid = static_cast<uint64_t>(get_process_id());
  const auto date_t = static_cast<uint64_t>(std::time(nullptr));
  const auto hires_t = get_hires_time();
  const auto number = static_cast<uint64_t>(++s_tmp_name_number);

  // Form a string from the entropy, in a way that is suitable for a filename.
  return to_id_part(pid) + "-" + to_id_part(date_t) + "-" + to_id_part(hires_t) + "-" +
         to_id_part(number);
}

}  // namespace file
}  // namespace bcache
