#pragma once

#include <cstdint>

namespace localization {

enum class Language : int32_t {
    ZH_CN = 0,
    EN_US = 1,
};

Language get_language();
void set_language(Language language);
void set_language(int32_t language);

// Returns a process-lifetime UTF-8 string. Unknown text falls back to English.
const char* get(const char* english_text);

// Concatenated Chinese catalog text used to build the embedded font glyph range.
const char* get_zh_cn_glyph_text();

} // namespace localization
