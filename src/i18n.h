#pragma once
#include <string>

namespace ss::i18n {

enum class Lang { Zh, En };

void Init(const std::wstring& setting);   // "auto" -> system UI language, "zh", "en"
Lang Current();
const wchar_t* T(const wchar_t* key);     // lookup by key; falls back to key in En (never zh/empty)

} // namespace ss::i18n
