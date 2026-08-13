#pragma once

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nrc_debug {
inline constexpr wchar_t LOG_DIRECTORY[] = L"C:\\UEVR_NRC_DEBUG";
inline constexpr wchar_t LOG_PATH[] = L"C:\\UEVR_NRC_DEBUG\\uevr_nrc_debug.log";

inline bool is_target_process() noexcept {
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }

    const auto separator = std::wcsrchr(path, L'\\');
    const auto filename = separator != nullptr ? separator + 1 : path;
    return _wcsicmp(filename, L"NRC-Win64-Shipping.exe") == 0;
}

inline void log(std::string_view stage, std::string_view message) noexcept {
    if (!is_target_process()) {
        return;
    }

    try {
        CreateDirectoryW(LOG_DIRECTORY, nullptr);
        const auto file = CreateFileW(
            LOG_PATH,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE) {
            return;
        }

        SYSTEMTIME now{};
        GetLocalTime(&now);

        char prefix[192]{};
        const auto prefix_length = std::snprintf(
            prefix,
            sizeof(prefix),
            "%04u-%02u-%02u %02u:%02u:%02u.%03u [T%lu] [%.*s] ",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            now.wMilliseconds,
            GetCurrentThreadId(),
            static_cast<int>(stage.size()),
            stage.data());

        if (prefix_length > 0) {
            DWORD written{};
            WriteFile(file, prefix, static_cast<DWORD>(prefix_length), &written, nullptr);
            WriteFile(file, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
            constexpr char newline[] = "\r\n";
            WriteFile(file, newline, 2, &written, nullptr);
            FlushFileBuffers(file);
        }

        CloseHandle(file);
    } catch (...) {
        // Diagnostic logging must never take the game down.
    }
}

inline std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

inline std::vector<std::filesystem::path> config_paths() {
    std::vector<std::filesystem::path> paths{
        L"C:\\UEVR_NRC_DEBUG\\injector_config.txt",
        L"C:\\UEVR_NRC_DEBUG\\config.txt",
    };

    wchar_t appdata[32768]{};
    size_t appdata_length{};
    if (_wgetenv_s(&appdata_length, appdata, L"APPDATA") == 0 && appdata_length > 1) {
        const auto profile = std::filesystem::path{appdata} / L"UnrealVRMod" / L"NRC-Win64-Shipping";
        paths.emplace_back(profile / L"injector_config.txt");
        paths.emplace_back(profile / L"config.txt");
    }

    wchar_t module_path[32768]{};
    const auto module_length = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
    if (module_length > 0 && module_length < std::size(module_path)) {
        const auto directory = std::filesystem::path{module_path}.parent_path();
        paths.emplace_back(directory / L"injector_config.txt");
        paths.emplace_back(directory / L"config.txt");
    }

    return paths;
}

inline std::optional<std::string> read_setting(std::string_view key) {
    for (const auto& path : config_paths()) {
        std::ifstream input{path};
        if (!input) {
            continue;
        }

        std::string line{};
        while (std::getline(input, line)) {
            const auto separator = line.find('=');
            if (separator == std::string::npos) {
                continue;
            }

            if (trim(line.substr(0, separator)) == key) {
                return trim(line.substr(separator + 1));
            }
        }
    }

    return std::nullopt;
}

inline bool read_bool(std::string_view key, bool fallback) {
    const auto value = read_setting(key);
    if (!value) {
        return fallback;
    }

    auto normalized = *value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

inline bool disable_ue_hooks() {
    return is_target_process() && read_bool("NRCDisableUEHooks", true);
}

inline bool detection_only() {
    return is_target_process() && read_bool("NRCDetectionOnly", false);
}

inline bool disable_d3d_rehook() {
    return is_target_process() && read_bool("NRCDisableD3DRehook", true);
}

inline bool disable_message_hook_reinit() {
    return is_target_process() && read_bool("NRCDisableMessageHookReinit", true);
}

inline bool skip_initialize_hmd_device() {
    return is_target_process() && read_bool("NRCSkipInitializeHMDDevice", true);
}

inline bool replace_existing_stereo_device() {
    return is_target_process() && read_bool("NRCReplaceExistingStereoDevice", false);
}

inline std::optional<std::string> engine_version_override() {
    if (!is_target_process()) {
        return std::nullopt;
    }

    return read_setting("EngineVersionOverride");
}

inline std::string pointer(uintptr_t value) {
    char output[32]{};
    std::snprintf(output, sizeof(output), "0x%llX", static_cast<unsigned long long>(value));
    return output;
}

inline void log_configuration() {
    const auto version = engine_version_override().value_or("<auto>");
    log(
        "CONFIG",
        "EngineVersionOverride=" + version
            + " NRCDisableUEHooks=" + (disable_ue_hooks() ? "true" : "false")
            + " NRCDetectionOnly=" + (detection_only() ? "true" : "false")
            + " NRCDisableD3DRehook=" + (disable_d3d_rehook() ? "true" : "false")
            + " NRCDisableMessageHookReinit=" + (disable_message_hook_reinit() ? "true" : "false")
            + " NRCSkipInitializeHMDDevice=" + (skip_initialize_hmd_device() ? "true" : "false")
            + " NRCReplaceExistingStereoDevice=" + (replace_existing_stereo_device() ? "true" : "false"));
}
}
