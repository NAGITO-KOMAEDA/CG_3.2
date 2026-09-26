#include "RenderingSystem.hpp"

#include <cwchar>
#include <exception>

int main()
{
    try
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const PWSTR commandLine = GetCommandLineW();
        const int showCommand = SW_SHOWDEFAULT;
        const bool smokeTest = commandLine && std::wcsstr(commandLine, L"--smoke-test") != nullptr;
        RenderingSystem application(instance, showCommand, smokeTest);
        return application.Run();
    }
    catch (const std::exception& exception)
    {
        MessageBoxA(nullptr, exception.what(), "Deferred Sponza - fatal error", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
}
