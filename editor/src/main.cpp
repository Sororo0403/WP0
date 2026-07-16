#include "EditorScene.h"

#include "core/EngineRuntime.h"

#include <Windows.h>
#include <memory>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLine,
                   int showCommand) {
    (void)previousInstance;
    (void)commandLine;

    EngineRuntimeConfig config{};
    config.width = 1600;
    config.height = 900;
    config.title = L"WP0 Editor";
    config.cursorVisible = true;
    config.logPath = L"generated/logs/editor.log";

    EngineRuntime runtime;
    if (!runtime.Initialize(instance, showCommand, config)) {
        return -1;
    }
    if (!runtime.SetScene(
            std::make_unique<EditorScene>([&runtime]() { runtime.RequestClose(); }))) {
        return -1;
    }

    for (;;) {
        const EngineFrameResult result = runtime.Tick();
        if (result == EngineFrameResult::ExitRequested) {
            return 0;
        }
        if (result == EngineFrameResult::Failed) {
            return -1;
        }
    }
}
