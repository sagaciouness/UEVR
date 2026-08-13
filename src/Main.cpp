// dllmain
#include <windows.h>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <utility/Thread.hpp>
#include <utility/NrcDebug.hpp>

#include "Framework.hpp"

namespace {
void startup_impl(HMODULE module) {
    nrc_debug::log("RUNTIME_INIT", "Framework construction begin");
    try {
        g_framework = std::make_unique<Framework>(module);
        nrc_debug::log("RUNTIME_INIT", "Framework construction completed");
    } catch (const std::exception& exception) {
        nrc_debug::log("RUNTIME_INIT", std::string{"C++ exception: "} + exception.what());
    } catch (...) {
        nrc_debug::log("RUNTIME_INIT", "Unknown C++/SEH exception");
    }
}

DWORD WINAPI startup_thread(LPVOID module) {
    // The target is compiled with /EHa, so startup_impl's catch-all also receives SEH faults.
    startup_impl(static_cast<HMODULE>(module));
    return 0;
}
}

BOOL APIENTRY DllMain(HANDLE handle, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(static_cast<HMODULE>(handle));
        nrc_debug::log("DLL_ATTACH", "UEVRBackend.dll attached");
        const auto thread = CreateThread(nullptr, 0, startup_thread, handle, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            nrc_debug::log("DLL_ATTACH", "CreateThread failed code=" + std::to_string(GetLastError()));
        }
    }

    return TRUE;
}
