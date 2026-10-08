#include "terminal_dump.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void refuses(Action action) {
  try {
    action();
  } catch (const std::runtime_error&) {
    return;
  }
  throw std::runtime_error("expected terminal dump refusal");
}

void put_u64(std::string& bytes, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<char>(value >> shift));
}

void manifest(const std::string& path, const std::string& value) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << value;
  require(out.good(), "cannot write neutral region manifest");
}

}  // namespace

int main() {
  const std::string pattern = "/tmp/gsim-terminal-dump-XXXXXX";
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back('\0');
  char* created = mkdtemp(name.data());
  require(created != nullptr, "cannot create neutral test directory");
  const std::filesystem::path work(created);
  try {
    const std::string region_path = (work / "regions.txt").string();
    const std::string output_path = (work / "dump.bin").string();
    manifest(region_path, "0x1000 5\n0x2000 3\n");
    auto regions = terminal_dump::read_regions(region_path, 0x1000, 0x2000);
    require(regions.size() == 2 && regions[0].address == 0x1000 && regions[1].bytes == 3,
            "valid region order changed");
    terminal_dump::publish(output_path, regions, [](uint64_t address, size_t n, void* destination) {
      auto* bytes = static_cast<uint8_t*>(destination);
      for (size_t i = 0; i < n; ++i) bytes[i] = static_cast<uint8_t>(address + i);
    }, [] { return false; });
    std::ifstream dumped(output_path, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(dumped)), std::istreambuf_iterator<char>());
    std::string expected = "GSIMDMP1";
    put_u64(expected, 0);  // Coherent source only.
    put_u64(expected, 2);
    put_u64(expected, 0x1000); put_u64(expected, 5);
    expected.append("\0\1\2\3\4", 5);
    put_u64(expected, 0x2000); put_u64(expected, 3);
    expected.append("\0\1\2", 3);
    expected += "GSIMEND1";
    put_u64(expected, 2); put_u64(expected, 8);
    require(actual == expected, "complete coherent wire bytes changed");
    require(!std::filesystem::exists(output_path + ".partial"), "partial file remained after publish");
    refuses([&] {
      terminal_dump::publish(output_path, regions, [](uint64_t, size_t, void*) {}, [] { return false; });
    });
    std::ifstream again(output_path, std::ios::binary);
    require(std::string((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>()) == expected,
            "existing final dump was replaced");

    for (const char* bad : {"", "0x1000 0\n", "0x1000 5 extra\n", "0x1000 5\n0x1004 3\n",
                            "0x1000 5\n0x1000 5\n", "0x2fff 2\n", "-1 2\n"}) {
      manifest(region_path, bad);
      refuses([&] { terminal_dump::read_regions(region_path, 0x1000, 0x2000); });
    }
    manifest(region_path, std::string("0x1000 5\n\0", 10));
    refuses([&] { terminal_dump::read_regions(region_path, 0x1000, 0x2000); });
    manifest(region_path, "0x1000 5\n");
    refuses([&] { terminal_dump::read_regions(region_path, UINT64_MAX - 1, 4); });
    const std::string fifo_path = (work / "regions.fifo").string();
    require(mkfifo(fifo_path.c_str(), 0600) == 0, "cannot create neutral FIFO");
    refuses([&] { terminal_dump::read_regions(fifo_path, 0x1000, 0x2000); });
    const std::string symlink_path = (work / "regions.link").string();
    require(symlink(region_path.c_str(), symlink_path.c_str()) == 0, "cannot create neutral symlink");
    refuses([&] { terminal_dump::read_regions(symlink_path, 0x1000, 0x2000); });
    manifest(region_path, "0x1000 5000\n");
    auto long_region = terminal_dump::read_regions(region_path, 0x1000, 0x2000);
    const std::string failed_output = (work / "failed.bin").string();
    size_t calls = 0;
    refuses([&] {
      terminal_dump::publish(failed_output, long_region, [&](uint64_t, size_t, void*) {
        if (++calls == 2) throw std::runtime_error("coherent host-port read failed");
      }, [] { return false; });
    });
    require(calls == 2 && !std::filesystem::exists(failed_output) &&
            !std::filesystem::exists(failed_output + ".partial"),
            "failed coherent read exposed a final or partial file");
    bool cancelled = false;
    refuses([&] {
      terminal_dump::publish(failed_output, long_region, [&](uint64_t, size_t, void*) {
        cancelled = true;
      }, [&] { return cancelled; });
    });
    require(cancelled && !std::filesystem::exists(failed_output) &&
            !std::filesystem::exists(failed_output + ".partial"),
            "interruption during coherent read exposed a dump");
    unsigned cancellation_checks = 0;
    refuses([&] {
      terminal_dump::publish(failed_output, regions, [](uint64_t, size_t, void*) {},
                             [&] { return ++cancellation_checks == 8; });
    });
    require(cancellation_checks == 8 && !std::filesystem::exists(failed_output) &&
            !std::filesystem::exists(failed_output + ".partial"),
            "interruption at final publication exposed a dump");

    const std::string packet_output = (work / "packet.bin").string();
    const std::vector<terminal_dump::Region> packet_regions{{0x1000, 8}, {0x2000, 32}};
    static const char packet_body[] = "OUT_BIN_BEGIN v1 x 1 1 1 u 1\n\0OUT_BIN_END v1 0000000000000000\nDONE\n";
    const std::string payload(packet_body, sizeof(packet_body) - 1);
    auto packet_read = [&](uint64_t address, size_t n, void* destination) {
      if (address == 0x1000) {
        require(n == 8, "packet length read changed");
        auto* bytes = static_cast<uint8_t*>(destination);
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(payload.size() >> (8 * i));
      } else {
        require(address >= 0x2000 && address + n <= 0x2000 + payload.size(),
                "packet copied unused arena capacity");
        memcpy(destination, payload.data() + address - 0x2000, n);
      }
    };
    const std::vector<terminal_dump::Region> sized_packet{{0x1000, 8}, {0x2000, 128}};
    refuses([&] {
      terminal_dump::publish_packet(failed_output, {{0x2000, 8}, {0x2004, 128}},
                                    packet_read, [] { return false; });
    });
    refuses([&] {
      terminal_dump::publish_packet(failed_output, {{UINT64_MAX - 7, 8}, {0x2000, 128}},
                                    packet_read, [] { return false; });
    });
    require(!std::filesystem::exists(failed_output), "overlap or overflow exposed a packet");
    terminal_dump::publish_packet(packet_output, sized_packet, packet_read, [] { return false; });
    std::ifstream packet_file(packet_output, std::ios::binary);
    const std::string actual_packet((std::istreambuf_iterator<char>(packet_file)), std::istreambuf_iterator<char>());
    std::string expected_packet = "GSIMPKT1";
    put_u64(expected_packet, 0x1000); put_u64(expected_packet, 0x2000);
    put_u64(expected_packet, 128); put_u64(expected_packet, payload.size());
    expected_packet += payload + "PKTEND1\n";
    require(actual_packet == expected_packet && !std::filesystem::exists(packet_output + ".partial"),
            "packet wire lost arbitrary payload bytes or exposed a partial file");
    refuses([&] { terminal_dump::publish_packet(packet_output, sized_packet, packet_read, [] { return false; }); });
    refuses([&] { terminal_dump::publish_packet(failed_output, packet_regions, packet_read, [] { return false; }); });
    refuses([&] {
      terminal_dump::publish_packet(failed_output, sized_packet, [](uint64_t, size_t n, void* destination) {
        memset(destination, 0, n);
      }, [] { return false; });
    });
    require(!std::filesystem::exists(failed_output), "invalid published length exposed a packet");
    unsigned length_reads = 0;
    refuses([&] {
      terminal_dump::publish_packet(failed_output, sized_packet,
          [&](uint64_t address, size_t n, void* destination) {
            packet_read(address, n, destination);
            if (address == 0x1000 && ++length_reads == 2)
              static_cast<uint8_t*>(destination)[0] ^= 1;
          }, [] { return false; });
    });
    require(length_reads == 2 && !std::filesystem::exists(failed_output) &&
            !std::filesystem::exists(failed_output + ".partial"),
            "changed published length exposed a packet");
    bool packet_cancelled = false;
    refuses([&] {
      terminal_dump::publish_packet(failed_output, sized_packet,
          [&](uint64_t address, size_t n, void* destination) {
            packet_read(address, n, destination);
            packet_cancelled = true;
          }, [&] { return packet_cancelled; });
    });
    require(packet_cancelled && !std::filesystem::exists(failed_output),
            "interrupted coherent packet exposed a final file");

    std::string valid_regions = "+dump-regions=" + region_path;
    std::string valid_output = "+dump-out=" + output_path;
    char executable[] = "emulator";
    char coherent[] = "+dump-mode=coherent";
    char* good[] = {executable, valid_regions.data(), valid_output.data(), coherent};
    require(terminal_dump::request_from_args(4, good).enabled, "coherent opt-in not selected");
    char* legacy_mode[] = {executable, valid_regions.data(), valid_output.data()};
    require(terminal_dump::request_from_args(3, legacy_mode).mode == terminal_dump::Request::Mode::coherent,
            "existing implicit coherent mode changed");
    char packet_mode[] = "+dump-mode=coherent-packet";
    char* packet_args[] = {executable, valid_regions.data(), valid_output.data(), packet_mode};
    require(terminal_dump::request_from_args(4, packet_args).mode ==
                terminal_dump::Request::Mode::coherent_packet,
            "packet mode was not explicitly selected");
    char hybrid[] = "+dump-mode=hybrid";
    char* bad_mode[] = {executable, valid_regions.data(), valid_output.data(), hybrid};
    refuses([&] { terminal_dump::request_from_args(4, bad_mode); });
    char* duplicate[] = {executable, valid_regions.data(), valid_regions.data(), valid_output.data()};
    refuses([&] { terminal_dump::request_from_args(4, duplicate); });
    char* missing[] = {executable, valid_regions.data()};
    refuses([&] { terminal_dump::request_from_args(2, missing); });
  } catch (...) {
    std::filesystem::remove_all(work);
    throw;
  }
  std::filesystem::remove_all(work);
  std::cout << "terminal coherent dump: pass\n";
}
