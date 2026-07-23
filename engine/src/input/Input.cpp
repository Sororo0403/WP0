#include "input/Input.h"

#include "input/InputReplayLimits.h"
#include "internal/InputInternal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cctype>
#include <exception>
#include <filesystem>
#include <new>
#include <string>
#include <utility>

#include <objbase.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "ole32.lib")

namespace {
bool IsValidActionName(std::string_view name) {
    return !name.empty() && name.size() <= 64u &&
           name.find('\0') == std::string_view::npos;
}

bool IsValidActionId(std::string_view id) {
    if (id.size() != 36u || id[8] != '-' || id[13] != '-' ||
        id[18] != '-' || id[23] != '-') {
        return false;
    }
    for (size_t index = 0u; index < id.size(); ++index) {
        if (index == 8u || index == 13u || index == 18u || index == 23u) {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(id[index]))) {
            return false;
        }
    }
    return true;
}

bool IsValidActionBinding(const InputActionBinding& binding) {
    const auto validKey = [](int key) {
        return key == -1 || (key >= 0 && key < 256);
    };
    return validKey(binding.negativeKey) &&
           std::ranges::all_of(binding.positiveKeys, validKey) &&
           binding.gamepadAxis >= InputActionAxisSource::None &&
           binding.gamepadAxis <= InputActionAxisSource::GamepadRightTrigger &&
           binding.type >= InputActionType::Button &&
           binding.type <= InputActionType::Axis;
}

std::string GenerateActionId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return {};
    }
    std::array<char, 37> text{};
    sprintf_s(text.data(), text.size(),
              "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
              guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
              guid.Data4[6], guid.Data4[7]);
    return text.data();
}

float NormalizeThumbAxis(SHORT value, SHORT deadZone) {
    const int intValue = static_cast<int>(value);
    const int absValue = std::abs(intValue);
    if (absValue <= deadZone) {
        return 0.0f;
    }

    const int maxValue = intValue < 0 ? 32768 : 32767;
    const float normalized =
        static_cast<float>(absValue - deadZone) / static_cast<float>(maxValue - deadZone);
    return std::clamp(normalized, 0.0f, 1.0f) * (intValue < 0 ? -1.0f : 1.0f);
}

float NormalizeTrigger(BYTE value) {
    if (value <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        return 0.0f;
    }

    constexpr float maxValue = 255.0f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    return std::clamp(static_cast<float>(value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) / maxValue, 0.0f,
                      1.0f);
}

std::wstring GetDefaultReplayDirectory() {
    std::array<wchar_t, MAX_PATH> pathBuffer{};
    const DWORD length =
        GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (length == 0 || length >= pathBuffer.size()) {
        return L"replays";
    }

    try {
        const std::filesystem::path executablePath(std::wstring(pathBuffer.data(), length));
        return (executablePath.parent_path() / L"replays").wstring();
    } catch (const std::exception&) {
        return L"replays";
    }
}

} // namespace

Input::Input() : state_(std::make_unique<State>()) {
    ResetDefaultActionBindings();
}

Input::~Input() {
    FinishRecording();
}

bool Input::SetActionBinding(std::string name,
                             const InputActionBinding& binding) {
    if (!IsValidActionName(name) || !IsValidActionBinding(binding)) {
        return false;
    }
    const auto found = std::ranges::find_if(
        state_->actionBindings,
        [&name](const auto& action) { return action.name == name; });
    if (found != state_->actionBindings.end()) {
        found->binding = binding;
        return true;
    }
    return SetActionBinding(std::move(name), binding, GenerateActionId());
}

