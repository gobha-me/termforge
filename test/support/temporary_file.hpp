#pragma once

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace tfsupport {

// Like tmpfile(), but honor the caller's temporary directory. Unlink before
// returning so fclose owns the complete lifetime of the anonymous capture.
inline auto temporary_file() -> std::FILE* {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "termforge-capture-XXXXXX")
          .string();
  const int fd = ::mkstemp(pattern.data());
  if (fd < 0) return nullptr;
  if (::unlink(pattern.c_str()) != 0) {
    ::close(fd);
    return nullptr;
  }
  std::FILE* stream = ::fdopen(fd, "w+b");
  if (stream == nullptr) ::close(fd);
  return stream;
}

} // namespace tfsupport
