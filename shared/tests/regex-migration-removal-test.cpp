#include <elder-terms/settings/macro-settings.h>
#include <cstdio>

int main() {
  for (const auto *pattern : {"(?i)ready", "a(?=b)", R"((a)\1)"}) {
    const elder_terms::MacroRule rule{"removed-syntax", pattern, elder_terms::MacroSendAction{"accepted"}};
    std::string reason;
    if (elder_terms::macro_rule_is_valid(rule, &reason)) {
      std::fputs("PCRE-only macro syntax is still accepted\n", stderr);
      return 1;
    }
  }
  return 0;
}