bool Input::SetActionBinding(std::string name,
                             const InputActionBinding& binding,
                             std::string id) {
    if (!IsValidActionName(name) || !IsValidActionBinding(binding) ||
        !IsValidActionId(id)) {
        return false;
    }
    const bool duplicate = std::ranges::any_of(
        state_->actionBindings, [&name, &id](const auto& action) {
            return action.name == name || action.id == id ||
                   action.id == name || action.name == id;
        });
    if (duplicate) {
        return false;
    }
    try {
        state_->actionBindings.push_back(
            {std::move(id), std::move(name), binding});
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool Input::RenameActionBinding(std::string_view oldName, std::string newName) {
    if (!IsValidActionName(newName)) {
        return false;
    }
    const auto source = std::ranges::find_if(
        state_->actionBindings,
        [oldName](const auto& action) {
            return action.name == oldName || action.id == oldName;
        });
    if (source == state_->actionBindings.end()) {
        return false;
    }
    if (source->name == newName) {
        return true;
    }
    const auto duplicate = std::ranges::find_if(
        state_->actionBindings,
        [&newName](const auto& action) {
            return action.name == newName || action.id == newName;
        });
    if (duplicate != state_->actionBindings.end()) {
        return false;
    }
    source->name = std::move(newName);
    return true;
}

bool Input::RemoveActionBinding(std::string_view name) {
    const auto found = std::ranges::find_if(
        state_->actionBindings,
        [name](const auto& action) {
            return action.name == name || action.id == name;
        });
    if (found == state_->actionBindings.end()) {
        return false;
    }
    state_->actionBindings.erase(found);
    return true;
}

void Input::ClearActionBindings() {
    state_->actionBindings.clear();
}

void Input::ResetDefaultActionBindings() {
    ClearActionBindings();
    (void)SetActionBinding(
        "MoveHorizontal",
        {DIK_A, {DIK_D, -1}, 0, InputActionAxisSource::GamepadLeftX,
         InputActionType::Axis},
        "00000000-0000-4000-8000-000000000001");
    (void)SetActionBinding(
        "MoveVertical",
        {DIK_S, {DIK_W, -1}, 0, InputActionAxisSource::GamepadLeftY,
         InputActionType::Axis},
        "00000000-0000-4000-8000-000000000002");
    (void)SetActionBinding(
        "Sprint",
        {-1, {DIK_LSHIFT, DIK_RSHIFT}, XINPUT_GAMEPAD_LEFT_THUMB,
         InputActionAxisSource::None},
        "00000000-0000-4000-8000-000000000003");
    (void)SetActionBinding(
        "Jump",
        {-1, {DIK_SPACE, -1}, XINPUT_GAMEPAD_A,
         InputActionAxisSource::None},
        "00000000-0000-4000-8000-000000000004");
}

std::vector<std::string> Input::GetActionNames() const {
    std::vector<std::string> names;
    try {
        names.reserve(state_->actionBindings.size());
        for (const auto& action : state_->actionBindings) {
            names.push_back(action.name);
        }
    } catch (const std::exception&) {
        names.clear();
    }
    return names;
}

const InputActionBinding* Input::GetActionBinding(std::string_view name) const {
    const auto found = std::ranges::find_if(
        state_->actionBindings,
        [name](const auto& action) {
            return action.name == name || action.id == name;
        });
    return found == state_->actionBindings.end() ? nullptr : &found->binding;
}

std::string Input::GetActionId(std::string_view nameOrId) const {
    const auto found = std::ranges::find_if(
        state_->actionBindings,
        [nameOrId](const auto& action) {
            return action.name == nameOrId || action.id == nameOrId;
        });
    return found == state_->actionBindings.end() ? std::string{} : found->id;
}

std::string Input::GetActionName(std::string_view nameOrId) const {
    const auto found = std::ranges::find_if(
        state_->actionBindings,
        [nameOrId](const auto& action) {
            return action.name == nameOrId || action.id == nameOrId;
        });
    return found == state_->actionBindings.end() ? std::string{} : found->name;
}

float Input::GetActionGamepadAxis(InputActionAxisSource source) const {
    switch (source) {
    case InputActionAxisSource::None:
        return 0.0f;
    case InputActionAxisSource::GamepadLeftX:
        return GetGamepadLeftStickX();
    case InputActionAxisSource::GamepadLeftY:
        return GetGamepadLeftStickY();
    case InputActionAxisSource::GamepadRightX:
        return GetGamepadRightStickX();
    case InputActionAxisSource::GamepadRightY:
        return GetGamepadRightStickY();
    case InputActionAxisSource::GamepadLeftTrigger:
        return GetGamepadLeftTrigger();
    case InputActionAxisSource::GamepadRightTrigger:
        return GetGamepadRightTrigger();
    }
    return 0.0f;
}

float Input::GetActionAxis(std::string_view name) const {
    const InputActionBinding* binding = GetActionBinding(name);
    if (binding == nullptr || binding->type != InputActionType::Axis) {
        return 0.0f;
    }
    float value = GetActionGamepadAxis(binding->gamepadAxis);
    if (binding->negativeKey >= 0 && IsKeyPress(binding->negativeKey)) {
        value -= 1.0f;
    }
    for (int key : binding->positiveKeys) {
        if (key >= 0 && IsKeyPress(key)) {
            value += 1.0f;
        }
    }
    return std::clamp(value, -1.0f, 1.0f);
}

bool Input::EvaluateActionButton(const InputActionBinding& binding,
                                 bool previous) const {
    if (state_->keyboardQueryEnabled) {
        const auto& keys = previous ? state_->keyPrev : state_->keyNow;
        for (int key : binding.positiveKeys) {
            if (key >= 0 && std::cmp_less(key, keys.size()) &&
                (keys[static_cast<size_t>(key)] & kPressMask) != 0) {
                return true;
            }
        }
    }
    const bool connected = previous ? state_->gamepadPrevConnected
                                    : state_->gamepadConnected;
    if (state_->gamepadQueryEnabled && connected &&
        binding.gamepadButton != 0) {
        const XINPUT_STATE& gamepad =
            previous ? state_->gamepadPrevState : state_->gamepadState;
        return (gamepad.Gamepad.wButtons & binding.gamepadButton) != 0;
    }
    return false;
}

bool Input::IsActionPressed(std::string_view name) const {
    const InputActionBinding* binding = GetActionBinding(name);
    return binding != nullptr && binding->type == InputActionType::Button &&
           EvaluateActionButton(*binding, false);
}

bool Input::IsActionTriggered(std::string_view name) const {
    const InputActionBinding* binding = GetActionBinding(name);
    return binding != nullptr && binding->type == InputActionType::Button &&
           EvaluateActionButton(*binding, false) &&
           !EvaluateActionButton(*binding, true);
}

bool Input::IsActionReleased(std::string_view name) const {
    const InputActionBinding* binding = GetActionBinding(name);
    return binding != nullptr && binding->type == InputActionType::Button &&
           !EvaluateActionButton(*binding, false) &&
           EvaluateActionButton(*binding, true);
}

Input::ReplayMode Input::GetReplayMode() const {
    return state_->replayMode;
}

bool Input::IsReplayFinished() const {
    return state_->replayFinished;
}

size_t Input::GetReplayFrameIndex() const {
    return state_->replayFrameIndex;
}

size_t Input::GetReplayFrameCount() const {
    return state_->replayFrames.size();
}

const std::wstring& Input::GetReplayPath() const {
    return state_->replayPath;
}

long Input::GetMouseDX() const {
    return state_->mouseQueryEnabled ? state_->mouseState.lX : 0L;
}

long Input::GetMouseDY() const {
    return state_->mouseQueryEnabled ? state_->mouseState.lY : 0L;
}

long Input::GetMouseWheel() const {
    return state_->mouseQueryEnabled ? state_->mouseState.lZ : 0L;
}

bool Input::IsGamepadConnected() const {
    return state_->gamepadQueryEnabled && state_->gamepadConnected;
}

float Input::GetGamepadLeftStickX() const {
    return state_->gamepadQueryEnabled ? state_->gamepadLeftStickX : 0.0f;
}

float Input::GetGamepadLeftStickY() const {
    return state_->gamepadQueryEnabled ? state_->gamepadLeftStickY : 0.0f;
}

float Input::GetGamepadRightStickX() const {
    return state_->gamepadQueryEnabled ? state_->gamepadRightStickX : 0.0f;
}

float Input::GetGamepadRightStickY() const {
    return state_->gamepadQueryEnabled ? state_->gamepadRightStickY : 0.0f;
}

float Input::GetGamepadLeftTrigger() const {
    return state_->gamepadQueryEnabled ? state_->gamepadLeftTrigger : 0.0f;
}

float Input::GetGamepadRightTrigger() const {
    return state_->gamepadQueryEnabled ? state_->gamepadRightTrigger : 0.0f;
}

void Input::SetQueryEnabled(bool keyboardEnabled, bool mouseEnabled, bool gamepadEnabled) {
    state_->keyboardQueryEnabled = keyboardEnabled;
    state_->mouseQueryEnabled = mouseEnabled;
    state_->gamepadQueryEnabled = gamepadEnabled;
}

void Input::ClearInputState(bool clearPrevious) {
    state_->keyNow.fill(0);
    state_->mouseState = {};
    state_->gamepadState = {};
    state_->gamepadConnected = false;
    state_->gamepadLeftStickX = 0.0f;
    state_->gamepadLeftStickY = 0.0f;
    state_->gamepadRightStickX = 0.0f;
    state_->gamepadRightStickY = 0.0f;
    state_->gamepadLeftTrigger = 0.0f;
    state_->gamepadRightTrigger = 0.0f;

    if (clearPrevious) {
        state_->keyPrev.fill(0);
        state_->mousePrevState = {};
        state_->gamepadPrevState = {};
        state_->gamepadPrevConnected = false;
    }
}

void Input::Initialize(HINSTANCE hInstance, HWND hwnd) {
    if (state_->replayDirectory.empty()) {
        state_->replayDirectory = GetDefaultReplayDirectory();
    }

    state_->directInput.Reset();
    state_->keyboard.Reset();
    state_->mouse.Reset();
    ClearInputState(true);

    HRESULT hr =
        DirectInput8Create(hInstance, DIRECTINPUT_VERSION, IID_IDirectInput8,
                           reinterpret_cast<void**>(state_->directInput.GetAddressOf()), nullptr);
    if (FAILED(hr) || !state_->directInput) {
        return;
    }

    hr = state_->directInput->CreateDevice(GUID_SysKeyboard, state_->keyboard.GetAddressOf(),
                                           nullptr);
    if (SUCCEEDED(hr) && state_->keyboard) {
        hr = state_->keyboard->SetDataFormat(&c_dfDIKeyboard);
        if (SUCCEEDED(hr)) {
            hr = state_->keyboard->SetCooperativeLevel(hwnd, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
        }
        if (SUCCEEDED(hr)) {
            state_->keyboard->Acquire();
        } else {
            state_->keyboard.Reset();
        }
    } else {
        state_->keyboard.Reset();
    }

    hr = state_->directInput->CreateDevice(GUID_SysMouse, state_->mouse.GetAddressOf(), nullptr);
    if (SUCCEEDED(hr) && state_->mouse) {
        hr = state_->mouse->SetDataFormat(&c_dfDIMouse);
        if (SUCCEEDED(hr)) {
            hr = state_->mouse->SetCooperativeLevel(hwnd, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
        }
        if (SUCCEEDED(hr)) {
            state_->mouse->Acquire();
        } else {
            state_->mouse.Reset();
        }
    } else {
        state_->mouse.Reset();
    }
}

void Input::Update(float deltaTime) {
    (void)deltaTime;

    if (state_->replayMode == ReplayMode::Replay) {
        state_->keyPrev = state_->keyNow;
        state_->mousePrevState = state_->mouseState;
        state_->gamepadPrevConnected = state_->gamepadConnected;
        state_->gamepadPrevState = state_->gamepadState;

        if (state_->replayFrameIndex < state_->replayFrames.size()) {
            ApplyReplayFrame(state_->replayFrames[state_->replayFrameIndex]);
            ++state_->replayFrameIndex;
            state_->replayFinished = state_->replayFrameIndex >= state_->replayFrames.size();
        } else {
            state_->replayFinished = true;
            ClearInputState(false);
        }
        return;
    }

    UpdateKeyboard();
    UpdateMouse();
    UpdateGamepad();

    if (state_->replayMode == ReplayMode::Record &&
        state_->recordedFrames.size() < InputReplayLimits::kMaxFrames) {
        try {
            state_->recordedFrames.push_back(CaptureFrame());
            state_->recordingDirty = true;
        } catch (const std::exception&) {
        }
    }
}

void Input::UpdateKeyboard() {
    state_->keyPrev = state_->keyNow;
    if (!state_->keyboard) {
        state_->keyNow.fill(0);
        return;
    }

    HRESULT hr = state_->keyboard->GetDeviceState(256, state_->keyNow.data());

    if (FAILED(hr)) {
        hr = state_->keyboard->Acquire();

        if (SUCCEEDED(hr)) {
            hr = state_->keyboard->GetDeviceState(256, state_->keyNow.data());
        }
        if (FAILED(hr)) {
            state_->keyNow.fill(0);
        }
    }
}

void Input::UpdateMouse() {
    state_->mousePrevState = state_->mouseState;
    if (!state_->mouse) {
        state_->mouseState = {};
        return;
    }

    HRESULT hr = state_->mouse->GetDeviceState(sizeof(DIMOUSESTATE), &state_->mouseState);

    if (FAILED(hr)) {
        hr = state_->mouse->Acquire();

        if (SUCCEEDED(hr)) {
            hr = state_->mouse->GetDeviceState(sizeof(DIMOUSESTATE), &state_->mouseState);
        }
        if (FAILED(hr)) {
            state_->mouseState = {};
        }
    }
}

void Input::UpdateGamepad() {
    state_->gamepadPrevState = state_->gamepadState;
    state_->gamepadPrevConnected = state_->gamepadConnected;
    ZeroMemory(&state_->gamepadState, sizeof(XINPUT_STATE));

    const DWORD result = XInputGetState(0, &state_->gamepadState);
    state_->gamepadConnected = result == ERROR_SUCCESS;

    if (!state_->gamepadConnected) {
        state_->gamepadLeftStickX = 0.0f;
        state_->gamepadLeftStickY = 0.0f;
        state_->gamepadRightStickX = 0.0f;
        state_->gamepadRightStickY = 0.0f;
        state_->gamepadLeftTrigger = 0.0f;
        state_->gamepadRightTrigger = 0.0f;
        return;
    }

    const XINPUT_GAMEPAD& pad = state_->gamepadState.Gamepad;
    state_->gamepadLeftStickX =
        NormalizeThumbAxis(pad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    state_->gamepadLeftStickY =
        NormalizeThumbAxis(pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    state_->gamepadRightStickX =
        NormalizeThumbAxis(pad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    state_->gamepadRightStickY =
        NormalizeThumbAxis(pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    state_->gamepadLeftTrigger = NormalizeTrigger(pad.bLeftTrigger);
    state_->gamepadRightTrigger = NormalizeTrigger(pad.bRightTrigger);
}

bool Input::IsKeyPress(int dik) const {
    if (!state_->keyboardQueryEnabled || dik < 0 ||
        std::cmp_greater_equal(dik, state_->keyNow.size())) {
        return false;
    }
    return (state_->keyNow[dik] & kPressMask) != 0;
}

bool Input::IsKeyTrigger(int dik) const {
    if (!state_->keyboardQueryEnabled || dik < 0 ||
        std::cmp_greater_equal(dik, state_->keyNow.size())) {
        return false;
    }
    return (state_->keyNow[dik] & kPressMask) && !(state_->keyPrev[dik] & kPressMask);
}

bool Input::IsKeyRelease(int dik) const {
    if (!state_->keyboardQueryEnabled || dik < 0 ||
        std::cmp_greater_equal(dik, state_->keyNow.size())) {
        return false;
    }
    return !(state_->keyNow[dik] & kPressMask) && (state_->keyPrev[dik] & kPressMask);
}

bool Input::IsMousePress(int button) const {
    if (!state_->mouseQueryEnabled || button < 0 ||
        std::cmp_greater_equal(button, _countof(state_->mouseState.rgbButtons))) {
        return false;
    }
    return (state_->mouseState.rgbButtons[button] & 0x80) != 0;
}

bool Input::IsMouseTrigger(int button) const {
    if (!state_->mouseQueryEnabled || button < 0 ||
        std::cmp_greater_equal(button, _countof(state_->mouseState.rgbButtons))) {
        return false;
    }
    return (state_->mouseState.rgbButtons[button] & 0x80) &&
           !(state_->mousePrevState.rgbButtons[button] & 0x80);
}

bool Input::IsMouseRelease(int button) const {
    if (!state_->mouseQueryEnabled || button < 0 ||
        std::cmp_greater_equal(button, _countof(state_->mouseState.rgbButtons))) {
        return false;
    }
    return !(state_->mouseState.rgbButtons[button] & 0x80) &&
           (state_->mousePrevState.rgbButtons[button] & 0x80);
}

bool Input::IsGamepadButtonPress(WORD button) const {
    return state_->gamepadQueryEnabled && state_->gamepadConnected &&
           (state_->gamepadState.Gamepad.wButtons & button) != 0;
}

bool Input::IsGamepadButtonTrigger(WORD button) const {
    return state_->gamepadQueryEnabled && state_->gamepadConnected &&
           (state_->gamepadState.Gamepad.wButtons & button) != 0 &&
           (state_->gamepadPrevState.Gamepad.wButtons & button) == 0;
}

bool Input::IsGamepadButtonRelease(WORD button) const {
    return state_->gamepadQueryEnabled && state_->gamepadPrevConnected &&
           (state_->gamepadState.Gamepad.wButtons & button) == 0 &&
           (state_->gamepadPrevState.Gamepad.wButtons & button) != 0;
}

bool Input::IsGamepadLeftTriggerTrigger(float threshold) const {
    return state_->gamepadQueryEnabled && state_->gamepadConnected &&
           state_->gamepadLeftTrigger > threshold &&
           NormalizeTrigger(state_->gamepadPrevState.Gamepad.bLeftTrigger) <= threshold;
}

bool Input::IsGamepadRightTriggerTrigger(float threshold) const {
    return state_->gamepadQueryEnabled && state_->gamepadConnected &&
           state_->gamepadRightTrigger > threshold &&
           NormalizeTrigger(state_->gamepadPrevState.Gamepad.bRightTrigger) <= threshold;
}
