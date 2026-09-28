#include "Renderer.h"

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace
{
Renderer* gRenderer = nullptr;

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_SIZE:
        if (gRenderer && wParam != SIZE_MINIMIZED)
            gRenderer->Resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(window, message, wParam, lParam);
    }
}

bool HasCommandLineSwitch(const wchar_t* expected)
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return false;
    bool found = false;
    for (int i = 1; i < count; ++i)
        found |= _wcsicmp(arguments[i], expected) == 0;
    LocalFree(arguments);
    return found;
}

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    const bool smokeTest = HasCommandLineSwitch(L"--smoke-test");
    try
    {
        const wchar_t* className = L"GpuParticleLabWindow";
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = className;
        if (!RegisterClassExW(&windowClass))
            throw std::runtime_error("RegisterClassExW failed");

        RECT clientArea{0, 0, 1280, 720};
        AdjustWindowRect(&clientArea, WS_OVERLAPPEDWINDOW, FALSE);
        HWND window = CreateWindowExW(0, className, L"GPU Particle Lab - loading Sponza...",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                      clientArea.right - clientArea.left, clientArea.bottom - clientArea.top,
                                      nullptr, nullptr, instance, nullptr);
        if (!window)
            throw std::runtime_error("CreateWindowExW failed");

        if (!smokeTest)
        {
            ShowWindow(window, showCommand);
            UpdateWindow(window);
        }

        Renderer renderer;
        renderer.Initialize(window, 1280, 720, ExecutableDirectory());
        gRenderer = &renderer;

        auto previousTime = std::chrono::steady_clock::now();
        auto titleTime = previousTime;
        uint32_t framesSinceTitle = 0;
        uint32_t smokeFrames = 0;
        MSG message{};
        while (message.message != WM_QUIT)
        {
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }

            const auto currentTime = std::chrono::steady_clock::now();
            const float deltaSeconds = std::chrono::duration<float>(currentTime - previousTime).count();
            previousTime = currentTime;

            float forward = 0.0f;
            float right = 0.0f;
            float up = 0.0f;
            if (GetAsyncKeyState('W') & 0x8000) forward += 1.0f;
            if (GetAsyncKeyState('S') & 0x8000) forward -= 1.0f;
            if (GetAsyncKeyState('D') & 0x8000) right += 1.0f;
            if (GetAsyncKeyState('A') & 0x8000) right -= 1.0f;
            if (GetAsyncKeyState('E') & 0x8000) up += 1.0f;
            if (GetAsyncKeyState('Q') & 0x8000) up -= 1.0f;
            renderer.MoveCamera(forward, right, up, deltaSeconds);

            const float rotationSpeed = 1.1f * deltaSeconds;
            if (GetAsyncKeyState(VK_LEFT) & 0x8000) renderer.RotateCamera(-rotationSpeed, 0.0f);
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) renderer.RotateCamera(rotationSpeed, 0.0f);
            if (GetAsyncKeyState(VK_UP) & 0x8000) renderer.RotateCamera(0.0f, rotationSpeed);
            if (GetAsyncKeyState(VK_DOWN) & 0x8000) renderer.RotateCamera(0.0f, -rotationSpeed);
            if (GetAsyncKeyState(VK_SPACE) & 1) renderer.SetPaused(!renderer.IsPaused());
            if (GetAsyncKeyState('R') & 1) renderer.ResetParticles();

            renderer.Update(deltaSeconds);
            renderer.Render();
            ++framesSinceTitle;

            const float titleInterval = std::chrono::duration<float>(currentTime - titleTime).count();
            if (!smokeTest && titleInterval >= 1.0f)
            {
                const int fps = static_cast<int>(framesSinceTitle / titleInterval + 0.5f);
                const std::wstring title = L"GPU Particle Lab | " + std::to_wstring(renderer.ParticleCount()) +
                    L" opaque particles | " + std::to_wstring(fps) + L" FPS | WASD/QE, arrows, Space, R";
                SetWindowTextW(window, title.c_str());
                titleTime = currentTime;
                framesSinceTitle = 0;
            }

            if (smokeTest && ++smokeFrames >= 3)
                DestroyWindow(window);
        }

        gRenderer = nullptr;
        return 0;
    }
    catch (const std::exception& error)
    {
        if (smokeTest)
        {
            std::ofstream report(ExecutableDirectory() / "smoke-test-error.txt", std::ios::trunc);
            report << error.what();
        }
        else
        {
            MessageBoxA(nullptr, error.what(), "ParticleLab error", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
