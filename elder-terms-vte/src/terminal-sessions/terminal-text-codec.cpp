#include "terminal-text-codec.h"

namespace elder_terms {

class TerminalCursorKeyMapper {
private:
  TerminalCursorKeyMode mode;
  std::string pending;

  static unsigned char map_cursor_sequence(unsigned char introducer,
                                           unsigned char final_byte) {
    if (introducer != '[' && introducer != 'O') {
      return 0;
    }
    switch (final_byte) {
    case 'A':
      return 0x1e;
    case 'B':
      return 0x1f;
    case 'C':
      return 0x1c;
    case 'D':
      return 0x1d;
    default:
      return 0;
    }
  }

public:
  explicit TerminalCursorKeyMapper(TerminalCursorKeyMode mode)
      : mode(mode) {
  }

  std::vector<unsigned char>
  transform(std::span<const unsigned char> bytes) {
    if (mode == TerminalCursorKeyMode::normal) {
      return {bytes.begin(), bytes.end()};
    }

    if (!bytes.empty()) {
      pending.append(reinterpret_cast<const char *>(bytes.data()),
                     bytes.size());
    }
    std::vector<unsigned char> output;
    std::size_t index = 0;
    while (index < pending.size()) {
      const auto current = static_cast<unsigned char>(pending[index]);
      if (current != 0x1b) {
        output.push_back(current);
        ++index;
        continue;
      }
      if (index + 1 >= pending.size()) {
        break;
      }

      const auto introducer =
          static_cast<unsigned char>(pending[index + 1]);
      if (introducer != '[' && introducer != 'O') {
        output.push_back(current);
        ++index;
        continue;
      }
      if (index + 2 >= pending.size()) {
        break;
      }

      const auto final_byte =
          static_cast<unsigned char>(pending[index + 2]);
      const unsigned char mapped =
          map_cursor_sequence(introducer, final_byte);
      if (mapped != 0) {
        output.push_back(mapped);
        index += 3;
        continue;
      }

      output.push_back(current);
      ++index;
    }
    pending.erase(0, index);
    return output;
  }
};

class TerminalTextCodec::Impl {
public:
  TerminalTextStreamPtr decoder;
  TerminalTextEncoder encoder;
  TerminalCursorKeyMapper cursor_key_mapper;

  explicit Impl(const TerminalTextSettings &settings)
      : decoder(create_terminal_text_decoder(settings.encoding)),
        encoder(settings),
        cursor_key_mapper(settings.cursor_key_mode) {
  }
};

class TerminalTextEncoder::Impl {
public:
  TerminalTextStreamPtr encoder;

  explicit Impl(const TerminalTextSettings &settings)
      : encoder(create_terminal_text_encoder(settings.encoding)) {
  }
};

TerminalTextEncoder::TerminalTextEncoder(const TerminalTextSettings &settings)
    : impl(std::make_unique<Impl>(settings)) {
}

TerminalTextEncoder::~TerminalTextEncoder() = default;

TerminalTextConversionResult
TerminalTextEncoder::encode(std::span<const unsigned char> bytes) {
  return convert_terminal_text(impl->encoder.get(), bytes);
}

TerminalTextConversionResult TerminalTextEncoder::finish() {
  return finish_terminal_text(impl->encoder.get());
}

TerminalTextCodec::TerminalTextCodec(const TerminalTextSettings &settings)
    : impl(std::make_unique<Impl>(settings)) {
}

TerminalTextCodec::~TerminalTextCodec() = default;

TerminalTextConversionResult
TerminalTextCodec::decode(std::span<const unsigned char> bytes) {
  return convert_terminal_text(impl->decoder.get(), bytes);
}

TerminalTextConversionResult
TerminalTextCodec::encode(std::span<const unsigned char> bytes) {
  const std::vector<unsigned char> mapped =
      impl->cursor_key_mapper.transform(bytes);
  return impl->encoder.encode(
      std::span<const unsigned char>(mapped.data(), mapped.size()));
}

} // namespace elder_terms
