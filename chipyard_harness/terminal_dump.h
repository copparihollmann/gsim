// Opt-in terminal memory readback. The supplied reader must use the SoC's
// coherent host port; this format does not authorize a backing-store read.
#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

namespace terminal_dump {

struct Request {
  bool enabled = false;
  std::string regions;
  std::string output;
};

struct Region {
  uint64_t address;
  uint64_t bytes;
};

inline Request request_from_args(int argc, char** argv) {
  Request request;
  bool have_regions = false, have_output = false, have_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg.rfind("+dump-", 0) != 0) continue;
    request.enabled = true;
    if (arg.rfind("+dump-regions=", 0) == 0 && !have_regions) {
      request.regions = arg.substr(sizeof("+dump-regions=") - 1);
      have_regions = true;
    } else if (arg.rfind("+dump-out=", 0) == 0 && !have_output) {
      request.output = arg.substr(sizeof("+dump-out=") - 1);
      have_output = true;
    } else if (arg == "+dump-mode=coherent" && !have_mode) {
      have_mode = true;
    } else {
      throw std::runtime_error("unknown or duplicate terminal dump option");
    }
  }
  if (request.enabled &&
      (!have_regions || !have_output || request.regions.empty() || request.output.empty() ||
       !std::filesystem::path(request.regions).is_absolute() ||
       !std::filesystem::path(request.output).is_absolute()))
    throw std::runtime_error("terminal dump requires absolute region and output paths");
  return request;
}

inline uint64_t number(const std::string& token) {
  if (token.empty() || token[0] < '0' || token[0] > '9')
    throw std::runtime_error("invalid terminal dump region integer");
  size_t consumed = 0;
  uint64_t value;
  try {
    value = std::stoull(token, &consumed, 0);
  } catch (const std::exception&) {
    throw std::runtime_error("invalid terminal dump region integer");
  }
  if (consumed != token.size()) throw std::runtime_error("invalid terminal dump region integer");
  return value;
}

inline std::vector<Region> read_regions(const std::string& path, uint64_t base, uint64_t memory_bytes) {
  constexpr size_t maximum_manifest_bytes = 1 << 20;
  constexpr size_t maximum_regions = 4096;
  if (memory_bytes == 0 || base > UINT64_MAX - memory_bytes)
    throw std::runtime_error("invalid terminal dump DRAM bounds");
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) throw std::runtime_error("cannot open terminal dump region manifest");
  struct stat info {};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<uint64_t>(info.st_size) > maximum_manifest_bytes) {
    close(fd);
    throw std::runtime_error("terminal dump region manifest is not a bounded regular file");
  }
  std::string content(static_cast<size_t>(info.st_size), '\0');
  size_t read_bytes = 0;
  while (read_bytes < content.size()) {
    ssize_t got = read(fd, content.data() + read_bytes, content.size() - read_bytes);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) { close(fd); throw std::runtime_error("terminal dump region manifest was truncated"); }
    read_bytes += static_cast<size_t>(got);
  }
  struct stat after {};
  if (fstat(fd, &after) != 0 || after.st_dev != info.st_dev || after.st_ino != info.st_ino ||
      after.st_size != info.st_size || after.st_mtim.tv_sec != info.st_mtim.tv_sec ||
      after.st_mtim.tv_nsec != info.st_mtim.tv_nsec ||
      after.st_ctim.tv_sec != info.st_ctim.tv_sec || after.st_ctim.tv_nsec != info.st_ctim.tv_nsec) {
    close(fd);
    throw std::runtime_error("terminal dump region manifest changed while reading");
  }
  if (close(fd) != 0) throw std::runtime_error("cannot close terminal dump region manifest");
  if (content.find('\0') != std::string::npos)
    throw std::runtime_error("terminal dump region manifest contains NUL bytes");
  std::vector<Region> regions;
  std::istringstream input(content);
  std::string line;
  uint64_t total = 0;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    std::istringstream fields(line);
    std::string address_text, bytes_text, extra;
    if (!(fields >> address_text >> bytes_text) || fields >> extra)
      throw std::runtime_error("malformed terminal dump region row");
    const uint64_t address = number(address_text), bytes = number(bytes_text);
    if (bytes == 0 || address < base || address - base > memory_bytes ||
        bytes > memory_bytes - (address - base) || bytes > memory_bytes - total)
      throw std::runtime_error("terminal dump region exceeds declared DRAM or byte budget");
    regions.push_back({address, bytes});
    total += bytes;
    if (regions.size() > maximum_regions) throw std::runtime_error("too many terminal dump regions");
  }
  if (regions.empty()) throw std::runtime_error("terminal dump requests no regions");
  auto ordered = regions;
  std::sort(ordered.begin(), ordered.end(), [](const Region& a, const Region& b) {
    return a.address < b.address;
  });
  for (size_t i = 1; i < ordered.size(); ++i) {
    if (ordered[i].address < ordered[i - 1].address + ordered[i - 1].bytes)
      throw std::runtime_error("terminal dump regions overlap");
  }
  return regions;
}

