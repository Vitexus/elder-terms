// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings/regular-expression.h>
#include <elder-terms/settings/regex-capture-template.h>
#include <elder-terms/settings/hyperlink-settings.h>
#include <elder-terms/settings/macro-settings.h>
#include <boxrig/terminal.h>

#include <cstdio>
#include <cstdint>
#include <memory>
#include <stdexcept>

using namespace elder_terms;

static void expect(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

static auto compile(const std::string &pattern) {
  std::string reason;
  std::unique_ptr<RegularExpressionState, decltype(&destroy_regular_expression)> program(
      create_regular_expression(pattern, &reason), destroy_regular_expression);
  expect(program != nullptr, "compile bounded expression: " + reason);
  return program;
}

int main() {
  try {
    std::string reason;
    auto program = compile(R"(^\u{1b}(?<path>/[\p{L}]+)(?:/(?<suffix>[A-Z]+))?$)");
    for (const auto &name : {"0", "1", "2", "path", "suffix", "0002"})
      expect(regular_expression_capture_exists(program.get(), name), "metadata defines " + std::string(name));
    for (const std::string &name : {std::string("3"), std::string("missing"), std::string("Path"),
        std::string(""), std::string("+1"), std::string("1x"), std::string(100, '9'), std::string("path\0ignored", 12)})
      expect(!regular_expression_capture_exists(program.get(), name), "invalid reference is rejected before matching");
    expect(regex_capture_template_is_valid("${0}:${path}:$${suffix}", program.get(), {}, &reason),
        "defined captures validate without any input");
    expect(!regex_capture_template_is_valid("${missing}", program.get(), {}, &reason), "unknown capture cannot be saved");
    auto match = search_regular_expression(program.get(), "\x1b/日本", &reason);
    expect(match.has_value(), "control input with an absent optional capture matches");
    expect(regular_expression_capture(*match, "suffix") == std::optional<std::string>{""}, "absent group expands as empty");
    program.reset();
    expect(regular_expression_capture(*match, "path") == std::optional<std::string>{"/日本"},
        "result survives program and matcher lifetime");
    expect(!regular_expression_capture(*match, std::string("path\0ignored", 12)), "result lookup rejects embedded NUL");
    program = compile(R"((?<unused>a){0}b)");
    expect(regular_expression_capture_exists(program.get(), "unused"), "zero repetitions retain capture metadata");
    match = search_regular_expression(program.get(), "b", &reason);
    expect(match && regular_expression_capture(*match, "unused") == std::optional<std::string>{""},
        "zero repetitions yield an absent capture");

    std::string groups;
    for (int group = 0; group < 32; ++group) groups += "(a)";
    program = compile(groups);
    expect(regular_expression_capture_exists(program.get(), "32") && !regular_expression_capture_exists(program.get(), "33"),
        "maximum default group number is available");
    match = search_regular_expression(program.get(), std::string(32, 'a'), &reason);
    expect(match && regular_expression_capture(*match, "32") == std::optional<std::string>{"a"}, "last capture is readable");

    for (const auto &invalid : {std::string("a*"), std::string("(a)?"), groups + "(a)", std::string("a{4097}"),
        std::string("abc\\p{Letter}"), std::string(65537, 'a')}) {
      std::unique_ptr<RegularExpressionState, decltype(&destroy_regular_expression)> rejected(
          create_regular_expression(invalid, &reason), destroy_regular_expression);
      expect(!rejected && reason.find("byte ") != std::string::npos, "invalid syntax/capacity has a byte diagnostic");
    }
    auto *invalid = create_regular_expression(R"(abc\p{Letter})", &reason);
    expect(!invalid && reason.find("byte 3") != std::string::npos, "unknown category identifies the escape byte");

    program = compile("foo|foobar");
    match = search_regular_expression(program.get(), "foobar", &reason);
    expect(match && match->end == 6, "same-start alternatives select the longest result");
    program = compile("^a.b$");
    expect(search_regular_expression(program.get(), "a\nb", &reason).has_value(), "dot includes LF");
    program = compile("^abc$");
    expect(!search_regular_expression(program.get(), "abc\n", &reason), "anchors cover the full input including a final LF");
    program = compile(R"(\d+)");
    expect(!search_regular_expression(program.get(), "１２３", &reason), "ASCII digits exclude Unicode decimal digits");
    program = compile(R"(\p{Nd}+)");
    expect(search_regular_expression(program.get(), "１２３", &reason).has_value(), "Unicode Nd includes full-width digits");
    program = compile(R"(\p{L}+)");
    expect(!search_regular_expression(program.get(), "e\u0301", &reason), "whole match cannot split a combining grapheme");
    program = compile(R"([\p{L}\p{M}]+)");
    match = search_regular_expression(program.get(), "e\u0301", &reason);
    expect(match && match->end == 3, "union includes both parts of a grapheme");
    program = compile(R"([^\P{L}]+)");
    expect(search_regular_expression(program.get(), "日本", &reason).has_value(), "negated complement selects letters");
    program = compile(R"([\P{L}A]+)");
    match = search_regular_expression(program.get(), "A1B", &reason);
    expect(match && match->end == 2, "class alternatives form a union");

    program = compile(std::string("a\0b", 3));
    match = search_regular_expression(program.get(), std::string("xa\0by", 5), &reason);
    expect(match && match->start == 1 && match->end == 4 &&
        regular_expression_capture(*match, "0") == std::optional<std::string>{std::string("a\0b", 3)},
        "pattern, subject and result preserve NUL");
    expect(!search_regular_expression(program.get(), std::string("a\xff", 2), &reason) &&
        reason.find("byte 1") != std::string::npos, "input diagnostic identifies invalid UTF-8");
    expect(!search_regular_expression(program.get(), std::string(1024 * 1024 + 1, 'a'), &reason) &&
        reason.find("byte 1048576") != std::string::npos, "matcher input has a finite byte capacity");

    program = compile(R"((?<scheme>https?)://(?<host>[\p{L}0-9.-]+))");
    for (const std::string subject : {"前 https://example.みんな　next", "前 https://example.みんな😀",
        "前 https://日本.example/", "前 https://e\u0301/"}) {
      boxrig_regex_matcher *matcher = nullptr;
      std::size_t offset = 0;
      expect(boxrig_regex_matcher_create(program.get(), reinterpret_cast<const uint8_t *>(subject.data()),
          subject.size(), 0, &offset, &matcher) == BOXRIG_STATUS_OK, "create bounded independent matcher");
      const std::unique_ptr<boxrig_regex_matcher, decltype(&boxrig_regex_matcher_destroy)> matcher_owner(
          matcher, boxrig_regex_matcher_destroy);
      boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
      boxrig_regex_match range{};
      std::size_t used = 0;
      while (progress == BOXRIG_REGEX_IN_PROGRESS)
        expect(boxrig_regex_matcher_resume(matcher, 1, &used, &progress, &range) == BOXRIG_STATUS_OK && used <= 1,
            "independent matcher observes a work budget of one");
      boxrig_regex_result *direct = nullptr;
      if (progress == BOXRIG_REGEX_MATCH)
        expect(boxrig_regex_matcher_result(matcher, &direct) == BOXRIG_STATUS_OK, "retain independent captures");
      const std::shared_ptr<boxrig_regex_result> direct_owner(direct, boxrig_regex_result_destroy);
      if (subject.find("example.みんな") != std::string::npos)
        expect(range.start == 4 && range.end == 29 &&
            regular_expression_capture(direct, "host") == std::optional<std::string>{"example.みんな"},
            "Unicode space and emoji stop the original host capture at its UTF-8 byte boundary");

      auto config = boxrig_terminal_config_default();
      config.rows = 1;
      config.columns = 80;
      boxrig_terminal *terminal = nullptr;
      expect(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK, "create screen for the same program");
      const std::unique_ptr<boxrig_terminal, decltype(&boxrig_terminal_destroy)> terminal_owner(terminal, boxrig_terminal_destroy);
      expect(boxrig_terminal_feed(terminal, reinterpret_cast<const uint8_t *>(subject.data()), subject.size(), &used) ==
          BOXRIG_STATUS_OK && used == subject.size(), "feed Unicode screen text");
      std::uint64_t recognizer = 0;
      const auto limits = boxrig_text_recognizer_limits_default();
      expect(boxrig_terminal_register_compiled_text_recognizer(terminal, program.get(), BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY,
          &limits, &recognizer) == BOXRIG_STATUS_OK, "share the compiled program with the screen recognizer");
      bool more = true;
      std::size_t steps = 0;
      while (more)
        expect(boxrig_terminal_resume_text_recognizers(terminal, 1, &used, &more) == BOXRIG_STATUS_OK &&
            used <= 1 && ++steps < 100000, "screen recognizer observes a finite budget of one");
      std::size_t found = 0;
      boxrig_text_recognizer_event event{};
      while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        boxrig_terminal_text_recognizer_event_consume(terminal);
        expect(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED, "complete screen recognition");
        boxrig_text_recognition_result screen[2]{};
        std::size_t count = 0;
        expect(boxrig_terminal_query_text_recognizer_line_results(terminal, recognizer, event.screen, event.line_id,
            event.line_revision, event.result_set_revision, screen, 2, &count) == BOXRIG_STATUS_OK, "query screen results");
        found += count;
        for (std::size_t index = 0; index < count; ++index) {
          boxrig_regex_result *result = nullptr;
          expect(boxrig_terminal_text_recognizer_result(terminal, recognizer, screen[index].match_id,
              screen[index].result_set_revision, &result) == BOXRIG_STATUS_OK, "retain screen captures without rematching");
          const std::shared_ptr<boxrig_regex_result> screen_owner(result, boxrig_regex_result_destroy);
          for (const auto *capture : {"0", "1", "scheme", "2", "host"})
            expect(regular_expression_capture(result, capture) == regular_expression_capture(direct, capture),
                "independent and screen matching retain identical Unicode captures");
        }
      }
      expect(found == (direct ? 1U : 0U), "whole grapheme matching agrees across both routes");
    }

    for (const auto &rule : default_hyperlink_action_rules())
      expect(hyperlink_action_rule_is_valid(rule, &reason), "default rule validates its named metadata: " + reason);
    MacroRule macro{"ready", R"(^\u{1b}(?<path>/[\p{L}]+)$)", MacroSendAction{"${path}:${0}:$$"}};
    expect(macro_rule_is_valid(macro, &reason), "macro validator accepts boxrig syntax and names");
    macro.action = MacroSendAction{"${missing}"};
    expect(!macro_rule_is_valid(macro, &reason), "macro validator rejects unknown names before matching");
    std::puts("shared regex: metadata, captures, Unicode, bounds and validation passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
