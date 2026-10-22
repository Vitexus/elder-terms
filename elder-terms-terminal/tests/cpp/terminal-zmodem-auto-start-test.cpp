// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-zmodem-auto-start.h"

#include <iostream>
#include <optional>
#include <string>

namespace elder_terms_zmodem_auto_start_test {

static bool expect_true(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

static std::string build_zhex_header(const std::string &digits) {
  std::string header = "**";
  header.push_back(0x18);
  header.push_back('B');
  header += digits;
  header += "\r\n";
  header.push_back(0x11);
  return header;
}

static bool run() {
  elder_terms::TerminalZmodemAutoStartDetectorState state;

  if (!expect_true(
          !elder_terms::feed_terminal_zmodem_auto_start_detector(
               &state, "plain terminal output")
               .has_value(),
          "plain output should not trigger ZMODEM auto-start")) {
    return false;
  }

  const std::string zrqinit = build_zhex_header("00000000000000");
  if (!expect_true(
          elder_terms::feed_terminal_zmodem_auto_start_detector(
              &state, zrqinit) ==
              std::optional<elder_terms::TerminalTransferDirection>(
                  elder_terms::TerminalTransferDirection::receive),
          "ZRQINIT should request local ZMODEM receive")) {
    return false;
  }

  const std::string zrinit = build_zhex_header("0100000063f694");
  if (!expect_true(
          elder_terms::feed_terminal_zmodem_auto_start_detector(
              &state, zrinit) ==
              std::optional<elder_terms::TerminalTransferDirection>(
                  elder_terms::TerminalTransferDirection::send),
          "ZRINIT should request local ZMODEM send")) {
    return false;
  }

  const std::string split_zrqinit = build_zhex_header("00000000000000");
  if (!expect_true(
          !elder_terms::feed_terminal_zmodem_auto_start_detector(
               &state, split_zrqinit.substr(0, 5))
               .has_value(),
          "partial ZRQINIT should wait for remaining bytes")) {
    return false;
  }
  if (!expect_true(
          elder_terms::feed_terminal_zmodem_auto_start_detector(
              &state, split_zrqinit.substr(5)) ==
              std::optional<elder_terms::TerminalTransferDirection>(
                  elder_terms::TerminalTransferDirection::receive),
          "split ZRQINIT should trigger after the full header arrives")) {
    return false;
  }

  const std::string invalid_crc = build_zhex_header("00000000000001");
  if (!expect_true(
          !elder_terms::feed_terminal_zmodem_auto_start_detector(
               &state, invalid_crc)
               .has_value(),
          "headers with invalid CRC should not trigger ZMODEM auto-start")) {
    return false;
  }

  for (const auto &header : {zrqinit, zrinit}) {
    const auto direction = header == zrqinit ? elder_terms::TerminalTransferDirection::receive :
        elder_terms::TerminalTransferDirection::send;
    const auto stream = "BEFORE" + header + "BINARY";
    for (std::size_t split = 0; split <= stream.size(); ++split) {
      elder_terms::TerminalZmodemAutoStartDetectorState filtering;
      auto first = elder_terms::filter_terminal_zmodem_auto_start(&filtering, stream.substr(0, split));
      auto second = first.direction ? elder_terms::TerminalZmodemAutoStartResult{
          {}, stream.substr(split), {}} :
          elder_terms::filter_terminal_zmodem_auto_start(&filtering, stream.substr(split));
      if (!expect_true(first.terminal_data + second.terminal_data == "BEFORE" &&
          first.transfer_data + second.transfer_data == header + "BINARY" &&
          (first.direction ? first.direction : second.direction) == direction,
          "every split retains the complete binary preamble and preceding normal text")) return false;
    }
  }
  elder_terms::TerminalZmodemAutoStartDetectorState filtering;
  const std::string ordinary = "* ** *** x*" + invalid_crc + "\r\nPLAIN";
  std::string restored;
  for (char value : ordinary) {
    const auto result = elder_terms::filter_terminal_zmodem_auto_start(&filtering, std::string_view(&value, 1));
    restored += result.terminal_data;
    if (!expect_true(!result.direction && result.transfer_data.empty(), "invalid preambles remain ordinary data")) return false;
  }
  if (!expect_true(restored == ordinary && filtering.tail.empty(), "ordinary bytes are neither lost nor duplicated")) return false;

  return true;
}

} // namespace elder_terms_zmodem_auto_start_test

int main() {
  return elder_terms_zmodem_auto_start_test::run() ? 0 : 1;
}