inline void write_all(int fd, const void* data, size_t bytes) {
  const auto* source = static_cast<const uint8_t*>(data);
  while (bytes) {
    ssize_t written = write(fd, source, bytes);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("terminal dump write failed");
    source += written;
    bytes -= static_cast<size_t>(written);
  }
}

inline void write_u64(int fd, uint64_t value) {
  uint8_t encoded[8];
  for (unsigned i = 0; i < 8; ++i) encoded[i] = static_cast<uint8_t>(value >> (i * 8));
  write_all(fd, encoded, sizeof encoded);
}

using CoherentRead = std::function<void(uint64_t, size_t, void*)>;
using Cancelled = std::function<bool()>;

inline void publish(const std::string& output, const std::vector<Region>& regions,
                    const CoherentRead& coherent_read, const Cancelled& cancelled) {
  if (regions.empty() || !coherent_read || !cancelled)
    throw std::runtime_error("terminal dump has no read or completion authority");
  const std::string partial = output + ".partial";
  int fd = open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("cannot create exclusive terminal dump partial file");
  bool published = false;
  try {
    if (cancelled()) throw std::runtime_error("terminal dump interrupted");
    write_all(fd, "GSIMDMP1", 8);
    write_u64(fd, 0);  // The only admitted source is a coherent SoC host-port read.
    write_u64(fd, regions.size());
    uint64_t total = 0;
    std::array<uint8_t, 4096> buffer {};
    for (const Region& region : regions) {
      write_u64(fd, region.address);
      write_u64(fd, region.bytes);
      for (uint64_t offset = 0; offset < region.bytes;) {
        if (cancelled()) throw std::runtime_error("terminal dump interrupted");
        size_t n = static_cast<size_t>(std::min<uint64_t>(buffer.size(), region.bytes - offset));
        coherent_read(region.address + offset, n, buffer.data());
        if (cancelled()) throw std::runtime_error("terminal dump interrupted");
        write_all(fd, buffer.data(), n);
        offset += n;
      }
      total += region.bytes;
    }
    write_all(fd, "GSIMEND1", 8);
    write_u64(fd, regions.size());
    write_u64(fd, total);
    if (cancelled()) throw std::runtime_error("terminal dump interrupted");
    if (fsync(fd) != 0) throw std::runtime_error("terminal dump sync failed");
    if (close(fd) != 0) { fd = -1; throw std::runtime_error("terminal dump close failed"); }
    fd = -1;
    if (cancelled()) throw std::runtime_error("terminal dump interrupted");
    // link() refuses an existing final path, unlike rename(). The complete
    // inode becomes visible atomically and is never a partial passing result.
    if (link(partial.c_str(), output.c_str()) != 0)
      throw std::runtime_error("terminal dump final path already exists or cannot be published");
    published = true;
    if (cancelled()) throw std::runtime_error("terminal dump interrupted");
    unlink(partial.c_str());
  } catch (...) {
    if (fd >= 0) close(fd);
    if (published) unlink(output.c_str());
    unlink(partial.c_str());
    throw;
  }
}

}  // namespace terminal_dump
