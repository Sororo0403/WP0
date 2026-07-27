#include "font/TextRenderer.h"

#include "core/ResourceHandle.h"
#include "sprite/Sprite.h"
#include "sprite/SpriteRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <new>
#include <vector>

namespace {

constexpr char32_t kReplacementCodepoint = 0xFFFDu;

bool IsContinuationByte(uint8_t value) {
    return (value & 0xC0u) == 0x80u;
}

bool AppendCodepoint(std::vector<char32_t>& codepoints, uint32_t value) {
    try {
        if (value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
            codepoints.push_back(kReplacementCodepoint);
            return true;
        }
        codepoints.push_back(static_cast<char32_t>(value));
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

struct Utf8Sequence {
    uint32_t value = 0u;
    size_t length = 0u;
    uint32_t minimum = 0u;
};

bool DecodeSequenceHeader(uint8_t first, Utf8Sequence& sequence) {
    if ((first & 0xE0u) == 0xC0u) {
        sequence = {first & 0x1Fu, 2u, 0x80u};
    } else if ((first & 0xF0u) == 0xE0u) {
        sequence = {first & 0x0Fu, 3u, 0x800u};
    } else if ((first & 0xF8u) == 0xF0u) {
        sequence = {first & 0x07u, 4u, 0x10000u};
    } else {
        return false;
    }
    return true;
}

bool DecodeContinuationBytes(std::string_view text, size_t index, const Utf8Sequence& sequence,
                             uint32_t& value) {
    for (size_t offset = 1u; offset < sequence.length; ++offset) {
        const uint8_t next = static_cast<uint8_t>(text[index + offset]);
        if (!IsContinuationByte(next)) {
            return false;
        }
        value = (value << 6u) | static_cast<uint32_t>(next & 0x3Fu);
    }
    return true;
}

std::vector<char32_t> DecodeUtf8(std::string_view text) {
    std::vector<char32_t> codepoints;
    try {
        codepoints.reserve(text.size());
    } catch (const std::exception&) {
        return {};
    }

    size_t index = 0u;
    while (index < text.size()) {
        const uint8_t first = static_cast<uint8_t>(text[index]);
        if (first < 0x80u) {
            try {
                codepoints.push_back(static_cast<char32_t>(first));
            } catch (const std::exception&) {
                return {};
            }
            ++index;
            continue;
        }

        Utf8Sequence sequence;
        if (!DecodeSequenceHeader(first, sequence)) {
            if (!AppendCodepoint(codepoints, kReplacementCodepoint)) {
                return {};
            }
            ++index;
            continue;
        }

        if (index + sequence.length > text.size()) {
            if (!AppendCodepoint(codepoints, kReplacementCodepoint)) {
                return {};
            }
            break;
        }

        uint32_t value = sequence.value;
        if (!DecodeContinuationBytes(text, index, sequence, value) || value < sequence.minimum) {
            if (!AppendCodepoint(codepoints, kReplacementCodepoint)) {
                return {};
            }
            ++index;
            continue;
        }

        if (!AppendCodepoint(codepoints, value)) {
            return {};
        }
        index += sequence.length;
    }

    return codepoints;
}

std::vector<char32_t> DecodeWide(std::wstring_view text) {
    std::vector<char32_t> codepoints;
    try {
        codepoints.reserve(text.size());
    } catch (const std::exception&) {
        return {};
    }

    size_t index = 0u;
    while (index < text.size()) {
        const uint32_t value = static_cast<uint32_t>(text[index]);
        if constexpr (sizeof(wchar_t) == 2u) {
            if (value >= 0xD800u && value <= 0xDBFFu) {
                if (index + 1u < text.size()) {
                    const uint32_t low = static_cast<uint32_t>(text[index + 1u]);
                    if (low >= 0xDC00u && low <= 0xDFFFu) {
                        const uint32_t codepoint =
                            0x10000u + ((value - 0xD800u) << 10u) + (low - 0xDC00u);
                        try {
                            codepoints.push_back(static_cast<char32_t>(codepoint));
                        } catch (const std::exception&) {
                            return {};
                        }
                        index += 2u;
                        continue;
                    }
                }
                try {
                    codepoints.push_back(kReplacementCodepoint);
                } catch (const std::exception&) {
                    return {};
                }
                ++index;
                continue;
            }
            if (value >= 0xDC00u && value <= 0xDFFFu) {
                try {
                    codepoints.push_back(kReplacementCodepoint);
                } catch (const std::exception&) {
                    return {};
                }
                ++index;
                continue;
            }
        }

        if (!AppendCodepoint(codepoints, value)) {
            return {};
        }
        ++index;
    }

    return codepoints;
}

std::vector<char32_t> CopyCodepoints(std::u32string_view text) {
    try {
        return std::vector<char32_t>(text.begin(), text.end());
    } catch (const std::exception&) {
        return {};
    }
}

float ResolveLineSpacing(float lineSpacing) {
    return std::isfinite(lineSpacing) ? lineSpacing : 0.0f;
}

float ResolveWrapWidth(float wrapWidth) {
    return std::isfinite(wrapWidth) && wrapWidth > 0.0f ? wrapWidth : 0.0f;
}

FontHandle ResolveStyleFont(const FontManager& fontManager, const TextStyle& style) {
    return style.font.IsValid() ? style.font : fontManager.GetDefaultFont();
}

std::vector<float> CalculateLineWidths(
    FontManager& fontManager, FontHandle font, float pixelSize,
    const std::vector<char32_t>& codepoints, float wrapWidth) {
    std::vector<float> lineWidths;
    try {
        lineWidths.reserve(
            1u + static_cast<size_t>(std::count(codepoints.begin(),
                                                codepoints.end(), U'\n')));
        lineWidths.push_back(0.0f);
    } catch (const std::exception&) {
        return {};
    }

    for (char32_t codepoint : codepoints) {
        if (codepoint == U'\r') {
            continue;
        }
        if (codepoint == U'\n') {
            try {
                lineWidths.push_back(0.0f);
            } catch (const std::exception&) {
                return {};
            }
            continue;
        }

        const FontGlyph* glyph =
            fontManager.GetGlyph(font, pixelSize, codepoint);
        if (glyph == nullptr) {
            continue;
        }

        float& currentWidth = lineWidths.back();
        if (wrapWidth > 0.0f && currentWidth > 0.0f &&
            currentWidth + glyph->advanceX > wrapWidth) {
            try {
                lineWidths.push_back(0.0f);
            } catch (const std::exception&) {
                return {};
            }
        }
        lineWidths.back() += glyph->advanceX;
    }
    return lineWidths;
}

float ResolveHorizontalAlignment(TextHorizontalAlignment alignment) {
    switch (alignment) {
    case TextHorizontalAlignment::Center:
        return 0.5f;
    case TextHorizontalAlignment::Right:
        return 1.0f;
    case TextHorizontalAlignment::Left:
    default:
        return 0.0f;
    }
}

TextLayoutMetrics MeasureCodepoints(FontManager& fontManager,
                                    const std::vector<char32_t>& codepoints,
                                    const TextStyle& style) {
    TextLayoutMetrics result{};
    if (codepoints.empty()) {
        return result;
    }

    const FontHandle font = ResolveStyleFont(fontManager, style);
    if (!font.IsValid()) {
        return result;
    }

    fontManager.PrepareGlyphs(font, style.pixelSize, codepoints);
    const FontMetrics fontMetrics = fontManager.GetMetrics(font, style.pixelSize);
    const float lineAdvance = fontMetrics.lineHeight + ResolveLineSpacing(style.lineSpacing);
    const float wrapWidth = ResolveWrapWidth(style.wrapWidth);
    const std::vector<float> lineWidths = CalculateLineWidths(
        fontManager, font, style.pixelSize, codepoints, wrapWidth);
    if (lineWidths.empty()) {
        return result;
    }

    result.lineCount = static_cast<uint32_t>(lineWidths.size());
    result.size.x =
        *std::max_element(lineWidths.begin(), lineWidths.end());
    result.size.y = fontMetrics.lineHeight;
    if (result.lineCount > 1u) {
        result.size.y +=
            static_cast<float>(result.lineCount - 1u) * lineAdvance;
    }
    return result;
}

void DrawCodepoints(FontManager& fontManager, SpriteRenderer& spriteRenderer,
                    const std::vector<char32_t>& codepoints, const DirectX::XMFLOAT2& position,
                    const TextStyle& style) {
    if (codepoints.empty()) {
        return;
    }

    const FontHandle font = ResolveStyleFont(fontManager, style);
    if (!font.IsValid()) {
        return;
    }

    fontManager.PrepareGlyphs(font, style.pixelSize, codepoints);
    const FontMetrics fontMetrics = fontManager.GetMetrics(font, style.pixelSize);
    const float lineAdvance = fontMetrics.lineHeight + ResolveLineSpacing(style.lineSpacing);
    const float wrapWidth = ResolveWrapWidth(style.wrapWidth);
    const std::vector<float> lineWidths = CalculateLineWidths(
        fontManager, font, style.pixelSize, codepoints, wrapWidth);
    if (lineWidths.empty()) {
        return;
    }
    const float blockWidth =
        *std::max_element(lineWidths.begin(), lineWidths.end());
    const float alignment =
        ResolveHorizontalAlignment(style.horizontalAlignment);
    const auto resolveLineStart = [&](size_t lineIndex) {
        return position.x +
               (blockWidth - lineWidths[lineIndex]) * alignment;
    };

    size_t lineIndex = 0u;
    float cursorX = resolveLineStart(lineIndex);
    float baselineY = position.y + fontMetrics.ascent;
    float currentWidth = 0.0f;
    for (char32_t codepoint : codepoints) {
        if (codepoint == U'\r') {
            continue;
        }
        if (codepoint == U'\n') {
            ++lineIndex;
            cursorX = resolveLineStart(lineIndex);
            baselineY += lineAdvance;
            currentWidth = 0.0f;
            continue;
        }

        const FontGlyph* glyph = fontManager.GetGlyph(font, style.pixelSize, codepoint);
        if (glyph == nullptr) {
            continue;
        }

        if (wrapWidth > 0.0f && currentWidth > 0.0f &&
            currentWidth + glyph->advanceX > wrapWidth) {
            ++lineIndex;
            cursorX = resolveLineStart(lineIndex);
            baselineY += lineAdvance;
            currentWidth = 0.0f;
        }

        if (glyph->visible && IsValidResourceId(glyph->textureId)) {
            Sprite sprite{};
            sprite.position = {cursorX + glyph->offset.x, baselineY + glyph->offset.y};
            sprite.size = glyph->size;
            sprite.uvLeftTop = glyph->uvLeftTop;
            sprite.uvSize = glyph->uvSize;
            sprite.color = style.color;
            sprite.textureId = glyph->textureId;
            sprite.zOrder = style.zOrder;
            sprite.blendMode = SpriteBlendMode::Alpha;
            spriteRenderer.Draw(sprite);
        }

        cursorX += glyph->advanceX;
        currentWidth += glyph->advanceX;
    }
}

} // namespace

void TextRenderer::Initialize(FontManager* fontManager, SpriteRenderer* spriteRenderer) {
    fontManager_ = fontManager;
    spriteRenderer_ = spriteRenderer;
}

void TextRenderer::Finalize() {
    fontManager_ = nullptr;
    spriteRenderer_ = nullptr;
}

bool TextRenderer::IsReady() const {
    return fontManager_ != nullptr && spriteRenderer_ != nullptr && fontManager_->IsReady();
}

void TextRenderer::DrawText(std::string_view utf8Text, const DirectX::XMFLOAT2& position,
                            const TextStyle& style) {
    if (!IsReady()) {
        return;
    }
    DrawCodepoints(*fontManager_, *spriteRenderer_, DecodeUtf8(utf8Text), position, style);
}

void TextRenderer::DrawText(std::wstring_view text, const DirectX::XMFLOAT2& position,
                            const TextStyle& style) {
    if (!IsReady()) {
        return;
    }
    DrawCodepoints(*fontManager_, *spriteRenderer_, DecodeWide(text), position, style);
}

void TextRenderer::DrawText(std::u32string_view text, const DirectX::XMFLOAT2& position,
                            const TextStyle& style) {
    if (!IsReady()) {
        return;
    }
    DrawCodepoints(*fontManager_, *spriteRenderer_, CopyCodepoints(text), position, style);
}

TextLayoutMetrics TextRenderer::MeasureText(std::string_view utf8Text, const TextStyle& style) {
    if (!IsReady()) {
        return {};
    }
    return MeasureCodepoints(*fontManager_, DecodeUtf8(utf8Text), style);
}

TextLayoutMetrics TextRenderer::MeasureText(std::wstring_view text, const TextStyle& style) {
    if (!IsReady()) {
        return {};
    }
    return MeasureCodepoints(*fontManager_, DecodeWide(text), style);
}

TextLayoutMetrics TextRenderer::MeasureText(std::u32string_view text, const TextStyle& style) {
    if (!IsReady()) {
        return {};
    }
    return MeasureCodepoints(*fontManager_, CopyCodepoints(text), style);
}
