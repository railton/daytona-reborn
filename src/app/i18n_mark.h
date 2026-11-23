// Interface text in files that must build without the translator (their
// tests: pacing, controls, force feedback). TXR("...") marks the text for
// scripts/extract_strings.py and translates it at run time once the game has
// set `translate` (app/i18n.h tr); until then it is the French itself.
#pragma once

namespace app::i18n {
inline const char *(*translate)(const char *) = nullptr;
inline const char *tx(const char *french) { return translate ? translate(french) : french; }
} // namespace app::i18n

#define TXR(text) ::app::i18n::tx(text)
