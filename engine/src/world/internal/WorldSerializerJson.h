#pragma once

#include "world/World.h"

#include "nlohmann/json.hpp"

#include <cmath>
#include <exception>
#include <utility>

namespace WorldSerializerJson {
using Json = nlohmann::json;

inline Json EncodeFloat2(const DirectX::XMFLOAT2& value) {
    return Json::array({value.x, value.y});
}

inline Json EncodeFloat3(const DirectX::XMFLOAT3& value) {
    return Json::array({value.x, value.y, value.z});
}

inline Json EncodeFloat4(const DirectX::XMFLOAT4& value) {
    return Json::array({value.x, value.y, value.z, value.w});
}

inline bool DecodeFloat2(const Json& value, DirectX::XMFLOAT2& result) {
    if (!value.is_array() || value.size() != 2u) {
        return false;
    }
    try {
        result = {value[0].get<float>(), value[1].get<float>()};
    } catch (const std::exception&) {
        return false;
    }
    return std::isfinite(result.x) && std::isfinite(result.y);
}

inline bool DecodeFloat3(const Json& value, DirectX::XMFLOAT3& result) {
    if (!value.is_array() || value.size() != 3u) {
        return false;
    }
    try {
        result = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
    } catch (const std::exception&) {
        return false;
    }
    return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z);
}

inline bool DecodeFloat4(const Json& value, DirectX::XMFLOAT4& result) {
    if (!value.is_array() || value.size() != 4u) {
        return false;
    }
    try {
        result = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>(),
                  value[3].get<float>()};
    } catch (const std::exception&) {
        return false;
    }
    return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z) &&
           std::isfinite(result.w);
}

inline const char* EncodeUiAnchor(UiAnchor anchor) {
    switch (anchor) {
    case UiAnchor::TopLeft:
        return "TopLeft";
    case UiAnchor::TopCenter:
        return "TopCenter";
    case UiAnchor::TopRight:
        return "TopRight";
    case UiAnchor::MiddleLeft:
        return "MiddleLeft";
    case UiAnchor::Center:
        return "Center";
    case UiAnchor::MiddleRight:
        return "MiddleRight";
    case UiAnchor::BottomLeft:
        return "BottomLeft";
    case UiAnchor::BottomCenter:
        return "BottomCenter";
    case UiAnchor::BottomRight:
        return "BottomRight";
    }
    return "TopLeft";
}

inline bool DecodeUiAnchor(const Json& value, UiAnchor& anchor) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "TopLeft") {
        anchor = UiAnchor::TopLeft;
    } else if (encoded == "TopCenter") {
        anchor = UiAnchor::TopCenter;
    } else if (encoded == "TopRight") {
        anchor = UiAnchor::TopRight;
    } else if (encoded == "MiddleLeft") {
        anchor = UiAnchor::MiddleLeft;
    } else if (encoded == "Center") {
        anchor = UiAnchor::Center;
    } else if (encoded == "MiddleRight") {
        anchor = UiAnchor::MiddleRight;
    } else if (encoded == "BottomLeft") {
        anchor = UiAnchor::BottomLeft;
    } else if (encoded == "BottomCenter") {
        anchor = UiAnchor::BottomCenter;
    } else if (encoded == "BottomRight") {
        anchor = UiAnchor::BottomRight;
    } else {
        return false;
    }
    return true;
}

inline DirectX::XMFLOAT2 UiAnchorFactor(UiAnchor anchor) {
    switch (anchor) {
    case UiAnchor::TopLeft:
        return {0.0f, 0.0f};
    case UiAnchor::TopCenter:
        return {0.5f, 0.0f};
    case UiAnchor::TopRight:
        return {1.0f, 0.0f};
    case UiAnchor::MiddleLeft:
        return {0.0f, 0.5f};
    case UiAnchor::Center:
        return {0.5f, 0.5f};
    case UiAnchor::MiddleRight:
        return {1.0f, 0.5f};
    case UiAnchor::BottomLeft:
        return {0.0f, 1.0f};
    case UiAnchor::BottomCenter:
        return {0.5f, 1.0f};
    case UiAnchor::BottomRight:
        return {1.0f, 1.0f};
    }
    return {0.0f, 0.0f};
}

