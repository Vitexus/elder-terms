// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-sessions/terminal-text-stream.h"

#include <initializer_list>
#include <stdexcept>
#include <vector>

using namespace elder_terms;
using Bytes = std::vector<unsigned char>;

static TerminalTextConversionResult convert(TerminalTextStream *stream, std::initializer_list<unsigned char> bytes) {
  return convert_terminal_text(stream, Bytes(bytes));
}

static void expect(const Bytes &actual, std::initializer_list<unsigned char> expected) {
  if (actual != Bytes(expected)) throw std::runtime_error("unexpected converted bytes");
}

static void utf8_passes_through_without_key_rewriting() {
  auto decoder = create_terminal_text_decoder("UTF-8");
  auto encoder = create_terminal_text_encoder("UTF-8");
  const auto received = convert(decoder.get(), {'a', 0xc3, 0xa9, 0x1b, '[', 'A'});
  const auto sent = convert(encoder.get(), {'a', 0xc3, 0xa9, 0x1b, '[', 'A'});
  expect(received.bytes, {'a', 0xc3, 0xa9, 0x1b, '[', 'A'});
  expect(sent.bytes, {'a', 0xc3, 0xa9, 0x1b, '[', 'A'});
  if (received.used_replacement || sent.used_replacement) throw std::runtime_error("valid UTF-8 was replaced");
}

static void shift_jis_spans_chunks_in_both_directions() {
  auto decoder = create_terminal_text_decoder("SHIFT-JIS");
  auto encoder = create_terminal_text_encoder("SHIFT-JIS");
  expect(convert(decoder.get(), {0x93}).bytes, {});
  expect(convert(decoder.get(), {0xfa, 0x96, 0x7b, 0x8c, 0xea}).bytes,
      {0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac, 0xe8, 0xaa, 0x9e});
  expect(convert(encoder.get(), {0xe6, 0x97}).bytes, {});
  expect(convert(encoder.get(), {0xa5, 0xe6, 0x9c, 0xac, 0xe8, 0xaa, 0x9e}).bytes,
      {0x93, 0xfa, 0x96, 0x7b, 0x8c, 0xea});
}

static void invalid_text_uses_directional_replacements() {
  auto decoder = create_terminal_text_decoder("ASCII");
  auto encoder = create_terminal_text_encoder("ASCII");
  const auto received = convert(decoder.get(), {0xff, 'A'});
  const auto sent = convert(encoder.get(), {0xc3, 0xa9, 'A'});
  expect(received.bytes, {0xef, 0xbf, 0xbd, 'A'});
  expect(sent.bytes, {'?', 'A'});
  if (!received.used_replacement || !sent.used_replacement) throw std::runtime_error("replacement was not reported");
}

static void shift_state_survives_chunks_and_finishes_once() {
  auto encoder = create_terminal_text_encoder("ISO-2022-JP");
  auto decoder = create_terminal_text_decoder("ISO-2022-JP");
  const auto first = convert(encoder.get(), {0xe6, 0x97, 0xa5});
  const auto second = convert(encoder.get(), {0xe6, 0x9c, 0xac});
  expect(first.bytes, {0x1b, '$', 'B', 0x46, 0x7c});
  expect(second.bytes, {0x4b, 0x5c});
  expect(convert_terminal_text(decoder.get(), first.bytes).bytes, {0xe6, 0x97, 0xa5});
  expect(convert_terminal_text(decoder.get(), second.bytes).bytes, {0xe6, 0x9c, 0xac});
  expect(finish_terminal_text(encoder.get()).bytes, {0x1b, '(', 'B'});
  bool rejected = false;
  try { (void)finish_terminal_text(encoder.get()); }
  catch (const std::logic_error &) { rejected = true; }
  if (!rejected) throw std::runtime_error("a finished stream accepted another finish");
}

static void incomplete_input_is_finalized_at_eof() {
  auto decoder = create_terminal_text_decoder("UTF-8");
  auto encoder = create_terminal_text_encoder("UTF-8");
  expect(convert(decoder.get(), {0xe6, 0x97}).bytes, {});
  expect(convert(encoder.get(), {0xe6, 0x97}).bytes, {});
  const auto received = finish_terminal_text(decoder.get());
  const auto sent = finish_terminal_text(encoder.get());
  expect(received.bytes, {0xef, 0xbf, 0xbd});
  expect(sent.bytes, {'?'});
  if (!received.used_replacement || !sent.used_replacement) throw std::runtime_error("incomplete EOF was not reported");
}

int main() {
  utf8_passes_through_without_key_rewriting();
  shift_jis_spans_chunks_in_both_directions();
  invalid_text_uses_directional_replacements();
  shift_state_survives_chunks_and_finishes_once();
  incomplete_input_is_finalized_at_eof();
}