inline const char* EncodeImageType(ImageType type) {
    return type == ImageType::Filled ? "Filled" : "Simple";
}

inline bool DecodeImageType(const Json& value, ImageType& type) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "Simple") {
        type = ImageType::Simple;
    } else if (encoded == "Filled") {
        type = ImageType::Filled;
    } else {
        return false;
    }
    return true;
}

inline const char* EncodeImageFillMethod(ImageFillMethod method) {
    return method == ImageFillMethod::Vertical ? "Vertical"
                                               : "Horizontal";
}

inline bool DecodeImageFillMethod(const Json& value, ImageFillMethod& method) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "Horizontal") {
        method = ImageFillMethod::Horizontal;
    } else if (encoded == "Vertical") {
        method = ImageFillMethod::Vertical;
    } else {
        return false;
    }
    return true;
}

inline const char* EncodeSliderDirection(SliderDirection direction) {
    switch (direction) {
    case SliderDirection::LeftToRight:
        return "LeftToRight";
    case SliderDirection::RightToLeft:
        return "RightToLeft";
    case SliderDirection::BottomToTop:
        return "BottomToTop";
    case SliderDirection::TopToBottom:
        return "TopToBottom";
    }
    return "LeftToRight";
}

inline bool DecodeSliderDirection(const Json& value,
                           SliderDirection& direction) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "LeftToRight") {
        direction = SliderDirection::LeftToRight;
    } else if (encoded == "RightToLeft") {
        direction = SliderDirection::RightToLeft;
    } else if (encoded == "BottomToTop") {
        direction = SliderDirection::BottomToTop;
    } else if (encoded == "TopToBottom") {
        direction = SliderDirection::TopToBottom;
    } else {
        return false;
    }
    return true;
}

inline const char* EncodeButtonNavigationMode(ButtonNavigationMode mode) {
    switch (mode) {
    case ButtonNavigationMode::Automatic:
        return "Automatic";
    case ButtonNavigationMode::Explicit:
        return "Explicit";
    case ButtonNavigationMode::None:
        return "None";
    }
    return "Automatic";
}

inline bool DecodeButtonNavigationMode(const Json& value,
                                ButtonNavigationMode& mode) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "Automatic") {
        mode = ButtonNavigationMode::Automatic;
    } else if (encoded == "Explicit") {
        mode = ButtonNavigationMode::Explicit;
    } else if (encoded == "None") {
        mode = ButtonNavigationMode::None;
    } else {
        return false;
    }
    return true;
}

inline const char* EncodeCanvasScreenMatchMode(CanvasScreenMatchMode mode) {
    switch (mode) {
    case CanvasScreenMatchMode::MatchWidthOrHeight:
        return "MatchWidthOrHeight";
    case CanvasScreenMatchMode::Expand:
        return "Expand";
    case CanvasScreenMatchMode::Shrink:
        return "Shrink";
    }
    return "Expand";
}

inline bool DecodeCanvasScreenMatchMode(const Json& value,
                                 CanvasScreenMatchMode& mode) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "MatchWidthOrHeight") {
        mode = CanvasScreenMatchMode::MatchWidthOrHeight;
    } else if (encoded == "Expand") {
        mode = CanvasScreenMatchMode::Expand;
    } else if (encoded == "Shrink") {
        mode = CanvasScreenMatchMode::Shrink;
    } else {
        return false;
    }
    return true;
}

inline const char* EncodeCanvasScaleMode(CanvasScaleMode mode) {
    return mode == CanvasScaleMode::ConstantPixelSize
               ? "ConstantPixelSize"
               : "ScaleWithScreenSize";
}

inline bool DecodeCanvasScaleMode(const Json& value, CanvasScaleMode& mode) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "ConstantPixelSize") {
        mode = CanvasScaleMode::ConstantPixelSize;
    } else if (encoded == "ScaleWithScreenSize") {
        mode = CanvasScaleMode::ScaleWithScreenSize;
    } else {
        return false;
    }
    return true;
}

inline void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}
} // namespace WorldSerializerJson
