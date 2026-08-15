#include "Localization.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <format>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>
#include <utility/String.hpp>
#include <imgui.h>

#include <sdk/CVar.hpp>
#include <sdk/Globals.hpp>

#include "Framework.hpp"
#include "utility/NrcDebug.hpp"

#include "../../VR.hpp"
#include "OpenXR.hpp"

using namespace nlohmann;

namespace runtimes {
namespace {
void log_desktop_spectator(std::string_view message) {
    spdlog::info("[VR][CinematicSpectator] {}", message);
    nrc_debug::log("CINEMATIC_SPECTATOR", message);
}

constexpr float MIN_DESKTOP_SPECTATOR_FOV = 80.0f;
constexpr float MAX_DESKTOP_SPECTATOR_FOV = 150.0f;
constexpr float RADIANS_TO_DEGREES = 57.29577951308232f;

float horizontal_fov_degrees(const std::array<float, 4>& fov) {
    return (std::atan(fov[1]) - std::atan(fov[0])) * RADIANS_TO_DEGREES;
}

std::optional<float> solve_projection_scale(
    const std::array<float, 4>& base_fov, float target_degrees)
{
    if (!std::isfinite(target_degrees) ||
        target_degrees <= 0.0f || target_degrees >= 179.0f)
    {
        return std::nullopt;
    }

    const auto center = (base_fov[0] + base_fov[1]) * 0.5f;
    const auto half_span = (base_fov[1] - base_fov[0]) * 0.5f;
    if (!std::isfinite(center) || !std::isfinite(half_span) || half_span <= 0.0f) {
        return std::nullopt;
    }

    const auto angle_for_scale = [center, half_span](float scale) {
        return (std::atan(center + half_span * scale) -
                std::atan(center - half_span * scale)) * RADIANS_TO_DEGREES;
    };

    float low = 0.0f;
    float high = 1.0f;
    while (angle_for_scale(high) < target_degrees && high < 1024.0f) {
        high *= 2.0f;
    }

    if (!std::isfinite(angle_for_scale(high)) || angle_for_scale(high) < target_degrees) {
        return std::nullopt;
    }

    for (uint32_t i = 0; i < 48; ++i) {
        const auto middle = (low + high) * 0.5f;
        if (angle_for_scale(middle) < target_degrees) {
            low = middle;
        } else {
            high = middle;
        }
    }

    return (low + high) * 0.5f;
}

}

std::string_view OpenXR::get_desktop_spectator_status() const {
    switch (desktop_spectator.status) {
    case DesktopSpectatorStatus::WaitingForFov:
        return "Waiting for OpenXR FOV";
    case DesktopSpectatorStatus::Active:
        return "Active";
    case DesktopSpectatorStatus::Fallback:
        return "Fallback";
    default:
        return "Off";
    }
}

std::string_view OpenXR::get_desktop_spectator_fov_mode() const {
    switch (desktop_spectator.fov_mode) {
    case DesktopSpectatorFovMode::Cropped:
        return "Cropped";
    case DesktopSpectatorFovMode::Expanded:
        return "Expanded";
    default:
        return "Automatic";
    }
}


void OpenXR::configure_desktop_spectator(
    bool requested, uint32_t eye, float aspect, bool d3d11, bool extreme, bool using_2d_screen,
    bool fov_override_enabled, float horizontal_fov)
{
    desktop_spectator = {};
    desktop_spectator.requested = requested;
    desktop_spectator.eye = eye > 1 ? 1 : eye;
    desktop_spectator.aspect = aspect;
    desktop_spectator.effective_aspect = aspect;
    desktop_spectator.extreme = extreme;
    desktop_spectator.fov_override_enabled = fov_override_enabled;
    desktop_spectator.target_horizontal_fov = std::isfinite(horizontal_fov)
        ? std::clamp(horizontal_fov, MIN_DESKTOP_SPECTATOR_FOV, MAX_DESKTOP_SPECTATOR_FOV)
        : 105.0f;
    pending_desktop_spectator_fov_enabled.store(fov_override_enabled, std::memory_order_release);
    pending_desktop_spectator_horizontal_fov.store(
        desktop_spectator.target_horizontal_fov, std::memory_order_release);
    desktop_spectator_fov_update_pending.store(false, std::memory_order_release);
    desktop_spectator_revision.store(0, std::memory_order_release);

    log_desktop_spectator(std::format(
        "Session config requested={} eye={} aspect={} renderer={} path={} meta={} fov_override={} target_fov={}",
        requested,
        desktop_spectator.eye == 0 ? "left" : "right",
        aspect,
        d3d11 ? "D3D11" : "D3D12",
        extreme ? "Extreme/AFR" : "standard",
        is_meta_openxr_system,
        fov_override_enabled,
        desktop_spectator.target_horizontal_fov));

    if (!requested) {
        return;
    }

    desktop_spectator.status = DesktopSpectatorStatus::WaitingForFov;
    view_bounds[0][0] = view_bounds[1][0] = 0.0f;
    view_bounds[0][1] = view_bounds[1][1] = 1.0f;
    view_bounds[0][2] = view_bounds[1][2] = 0.0f;
    view_bounds[0][3] = view_bounds[1][3] = 1.0f;
    should_recalculate_eye_projections = true;

    if (!d3d11) {
        fallback_desktop_spectator("Cinematic spectator currently supports D3D11 only", false);
    } else if (using_2d_screen) {
        fallback_desktop_spectator("Cinematic spectator is unavailable in 2D Screen Mode", false);
    } else if (!std::isfinite(aspect) || aspect < 1.0f || aspect > 3.0f) {
        fallback_desktop_spectator(std::format("Invalid spectator aspect {} (expected 1.0 to 3.0)", aspect), false);
    }
}
void OpenXR::request_desktop_spectator_fov(bool enabled, float horizontal_fov) {
    const auto sanitized_fov = std::isfinite(horizontal_fov)
        ? std::clamp(horizontal_fov, MIN_DESKTOP_SPECTATOR_FOV, MAX_DESKTOP_SPECTATOR_FOV)
        : 105.0f;
    const auto current_enabled = pending_desktop_spectator_fov_enabled.load(std::memory_order_acquire);
    const auto current_fov = pending_desktop_spectator_horizontal_fov.load(std::memory_order_acquire);

    if (current_enabled == enabled && std::abs(current_fov - sanitized_fov) < 0.001f) {
        return;
    }

    pending_desktop_spectator_fov_enabled.store(enabled, std::memory_order_release);
    pending_desktop_spectator_horizontal_fov.store(sanitized_fov, std::memory_order_release);
    desktop_spectator_fov_update_pending.store(true, std::memory_order_release);
}


void OpenXR::fallback_desktop_spectator(std::string reason, bool request_render_reset) {
    if (desktop_spectator.status == DesktopSpectatorStatus::Fallback) {
        return;
    }

    const bool dimensions_changed = desktop_spectator.status == DesktopSpectatorStatus::Active && !desktop_spectator.extreme;
    desktop_spectator.status = DesktopSpectatorStatus::Fallback;
    desktop_spectator.fallback_reason = std::move(reason);
    eye_width_adjustment = 1.0f;
    eye_height_adjustment = 1.0f;
    view_bounds[0][0] = view_bounds[1][0] = 0.0f;
    view_bounds[0][1] = view_bounds[1][1] = 1.0f;
    view_bounds[0][2] = view_bounds[1][2] = 0.0f;
    view_bounds[0][3] = view_bounds[1][3] = 1.0f;
    should_recalculate_eye_projections = true;

    log_desktop_spectator(std::format("Fallback to DesktopRecordingFix_V2: {}", desktop_spectator.fallback_reason));

    if (request_render_reset && dimensions_changed) {
        if (const auto vr = VR::get(); vr != nullptr && vr->get_fake_stereo_hook() != nullptr) {
            vr->get_fake_stereo_hook()->set_should_recreate_textures(true);
            vr->reinitialize_renderer();
        }
    }
}

void OpenXR::initialize_desktop_spectator() {
    const bool first_activation = desktop_spectator.status == DesktopSpectatorStatus::WaitingForFov;
    if (!first_activation && desktop_spectator.status != DesktopSpectatorStatus::Active) {
        return;
    }

    if (views.size() < 2 || view_configs.size() < 2) {
        fallback_desktop_spectator("OpenXR did not provide two stereo views", false);
        return;
    }

    if (first_activation) {
        desktop_spectator.original_width = static_cast<uint32_t>(
            static_cast<float>(view_configs[0].recommendedImageRectWidth) * resolution_scale->value());
        desktop_spectator.original_height = static_cast<uint32_t>(
            static_cast<float>(view_configs[0].recommendedImageRectHeight) * resolution_scale->value());

        if (desktop_spectator.original_width == 0 || desktop_spectator.original_height == 0) {
            fallback_desktop_spectator("OpenXR returned a zero-sized recommended eye texture", false);
            return;
        }

        for (uint32_t eye = 0; eye < 2; ++eye) {
            desktop_spectator.raw_fov[eye] = {
                raw_projections[eye][0], raw_projections[eye][1], raw_projections[eye][2], raw_projections[eye][3]};

            for (const auto value : desktop_spectator.raw_fov[eye]) {
                if (!std::isfinite(value)) {
                    fallback_desktop_spectator(std::format("OpenXR eye {} returned a non-finite FOV tangent", eye), false);
                    return;
                }
            }

            const auto& raw = desktop_spectator.raw_fov[eye];
            if (raw[1] <= raw[0] || raw[2] <= raw[3]) {
                fallback_desktop_spectator(std::format("OpenXR eye {} returned an invalid FOV span", eye), false);
                return;
            }
        }
    }

    const auto selected_eye = desktop_spectator.eye;
    const auto& selected_raw = desktop_spectator.raw_fov[selected_eye];
    const auto original_horizontal = selected_raw[1] - selected_raw[0];
    const auto vertical = selected_raw[2] - selected_raw[3];
    const auto target_horizontal = std::max(original_horizontal, vertical * desktop_spectator.aspect);
    auto base_horizontal_scale = target_horizontal / original_horizontal;

    if (!std::isfinite(base_horizontal_scale) || base_horizontal_scale < 1.0f) {
        fallback_desktop_spectator("Calculated horizontal projection scale is invalid", false);
        return;
    }

    if (desktop_spectator.extreme) {
        desktop_spectator.expanded_width = desktop_spectator.original_width;
    } else {
        const auto vr = VR::get();
        const auto texture_width_multiplier = vr != nullptr && vr->is_using_afr() ? 1U : 2U;
        const auto max_eye_width = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / texture_width_multiplier;
        const auto expanded_width = std::ceil(
            static_cast<double>(desktop_spectator.original_width) * static_cast<double>(base_horizontal_scale));

        if (!std::isfinite(expanded_width) || expanded_width < desktop_spectator.original_width ||
            expanded_width > max_eye_width ||
            desktop_spectator.original_height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        {
            fallback_desktop_spectator(std::format(
                "Expanded render target {}x{} exceeds the D3D11 texture limit",
                expanded_width * texture_width_multiplier,
                desktop_spectator.original_height), false);
            return;
        }

        desktop_spectator.expanded_width = static_cast<uint32_t>(expanded_width);
        base_horizontal_scale = static_cast<float>(desktop_spectator.expanded_width) /
            static_cast<float>(desktop_spectator.original_width);
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto& raw = desktop_spectator.raw_fov[eye];
        const auto center = (raw[0] + raw[1]) * 0.5f;
        const auto expanded_span = (raw[1] - raw[0]) * base_horizontal_scale;
        const auto expanded_left = center - expanded_span * 0.5f;
        const auto expanded_right = center + expanded_span * 0.5f;

        if (!std::isfinite(expanded_span) || !std::isfinite(expanded_left) || !std::isfinite(expanded_right)) {
            fallback_desktop_spectator(std::format("Calculated expanded FOV is invalid for eye {}", eye), false);
            return;
        }

        desktop_spectator.expanded_fov[eye] = {expanded_left, expanded_right, raw[2], raw[3]};
        desktop_spectator.uv_bounds[eye] = {
            (raw[0] - expanded_left) / expanded_span,
            (raw[1] - expanded_left) / expanded_span,
            0.0f,
            1.0f};
    }

    const auto baseline_fov = desktop_spectator.expanded_fov;
    const auto baseline_uv = desktop_spectator.uv_bounds;
    desktop_spectator.horizontal_scale = base_horizontal_scale;
    desktop_spectator.projection_scale = 1.0f;
    desktop_spectator.desktop_source_bounds = {0.0f, 1.0f, 0.0f, 1.0f};
    desktop_spectator.fov_mode = DesktopSpectatorFovMode::Automatic;
    desktop_spectator.fov_override_reason.clear();
    desktop_spectator.automatic_horizontal_fov = horizontal_fov_degrees(baseline_fov[selected_eye]);
    desktop_spectator.effective_horizontal_fov = desktop_spectator.automatic_horizontal_fov;
    desktop_spectator.target_horizontal_fov = std::clamp(
        desktop_spectator.target_horizontal_fov,
        MIN_DESKTOP_SPECTATOR_FOV,
        MAX_DESKTOP_SPECTATOR_FOV);

    const auto valid_uv = [](const std::array<float, 4>& uv) {
        return std::isfinite(uv[0]) && std::isfinite(uv[1]) &&
            std::isfinite(uv[2]) && std::isfinite(uv[3]) &&
            uv[0] >= 0.0f && uv[1] <= 1.0f && uv[0] < uv[1] &&
            uv[2] >= 0.0f && uv[3] <= 1.0f && uv[2] < uv[3];
    };

    if (desktop_spectator.fov_override_enabled) {
        if (!desktop_spectator.extreme) {
            desktop_spectator.fov_override_reason =
                "Custom spectator FOV requires the Extreme/AFR Cinematic path";
        } else if (!is_meta_openxr_system) {
            desktop_spectator.fov_override_reason =
                "Custom spectator FOV currently supports Meta OpenXR only";
        } else if (desktop_spectator.target_horizontal_fov <
                   desktop_spectator.automatic_horizontal_fov - 0.05f)
        {
            const auto crop_scale = solve_projection_scale(
                baseline_fov[selected_eye], desktop_spectator.target_horizontal_fov);
            if (!crop_scale || !std::isfinite(*crop_scale) || *crop_scale <= 0.0f || *crop_scale > 1.0f) {
                desktop_spectator.fov_override_reason =
                    "Could not solve the requested cropped spectator FOV";
            } else {
                const auto inset = (1.0f - *crop_scale) * 0.5f;
                desktop_spectator.desktop_source_bounds = {
                    inset, 1.0f - inset, inset, 1.0f - inset};
                desktop_spectator.fov_mode = DesktopSpectatorFovMode::Cropped;

                auto cropped_fov = baseline_fov[selected_eye];
                const auto center = (cropped_fov[0] + cropped_fov[1]) * 0.5f;
                const auto half_span = (cropped_fov[1] - cropped_fov[0]) * *crop_scale * 0.5f;
                cropped_fov[0] = center - half_span;
                cropped_fov[1] = center + half_span;
                desktop_spectator.effective_horizontal_fov = horizontal_fov_degrees(cropped_fov);
            }
        } else if (desktop_spectator.target_horizontal_fov >
                   desktop_spectator.automatic_horizontal_fov + 0.05f)
        {
            const auto projection_scale = solve_projection_scale(
                baseline_fov[selected_eye], desktop_spectator.target_horizontal_fov);
            auto candidate_fov = baseline_fov;
            auto candidate_uv = baseline_uv;
            bool valid_projection = projection_scale.has_value() &&
                std::isfinite(*projection_scale) && *projection_scale >= 1.0f;

            if (valid_projection) {
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    const auto& base = baseline_fov[eye];
                    const auto& raw = desktop_spectator.raw_fov[eye];
                    const auto center_horizontal = (base[0] + base[1]) * 0.5f;
                    const auto center_vertical = (base[2] + base[3]) * 0.5f;
                    const auto half_horizontal = (base[1] - base[0]) * *projection_scale * 0.5f;
                    const auto half_vertical = (base[2] - base[3]) * *projection_scale * 0.5f;

                    candidate_fov[eye] = {
                        center_horizontal - half_horizontal,
                        center_horizontal + half_horizontal,
                        center_vertical + half_vertical,
                        center_vertical - half_vertical};

                    const auto horizontal_span = candidate_fov[eye][1] - candidate_fov[eye][0];
                    const auto vertical_span = candidate_fov[eye][2] - candidate_fov[eye][3];
                    candidate_uv[eye] = {
                        (raw[0] - candidate_fov[eye][0]) / horizontal_span,
                        (raw[1] - candidate_fov[eye][0]) / horizontal_span,
                        (candidate_fov[eye][2] - raw[2]) / vertical_span,
                        (candidate_fov[eye][2] - raw[3]) / vertical_span};
                    valid_projection = valid_projection && valid_uv(candidate_uv[eye]);
                }
            }

            if (!valid_projection) {
                desktop_spectator.fov_override_reason =
                    "Could not solve the requested expanded spectator FOV";
            } else {
                desktop_spectator.expanded_fov = candidate_fov;
                desktop_spectator.uv_bounds = candidate_uv;
                desktop_spectator.horizontal_scale = base_horizontal_scale * *projection_scale;
                desktop_spectator.projection_scale = *projection_scale;
                desktop_spectator.fov_mode = DesktopSpectatorFovMode::Expanded;
                desktop_spectator.effective_horizontal_fov =
                    horizontal_fov_degrees(candidate_fov[selected_eye]);
            }
        }
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!valid_uv(desktop_spectator.uv_bounds[eye])) {
            fallback_desktop_spectator(std::format("Calculated OpenXR UV bounds are invalid for eye {}", eye), false);
            return;
        }
    }

    const auto selected_projection = desktop_spectator.expanded_fov[selected_eye];
    desktop_spectator.effective_aspect =
        (selected_projection[1] - selected_projection[0]) /
        (selected_projection[2] - selected_projection[3]);
    if (!std::isfinite(desktop_spectator.effective_aspect)) {
        fallback_desktop_spectator("Calculated spectator aspect is invalid", false);
        return;
    }

    const auto vr = VR::get();
    if (!desktop_spectator.extreme && (vr == nullptr || vr->get_fake_stereo_hook() == nullptr)) {
        fallback_desktop_spectator("Unreal stereo render target hook is unavailable", false);
        return;
    }

    eye_width_adjustment = desktop_spectator.extreme ? 1.0f : base_horizontal_scale;
    eye_height_adjustment = 1.0f;
    desktop_spectator.status = DesktopSpectatorStatus::Active;
    desktop_spectator.submission_logged = false;
    const auto revision = desktop_spectator_revision.fetch_add(1, std::memory_order_acq_rel) + 1;

    log_desktop_spectator(std::format(
        "FOV update revision={} mode={} baseline={} target={} effective={} projection_scale={} "
        "desktop_uv=[{},{},{},{}] eye={} aspect={} original={}x{} expanded={}x{} extreme={}",
        revision,
        get_desktop_spectator_fov_mode(),
        desktop_spectator.automatic_horizontal_fov,
        desktop_spectator.target_horizontal_fov,
        desktop_spectator.effective_horizontal_fov,
        desktop_spectator.projection_scale,
        desktop_spectator.desktop_source_bounds[0],
        desktop_spectator.desktop_source_bounds[1],
        desktop_spectator.desktop_source_bounds[2],
        desktop_spectator.desktop_source_bounds[3],
        selected_eye == 0 ? "left" : "right",
        desktop_spectator.effective_aspect,
        desktop_spectator.original_width,
        desktop_spectator.original_height,
        desktop_spectator.expanded_width,
        desktop_spectator.original_height,
        desktop_spectator.extreme));

    if (!desktop_spectator.fov_override_reason.empty()) {
        log_desktop_spectator(std::format(
            "FOV override disabled for this session update: {}",
            desktop_spectator.fov_override_reason));
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto& raw = desktop_spectator.raw_fov[eye];
        const auto& expanded = desktop_spectator.expanded_fov[eye];
        const auto& uv = desktop_spectator.uv_bounds[eye];
        log_desktop_spectator(std::format(
            "eye={} raw_tan=[{},{},{},{}] expanded_tan=[{},{},{},{}] submit_uv=[{},{},{},{}]",
            eye == 0 ? "left" : "right",
            raw[0], raw[1], raw[2], raw[3],
            expanded[0], expanded[1], expanded[2], expanded[3],
            uv[0], uv[1], uv[2], uv[3]));
    }

    if (first_activation && !desktop_spectator.extreme) {
        vr->get_fake_stereo_hook()->set_should_recreate_textures(true);
        vr->reinitialize_renderer();
    }
}
void OpenXR::on_draw_ui() {
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode(localization::get("OpenXR Options"))) {
        this->resolution_scale->draw("Resolution Scale");
        //this->push_dummy_projection->draw("Virtual Desktop Fix");

        ImGui::Checkbox(localization::get("Virtual Desktop Fix"), &this->push_dummy_projection);

        ImGui::SameLine();

        this->ignore_vd_checks->draw("Ignore Virtual Desktop Checks");

        if (ImGui::TreeNode(localization::get("Bindings"))) {
            display_bindings_editor();
            ImGui::TreePop();
        }
        
        ImGui::TreePop();
    }
}

void OpenXR::on_system_properties_acquired(const XrSystemProperties& system_properties) {
    spdlog::info("[OpenXR] OpenXR system Name: {}", system_properties.systemName);
    std::string openxr_system_name{system_properties.systemName};
    std::transform(
        openxr_system_name.begin(), openxr_system_name.end(), openxr_system_name.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    is_meta_openxr_system =
        openxr_system_name.find("meta") != std::string::npos ||
        openxr_system_name.find("oculus") != std::string::npos;
    log_desktop_spectator(std::format(
        "OpenXR system={} meta_runtime={}", system_properties.systemName, is_meta_openxr_system));

    spdlog::info("[OpenXR] OpenXR system Vendor: {}", system_properties.vendorId);
    spdlog::info("[OpenXR] OpenXR system max width: {}", system_properties.graphicsProperties.maxSwapchainImageWidth);
    spdlog::info("[OpenXR] OpenXR system max height: {}", system_properties.graphicsProperties.maxSwapchainImageHeight);
    spdlog::info("[OpenXR] OpenXR system supports {} layers", system_properties.graphicsProperties.maxLayerCount);
    spdlog::info("[OpenXR] OpenXR system orientation: {}", system_properties.trackingProperties.orientationTracking);
    spdlog::info("[OpenXR] OpenXR system position: {}", system_properties.trackingProperties.positionTracking);

    const auto should_check_vd = !this->ignore_vd_checks->value();

    if (should_check_vd && std::string_view{system_properties.systemName}.find("SteamVR/OpenXR") != std::string_view::npos) try {
        spdlog::info("[OpenXR] Detected SteamVR/OpenXR, checking for Virtual Desktop Streamer...");

        // Now double check that the Virtual Desktop streamer executable is running
        HANDLE hProcessSnap{};
        PROCESSENTRY32 pe32{};
        pe32.dwSize = sizeof(PROCESSENTRY32);

        // Take a snapshot of all processes in the system.
        hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

        if (hProcessSnap == INVALID_HANDLE_VALUE) {
            spdlog::error("[OpenXR] Failed to take a snapshot of all processes in the system.");
            return;
        }

        // Retrieve information about the first process,
        // and exit if unsuccessful
        if (!Process32First(hProcessSnap, &pe32)) {
            spdlog::error("[OpenXR] Failed to retrieve information about the first process.");
            CloseHandle(hProcessSnap); // clean the snapshot object
            return;
        }

        // Now walk the snapshot of processes, and
        // display information about each process in turn
        bool found = false;
        do {
            if (std::string_view{pe32.szExeFile}.find("VirtualDesktop.Streamer.exe") != std::string_view::npos) {
                spdlog::info("[OpenXR] Detected Virtual Desktop, enabling Virtual Desktop fix.");
                this->push_dummy_projection = true;
                found = true;
                break;
            }
        } while (Process32Next(hProcessSnap, &pe32));

        if (!found) {
            spdlog::info("[OpenXR] Did not detect Virtual Desktop, disabling Virtual Desktop fix.");
            this->push_dummy_projection = false;
        }

        CloseHandle(hProcessSnap);
    } catch(...) {
        spdlog::error("[OpenXR] Failed to check if Virtual Desktop is running.");
    }
}

void OpenXR::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : this->options) {
        option.config_load(cfg, set_defaults);
    }
}

void OpenXR::on_config_save(utility::Config& cfg) {
    for (IModValue& option : this->options) {
        option.config_save(cfg);
    }
}

void OpenXR::on_pre_render_game_thread(uint32_t frame_count) {
    this->pipeline_states[frame_count % OpenXR::QUEUE_SIZE].frame_count = frame_count;
}

VRRuntime::Error OpenXR::synchronize_frame(std::optional<uint32_t> frame_count) {
    std::scoped_lock _{sync_mtx};

    // cant sync frame between begin and endframe
    if (!this->session_ready || this->frame_began) {
        if (this->frame_began) {
            spdlog::info("Frame already began, skipping xrWaitFrame call.");
        }
        
        return VRRuntime::Error::UNSPECIFIED;
    }

    if (this->frame_synced) {
        spdlog::info("Frame already synchronized, skipping xrWaitFrame call.");

        return VRRuntime::Error::SUCCESS;
    }

    this->begin_profile();

    XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState local_frame_state{XR_TYPE_FRAME_STATE};
    auto result = xrWaitFrame(this->session, &frame_wait_info, &local_frame_state);

    this->end_profile("xrWaitFrame");

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrWaitFrame failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    } else {
        std::scoped_lock __{this->sync_assignment_mtx};

        // Correct for invalid predictedDisplayPeriod values. Seen on SteamVR OpenXR
        if (local_frame_state.predictedDisplayPeriod >= 1000000000) {
            spdlog::error("[VR] xrWaitFrame returned invalid predictedDisplayPeriod: {}", local_frame_state.predictedDisplayPeriod);
            local_frame_state.predictedDisplayPeriod = 0;
        }

        this->frame_state = local_frame_state;

        // Initialize all the existing frame states if they aren't already so we don't get some random error when calling xrEndFrame
        for (auto& pipeline_state : this->pipeline_states) {
            if (pipeline_state.frame_state.predictedDisplayTime <= 0) {
                pipeline_state.frame_state = this->frame_state;
            }
        }

        if (!frame_count && this->last_submit_state.frame_count > 0) {
            const auto next_frame = (this->last_submit_state.frame_count + 1) % OpenXR::QUEUE_SIZE;
            this->pipeline_states[next_frame].frame_state = this->frame_state;
            this->pipeline_states[next_frame].prev_frame_count = this->last_submit_state.frame_count;

            if (VR::get()->is_using_afr()) {
                const auto afr_frame = (next_frame + 1) % OpenXR::QUEUE_SIZE;
                this->pipeline_states[afr_frame].frame_state = this->frame_state;
                this->pipeline_states[afr_frame].prev_frame_count = next_frame;
            }
        } else if (frame_count) {
            this->pipeline_states[*frame_count % OpenXR::QUEUE_SIZE].frame_state = this->frame_state;
            this->pipeline_states[*frame_count % OpenXR::QUEUE_SIZE].prev_frame_count = *frame_count;

            if (VR::get()->is_using_afr()) {
                this->pipeline_states[(*frame_count + 1) % OpenXR::QUEUE_SIZE].frame_state = this->frame_state;
                this->pipeline_states[(*frame_count + 1) % OpenXR::QUEUE_SIZE].prev_frame_count = *frame_count;
            }
        }

        this->got_first_sync = true;
        this->frame_synced = true;
        this->should_update_eye_matrices = true;
    }
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_poses(bool from_view_extensions, uint32_t frame_count) {
    //std::scoped_lock ___{sync_mtx};
    std::scoped_lock _{ this->sync_assignment_mtx };

    if (!this->session_ready) {
        return VRRuntime::Error::SUCCESS;
    }

    const auto& vr = VR::get();

    /*if (!this->needs_pose_update) {
        return VRRuntime::Error::SUCCESS;
    }*/

    this->view_state = {XR_TYPE_VIEW_STATE};
    this->stage_view_state = {XR_TYPE_VIEW_STATE};

    uint32_t view_count{};

    bool should_enqueue = false;

    if (frame_count == 0) {
        frame_count = ++this->internal_frame_count;
        should_enqueue = true;
    } else {
        this->internal_frame_count = frame_count;
    }

    auto& pipeline_state = this->pipeline_states[frame_count % OpenXR::QUEUE_SIZE];

    if (pipeline_state.frame_state.predictedDisplayTime <= 1000) {
        pipeline_state.frame_state = this->frame_state;
        pipeline_state.prev_frame_count = frame_count;
    }

    // Only signal that we got the first valid pose if the display time becomes a sane value
    if (!this->got_first_valid_poses) {
        // Seen on VDXR
        if (pipeline_state.frame_state.predictedDisplayTime <= pipeline_state.frame_state.predictedDisplayPeriod) {
            spdlog::info("[VR] Frame state predicted display time is less than predicted display period!");
            return VRRuntime::Error::SUCCESS;
        }

        // Seen on VDXR. If for some reason the above if statement doesn't work, this will catch it.
        if (pipeline_state.frame_state.predictedDisplayTime == 11111111) {
            spdlog::info("[VR] Frame state predicted display time is 11111111!");
            return VRRuntime::Error::SUCCESS;
        }
    }
    
    // Not a sane value
    if (pipeline_state.frame_state.predictedDisplayTime <= 1000) {
        return VRRuntime::Error::SUCCESS;
    }

    // Pre-emptively update the frame state
    // this will allow us to get near-correct estimated poses for this frame
    // before xrWaitFrame has finished on the previous frame
    // This is usually the case *most* of the time, but sometimes the game thread will
    // actually execute after xrWaitFrame has finished, so we don't need to adjust the frame state
    if (pipeline_state.prev_frame_count + 1 < frame_count) {
        // Update the main frame state's predicted display time because it's possible that
        // the game thread can run multiple times before xrWaitFrame has finished
        // So maybe we will end up predicting more than one period ahead at times
        this->frame_state.predictedDisplayTime += this->frame_state.predictedDisplayPeriod;
        pipeline_state.frame_state = this->frame_state;
    }

    const auto display_time = pipeline_state.frame_state.predictedDisplayTime + (XrDuration)(pipeline_state.frame_state.predictedDisplayPeriod * this->prediction_scale);

    XrViewLocateInfo view_locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->view_space;

    auto result = xrLocateViews(this->session, &view_locate_info, &this->view_state, (uint32_t)this->views.size(), &view_count, this->views.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for view space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    view_locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->stage_space;

    result = xrLocateViews(this->session, &view_locate_info, &this->stage_view_state, (uint32_t)this->stage_views.size(), &view_count, this->stage_views.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for stage space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    pipeline_state.stage_views = this->stage_views;
    //this->frame_state_queue[frame_count % this->frame_state_queue.size()] = this->frame_state;
    
    if (should_enqueue) {
        enqueue_render_poses_unsafe(frame_count); // because we've already locked the mutexes.
    }

    result = xrLocateSpace(this->view_space, this->stage_space, display_time, &this->view_space_location);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateSpace for view space failed: {}", this->get_result_string(result));

        if (result == XR_ERROR_TIME_INVALID) {
            spdlog::info("[VR] Time: {}", display_time);
        }

        return (VRRuntime::Error)result;
    }

    pipeline_state.view_space_location = this->view_space_location;

    for (auto i = 0; i < this->hands.size(); ++i) {
        auto& hand = this->hands[i];
        hand.aim_location.next = &hand.aim_velocity;
        result = xrLocateSpace(hand.aim_space, this->stage_space, display_time, &hand.aim_location);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrLocateSpace for hand space failed: {}", this->get_result_string(result));
            return (VRRuntime::Error)result;
        }

        auto orientation_aim = runtimes::OpenXR::to_glm(hand.aim_location.pose.orientation);

        if (const auto pitch = vr->get_controller_pitch_offset(); pitch != 0.0f) {
            orientation_aim = glm::rotate(orientation_aim, glm::radians(pitch), Vector3f{1.0f, 0.0f, 0.0f});
        }

        this->aim_matrices[i] = Matrix4x4f{orientation_aim};
        this->aim_matrices[i][3] = Vector4f{*(Vector3f*)&hand.aim_location.pose.position, 1.0f};

        hand.grip_location.next = &hand.grip_velocity;
        result = xrLocateSpace(hand.grip_space, this->stage_space, display_time, &hand.grip_location);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrLocateSpace for hand space failed: {}", this->get_result_string(result));
            return (VRRuntime::Error)result;
        }

        auto orientation_grip = runtimes::OpenXR::to_glm(hand.grip_location.pose.orientation);

        if (const auto pitch = vr->get_controller_pitch_offset(); pitch != 0.0f) {
            orientation_grip = glm::rotate(orientation_grip, glm::radians(pitch), Vector3f{1.0f, 0.0f, 0.0f});
        }

        this->grip_matrices[i] = Matrix4x4f{orientation_grip};
        this->grip_matrices[i][3] = Vector4f{*(Vector3f*)&hand.grip_location.pose.position, 1.0f};
    }

    if (!this->got_first_valid_poses) {
        constexpr auto wanted_flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        this->got_first_valid_poses = (this->view_space_location.locationFlags & wanted_flags) == wanted_flags;

        if (this->got_first_valid_poses) {
            spdlog::info("[OpenXR] Got first valid poses at time: {} {} {}", display_time, pipeline_state.frame_state.predictedDisplayTime, pipeline_state.frame_state.predictedDisplayPeriod);
        }
    }

    this->got_first_poses = true;
    this->needs_pose_update = false;
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_render_target_size() {
    uint32_t view_count{};
    auto result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, 0, &view_count, nullptr); 
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    this->view_configs.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, view_count, &view_count, this->view_configs.data());
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

uint32_t OpenXR::get_width() const {
    if (this->view_configs.empty()) {
        return 0;
    }

    if (is_desktop_spectator_active() && !desktop_spectator.extreme) {
        return desktop_spectator.expanded_width;
    }

    return (uint32_t)((float)this->view_configs[0].recommendedImageRectWidth * this->resolution_scale->value() * eye_width_adjustment);
}

uint32_t OpenXR::get_height() const {
    if (this->view_configs.empty()) {
        return 0;
    }
    return (uint32_t)((float)this->view_configs[0].recommendedImageRectHeight * this->resolution_scale->value() * eye_height_adjustment);
}

VRRuntime::Error OpenXR::consume_events(std::function<void(void*)> callback) {
    std::scoped_lock _{event_mtx};

    XrEventDataBuffer edb{XR_TYPE_EVENT_DATA_BUFFER};
    auto result = xrPollEvent(this->instance, &edb);

    const auto bh = (XrEventDataBaseHeader*)&edb;

    while (result == XR_SUCCESS) {
        spdlog::info("VR: xrEvent: {}", this->get_structure_string(bh->type));

        if (callback) {
            callback(&edb);
        }

        if (bh->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto ev = (XrEventDataSessionStateChanged*)&edb;
            this->session_state = ev->state;

            spdlog::info("VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED {}", (uint32_t)ev->state);

            if (ev->state == XR_SESSION_STATE_READY) {
                spdlog::info("VR: XR_SESSION_STATE_READY");
                
                // Begin the session
                XrSessionBeginInfo session_begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                session_begin_info.primaryViewConfigurationType = this->view_config;

                result = xrBeginSession(this->session, &session_begin_info);

                if (result != XR_SUCCESS) {
                    this->error = std::string{"xrBeginSessionFailed: "} + this->get_result_string(result);
                    spdlog::error("VR: xrBeginSession failed: {}", this->get_result_string(result));
                } else {
                    this->session_ready = true;
                    synchronize_frame();
                }
            } else if (ev->state == XR_SESSION_STATE_LOSS_PENDING) {
                spdlog::info("VR: XR_SESSION_STATE_LOSS_PENDING");
                this->wants_reinitialize = true;
            } else if (ev->state == XR_SESSION_STATE_STOPPING) {
                spdlog::info("VR: XR_SESSION_STATE_STOPPING");

                if (this->ready()) {
                    xrEndSession(this->session);
                    this->session_ready = false;
                    this->frame_synced = false;
                    this->frame_began = false;

                    if (this->wants_reinitialize) {
                        //initialize_openxr();
                    }
                }
            }
        } else if (bh->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            this->wants_reset_origin = true;
        }

        edb = {XR_TYPE_EVENT_DATA_BUFFER};
        result = xrPollEvent(this->instance, &edb);
    } 
    
    if (result != XR_EVENT_UNAVAILABLE) {
        spdlog::error("VR: xrPollEvent failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_matrices(float nearz, float farz) {
    // exit immediately if we've updated the eye matrices since the last frame sync, so we only do this
    // operation once per sync
    if (!this->should_update_eye_matrices) {
        return VRRuntime::Error::SUCCESS;
    }

    if (!this->session_ready || this->views.size() < 2) {
        return VRRuntime::Error::SUCCESS;
    }
    if (desktop_spectator_fov_update_pending.exchange(false, std::memory_order_acq_rel)) {
        desktop_spectator.fov_override_enabled =
            pending_desktop_spectator_fov_enabled.load(std::memory_order_acquire);
        desktop_spectator.target_horizontal_fov =
            pending_desktop_spectator_horizontal_fov.load(std::memory_order_acquire);

        if (desktop_spectator.status == DesktopSpectatorStatus::WaitingForFov ||
            desktop_spectator.status == DesktopSpectatorStatus::Active)
        {
            should_recalculate_eye_projections = true;
            log_desktop_spectator(std::format(
                "Consumed live FOV request enabled={} target={}",
                desktop_spectator.fov_override_enabled,
                desktop_spectator.target_horizontal_fov));
        }
    }


    // always update the pose:
    std::unique_lock ___{ this->pose_mtx };
    const auto& left_pose = this->views[0].pose;
    const auto& right_pose = this->views[1].pose;
    this->eyes[0] = Matrix4x4f{OpenXR::to_glm(left_pose.orientation)};
    this->eyes[0][3] = Vector4f{*(Vector3f*)&left_pose.position, 1.0f};
    this->eyes[1] = Matrix4x4f{OpenXR::to_glm(right_pose.orientation)};
    this->eyes[1][3] = Vector4f{*(Vector3f*)&right_pose.position, 1.0f};

    auto get_mat = [&](int eye) {
        const auto& vr = VR::get();
        std::array<float, 4> tan_half_fov{};

        const auto cinematic_spectator = is_desktop_spectator_active();

        if (cinematic_spectator) {
            tan_half_fov = desktop_spectator.expanded_fov[eye];
        } else if (vr->get_horizontal_projection_override() == VR::HORIZONTAL_PROJECTION_OVERRIDE::HORIZONTAL_SYMMETRIC) {
            tan_half_fov[0] = -std::max(std::max(-this->raw_projections[0][0], this->raw_projections[0][1]),
                                        std::max(-this->raw_projections[1][0], this->raw_projections[1][1]));
            tan_half_fov[1] = -tan_half_fov[0];
        } else if (vr->get_horizontal_projection_override() == VR::HORIZONTAL_PROJECTION_OVERRIDE::HORIZONTAL_MIRROR) {
            float max_outer = std::max(-this->raw_projections[0][0], this->raw_projections[1][1]);
            float max_inner = std::max(this->raw_projections[0][1], -this->raw_projections[1][0]);
            tan_half_fov[0] = eye == 0 ? -max_outer : -max_inner;
            tan_half_fov[1] = eye == 0 ? max_inner : max_outer;
        } else {
            tan_half_fov[0] = this->raw_projections[eye][0];
            tan_half_fov[1] = this->raw_projections[eye][1];
        }

        if (!cinematic_spectator) {
            if (vr->get_vertical_projection_override() == VR::VERTICAL_PROJECTION_OVERRIDE::VERTICAL_SYMMETRIC) {
                tan_half_fov[2] = std::max(std::max(this->raw_projections[0][2], -this->raw_projections[0][3]),
                                            std::max(this->raw_projections[1][2], -this->raw_projections[1][3]));
                tan_half_fov[3] = -tan_half_fov[2];
            } else if (vr->get_vertical_projection_override() == VR::VERTICAL_PROJECTION_OVERRIDE::VERTICAL_MATCHED) {
                float max_top = std::max(this->raw_projections[0][2], this->raw_projections[1][2]);
                float max_bottom = std::max(-this->raw_projections[0][3], -this->raw_projections[1][3]);
                tan_half_fov[2] = max_top;
                tan_half_fov[3] = -max_bottom;
            } else {
                tan_half_fov[2] = this->raw_projections[eye][2];
                tan_half_fov[3] = this->raw_projections[eye][3];
            }
        }

        if (cinematic_spectator) {
            const auto expanded_horizontal = tan_half_fov[1] - tan_half_fov[0];
            const auto expanded_vertical = tan_half_fov[2] - tan_half_fov[3];
            view_bounds[eye][0] = (desktop_spectator.raw_fov[eye][0] - tan_half_fov[0]) / expanded_horizontal;
            view_bounds[eye][1] = (desktop_spectator.raw_fov[eye][1] - tan_half_fov[0]) / expanded_horizontal;
            view_bounds[eye][2] = (tan_half_fov[2] - desktop_spectator.raw_fov[eye][2]) / expanded_vertical;
            view_bounds[eye][3] = (tan_half_fov[2] - desktop_spectator.raw_fov[eye][3]) / expanded_vertical;
        } else {
            view_bounds[eye][0] = 0.5f - 0.5f * this->raw_projections[eye][0] / tan_half_fov[0];
            view_bounds[eye][1] = 0.5f + 0.5f * this->raw_projections[eye][1] / tan_half_fov[1];
            view_bounds[eye][2] = 0.5f - 0.5f * this->raw_projections[eye][2] / tan_half_fov[2];
            view_bounds[eye][3] = 0.5f + 0.5f * this->raw_projections[eye][3] / tan_half_fov[3];
        }

        // if we've derived the right eye, we have up to date view bounds for both so adjust the render target if necessary
        if (eye == 1) {
            if (cinematic_spectator) {
                eye_width_adjustment = desktop_spectator.extreme ? 1.0f : desktop_spectator.horizontal_scale;
                eye_height_adjustment = 1.0f;
            } else if (vr->should_grow_rectangle_for_projection_cropping()) {
                eye_width_adjustment = 1 / std::max(view_bounds[0][1] - view_bounds[0][0], view_bounds[1][1] - view_bounds[1][0]);
                eye_height_adjustment = 1 / std::max(view_bounds[0][3] - view_bounds[0][2], view_bounds[1][3] - view_bounds[1][2]);
            } else {
                eye_width_adjustment = 1;
                eye_height_adjustment = 1;
            }
            SPDLOG_INFO("Eye texture proportion scale: {} by {}", eye_width_adjustment, eye_height_adjustment);
        }

        const auto left =   tan_half_fov[0];
        const auto right =  tan_half_fov[1];
        const auto top =    tan_half_fov[2];
        const auto bottom = tan_half_fov[3];

        // signs: at this point we expect left[0] and bottom[3] to be negative
        SPDLOG_INFO("Original FOV for {} eye: {}, {}, {}, {}", eye == 0 ? "left" : "right", this->raw_projections[eye][0], this->raw_projections[eye][1],
                                                                                            this->raw_projections[eye][2], this->raw_projections[eye][3]);
        SPDLOG_INFO("Derived FOV for {} eye:  {}, {}, {}, {}", eye == 0 ? "left" : "right", left, right, top, bottom);
        SPDLOG_INFO("Derived texture bounds {} eye: {}, {}, {}, {}", eye == 0 ? "left" : "right", view_bounds[eye][0], view_bounds[eye][1], view_bounds[eye][2], view_bounds[eye][3]);
        float sum_rl = (right + left);
        float sum_tb = (top + bottom);
        float inv_rl = (1.0f / (right - left));
        float inv_tb = (1.0f / (top - bottom));

        return Matrix4x4f {
            (2.0f * inv_rl), 0.0f, 0.0f, 0.0f,
            0.0f, (2.0f * inv_tb), 0.0f, 0.0f,
            (sum_rl * -inv_rl), (sum_tb * -inv_tb), 0.0f, 1.0f,
            0.0f, 0.0f, nearz, 0.0f
        };
    };

    // if we've not yet derived an eye projection matrix, or we've changed the projection, derive it here
    // Hacky way to check for an uninitialised eye matrix - is there something better, is this necessary?
    if (this->should_recalculate_eye_projections || this->last_eye_matrix_nearz != nearz || this->projections[0][2][3] == 0) {
        // deriving the texture bounds when modifying projections requires left and right raw projections so get them all before we start:
        std::unique_lock __{this->eyes_mtx};
        const auto& left_fov = this->views[0].fov;
        this->raw_projections[0][0] = tan(left_fov.angleLeft);
        this->raw_projections[0][1] = tan(left_fov.angleRight);
        this->raw_projections[0][2] = tan(left_fov.angleUp);
        this->raw_projections[0][3] = tan(left_fov.angleDown);
        const auto& right_fov = this->views[1].fov;
        this->raw_projections[1][0] = tan(right_fov.angleLeft);
        this->raw_projections[1][1] = tan(right_fov.angleRight);
        this->raw_projections[1][2] = tan(right_fov.angleUp);
        this->raw_projections[1][3] = tan(right_fov.angleDown);

        bool valid_fov = true;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto& fov = this->raw_projections[eye];
            valid_fov = valid_fov && std::isfinite(fov[0]) && std::isfinite(fov[1]) &&
                std::isfinite(fov[2]) && std::isfinite(fov[3]) && fov[1] > fov[0] && fov[2] > fov[3];
        }

        if (!valid_fov) {
            if (desktop_spectator.status == DesktopSpectatorStatus::WaitingForFov && !desktop_spectator.fov_wait_logged) {
                log_desktop_spectator("Waiting for the first valid xrLocateViews stereo FOV; projection update deferred");
                desktop_spectator.fov_wait_logged = true;
            }

            this->should_update_eye_matrices = false;
            return VRRuntime::Error::SUCCESS;
        }

        if (desktop_spectator.status == DesktopSpectatorStatus::WaitingForFov && desktop_spectator.fov_wait_logged) {
            log_desktop_spectator("Valid xrLocateViews stereo FOV acquired; continuing Cinematic spectator initialization");
        }

        initialize_desktop_spectator();
        this->projections[0] = get_mat(0);
        this->projections[1] = get_mat(1);
        this->should_recalculate_eye_projections = false;
        this->last_eye_matrix_nearz = nearz;
    }
    // don't allow the eye matrices to be derived again until after the next frame sync
    this->should_update_eye_matrices = false;
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_input() {
    std::scoped_lock _{this->event_mtx};

    if (!this->ready() || this->session_state != XR_SESSION_STATE_FOCUSED) {
        return (VRRuntime::Error)XR_ERROR_SESSION_NOT_READY;
    }

    XrActiveActionSet active_action_set{this->action_set.handle, XR_NULL_PATH};
    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active_action_set;

    auto result = xrSyncActions(this->session, &sync_info);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to sync actions: {}", this->get_result_string(result));

        return (VRRuntime::Error)result;
    }

    const auto current_interaction_profile = this->get_current_interaction_profile();

    for (auto i = 0; i < 2; ++i) {
        auto& hand = this->hands[i];
        hand.forced_actions.clear();

        // Update controller pose state
        {
            XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
            get_info.subactionPath = hand.path;
            get_info.action = this->action_set.action_map["grippose"];

            XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};

            result = xrGetActionStatePose(this->session, &get_info, &pose_state);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to get action state pose {}: {}", i, this->get_result_string(result));

                return (VRRuntime::Error)result;
            }

            hand.active = pose_state.isActive;
        }

        {
            XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
            get_info.subactionPath = hand.path;
            get_info.action = this->action_set.action_map["pose"];

            XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};

            result = xrGetActionStatePose(this->session, &get_info, &pose_state);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to get action state pose {}: {}", i, this->get_result_string(result));

                return (VRRuntime::Error)result;
            }
        }

        // Handle vector activator stuff
        for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
            const auto activator = it.first;
            const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

            if (this->is_action_active(activator, (VRRuntime::Hand)i)) {
                const auto axis = this->get_action_axis(modifier, (VRRuntime::Hand)i);
                
                for (const auto& output : it.second) {
                    const auto distance = glm::length(output.value - axis);

                    if (distance < 0.7f) {
                        hand.forced_actions[output.action] = true;
                    }
                }
            }
        }
    }

    // TODO: Other non-hand specific inputs
    return VRRuntime::Error::SUCCESS;
}

void OpenXR::destroy() {
    if (!this->loaded) {
        return;
    }

    std::scoped_lock _{sync_mtx};

    if (this->session != nullptr) {
        if (this->session_ready) {
            xrEndSession(this->session);
        }

        xrDestroySession(this->session);
    }

    if (this->instance != nullptr && this->ever_submitted) {
        xrDestroyInstance(this->instance);
        this->instance = nullptr;
    }

    this->session = nullptr;
    this->session_ready = false;
    this->system = XR_NULL_SYSTEM_ID;
    this->frame_synced = false;
    this->frame_began = false;
}

OpenXR::PipelineState OpenXR::get_submit_state() {
    std::scoped_lock __{ this->sync_assignment_mtx };

    if (this->has_render_frame_count) {
        last_submit_state = this->pipeline_states[this->internal_render_frame_count % QUEUE_SIZE];
    } else {
        last_submit_state.stage_views = get_current_stage_view();
        last_submit_state.view_space_location = this->view_space_location;
        last_submit_state.frame_state = this->frame_state;
        last_submit_state.frame_count = this->internal_frame_count;
    }

    this->has_render_frame_count = false;

    /*if (get_frame_state(this->internal_render_frame_count-1).predictedDisplayTime > last_submit_state.frame_state.predictedDisplayTime) {
        spdlog::warn("[VR] Frame state is older than previous frame state!");
    }*/

    return last_submit_state;
}


void OpenXR::enqueue_render_poses(uint32_t frame_count) {
    //std::scoped_lock _{ this->sync_mtx };
    //std::unique_lock __{ this->pose_mtx };
    std::scoped_lock __{ this->sync_assignment_mtx };

    enqueue_render_poses_unsafe(frame_count);
}

void OpenXR::enqueue_render_poses_unsafe(uint32_t frame_count) {
    this->internal_render_frame_count = frame_count;
    this->has_render_frame_count = true;
}

std::vector<DXGI_FORMAT> OpenXR::get_supported_swapchain_formats() const {
    uint32_t count{};
    auto result = xrEnumerateSwapchainFormats(this->session, 0, &count, nullptr);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to enumerate swapchain formats: {}", this->get_result_string(result));

        return {};
    }

    std::vector<int64_t> formats(count);
    result = xrEnumerateSwapchainFormats(this->session, count, &count, formats.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to enumerate swapchain formats: {}", this->get_result_string(result));

        return {};
    }

    std::vector<DXGI_FORMAT> dxgi_formats(count);

    for (auto i = 0; i < count; ++i) {
        dxgi_formats[i] = (DXGI_FORMAT)formats[i];
    }

    return dxgi_formats;
}


std::string OpenXR::get_result_string(XrResult result) const {
    char result_string[XR_MAX_RESULT_STRING_SIZE]{};
    xrResultToString(this->instance, result, result_string);

    return result_string;
}

std::string OpenXR::get_structure_string(XrStructureType type) const {
    char structure_string[XR_MAX_STRUCTURE_NAME_SIZE]{};
    xrStructureTypeToString(this->instance, type, structure_string);

    return structure_string;
}

std::string OpenXR::get_path_string(XrPath path) const {
    if (path == XR_NULL_PATH) {
        return "XR_NULL_PATH";
    }

    std::string path_string{};
    path_string.resize(XR_MAX_PATH_LENGTH);

    uint32_t real_size{};

    if (auto result = xrPathToString(this->instance, path, XR_MAX_PATH_LENGTH, &real_size, path_string.data()); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path string: {}", this->get_result_string(result));
        return "";
    }

    path_string.resize(real_size-1);
    return path_string;
}

XrPath OpenXR::get_path(const std::string& path) const {
    XrPath path_handle{XR_NULL_PATH};

    if (auto result = xrStringToPath(this->instance, path.c_str(), &path_handle); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path: {}", this->get_result_string(result));
        return XR_NULL_PATH;
    }

    return path_handle;
}

std::string OpenXR::get_current_interaction_profile() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return "";
    }

    return this->get_path_string(state.interactionProfile);
}

XrPath OpenXR::get_current_interaction_profile_path() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return XR_NULL_PATH;
    }

    return state.interactionProfile;
}

std::optional<std::string> OpenXR::initialize_actions(const std::string& json_string) {
    spdlog::info("[VR] Initializing actions");

    if (auto result = xrStringToPath(this->instance, "/user/hand/left", &this->hands[VRRuntime::Hand::LEFT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (left): " + this->get_result_string(result);
    }

    if (auto result = xrStringToPath(this->instance, "/user/hand/right", &this->hands[VRRuntime::Hand::RIGHT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (right): " + this->get_result_string(result);
    }

    std::array<XrPath, 2> hand_paths{
        this->hands[VRRuntime::Hand::LEFT].path, 
        this->hands[VRRuntime::Hand::RIGHT].path
    };

    if (json_string.empty()) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating action set");

    XrActionSetCreateInfo action_set_create_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(action_set_create_info.actionSetName, "defaultopenxr");
    strcpy(action_set_create_info.localizedActionSetName, "Default");
    action_set_create_info.priority = 0;

    if (auto result = xrCreateActionSet(this->instance, &action_set_create_info, &this->action_set.handle); result != XR_SUCCESS) {
        return "xrCreateActionSet failed: " + this->get_result_string(result);
    }

    // Parse the JSON string using nlohmann
    json actions_json{};

    try {
        actions_json = json::parse(json_string);
    } catch (const std::exception& e) {
        return std::string{"json parse failed: "} + e.what();
    }

    if (actions_json.count("actions") == 0) {
        return "json missing actions";
    }

    auto actions_list = actions_json["actions"];
    bool has_pose_action = false;

    std::unordered_map<std::string, std::vector<XrActionSuggestedBinding>> profile_bindings{};

    for (const auto& controller : s_supported_controllers) {
        profile_bindings[controller] = {};
    }

    auto attempt_add_binding = [&](const std::string& interaction_profile, const XrActionSuggestedBinding& binding) -> bool {
        XrPath interaction_profile_path{};
        auto result = xrStringToPath(this->instance, interaction_profile.c_str(), &interaction_profile_path);
        auto& bindings = profile_bindings[interaction_profile];

        if (result == XR_SUCCESS) {
            bindings.push_back(binding);

            XrInteractionProfileSuggestedBinding suggested_bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested_bindings.interactionProfile = interaction_profile_path;
            suggested_bindings.countSuggestedBindings = (uint32_t)bindings.size();
            suggested_bindings.suggestedBindings = bindings.data();

            result = xrSuggestInteractionProfileBindings(this->instance, &suggested_bindings);

            if (result != XR_SUCCESS) {
                bindings.pop_back();
                spdlog::info("Bad binding passed to xrSuggestInteractionProfileBindings from {}: {}", interaction_profile, this->get_result_string(result));
                return false;
            }

            return true;
        } else {
            spdlog::info("Bad interaction profile passed to xrStringToPath: {}", this->get_result_string(result));
            return false;
        }
    };

    for (auto& action : actions_list) {
        XrActionCreateInfo action_create_info{XR_TYPE_ACTION_CREATE_INFO};
        auto action_name = action["name"].get<std::string>();

        if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
            action_name = action_name.substr(it + 1);
        }

        auto localized_action_name = action_name;
        std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);

        strcpy(action_create_info.actionName, action_name.c_str());
        strcpy(action_create_info.localizedActionName, localized_action_name.c_str());

        action_create_info.countSubactionPaths = (uint32_t)hand_paths.size();
        action_create_info.subactionPaths = hand_paths.data();

        if (action_name == "pose" || action_name == "grippose") {
            has_pose_action = true;
        }

        std::unordered_set<XrAction>* out_actions = nullptr;

        // Translate the OpenVR action types to OpenXR action types
        switch (utility::hash(action["type"].get<std::string>())) {
            case "boolean"_fnv:
                if (action["type"].get<std::string>().ends_with("/value")) {
                    action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                    out_actions = &this->action_set.float_actions;
                } else {
                    action_create_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
                    out_actions = &this->action_set.bool_actions;
                }
                
                break;
            case "skeleton"_fnv: // idk what this is in OpenXR
                continue;
            case "pose"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
                out_actions = &this->action_set.pose_actions;
                break;
            case "vector1"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                out_actions = &this->action_set.float_actions;
                break;
            case "vector2"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
                out_actions = &this->action_set.vector2_actions;
                break;
            case "vibration"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
                out_actions = &this->action_set.vibration_actions;
                break;
            default:
                continue;
        }
        
        // Create the action
        XrAction xr_action{XR_NULL_HANDLE};
        if (auto result = xrCreateAction(this->action_set.handle, &action_create_info, &xr_action); result != XR_SUCCESS) {
            return "xrCreateAction failed for " + action_name + ": " + this->get_result_string(result);
        }

        if (out_actions != nullptr) {
            out_actions->insert(xr_action);
        }

        spdlog::info("[VR] Created action {} with handle {:x}", action_name, (uintptr_t)xr_action);

        this->action_set.actions.push_back(xr_action);
        this->action_set.action_map[action_name] = xr_action;
        this->action_set.action_names[xr_action] = action_name;

        // Suggest bindings
        for (const auto& map_it : OpenXR::s_bindings_map) {
            if (map_it.action_name != action_name) {
                continue;
            }

            const auto& interaction_string = map_it.interaction_path_name;

            for (auto i = 0; i < 2; ++i) {
                auto hand_string = interaction_string;
                auto it = hand_string.find('*');
                auto index = i;
                bool wildcard = false;

                if (it != std::string::npos) {
                    if (i == VRRuntime::Hand::LEFT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "left");
                    } else if (i == VRRuntime::Hand::RIGHT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "right");
                    }

                    wildcard = true;
                } else {
                    if (hand_string.find("left") != std::string::npos) {
                        index = VRRuntime::Hand::LEFT;
                    } else if (hand_string.find("right") != std::string::npos) {
                        index = VRRuntime::Hand::RIGHT;
                    }
                }

                spdlog::info("[VR] {}", hand_string);

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, hand_string.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", hand_string);

                    if (!wildcard) {
                        break;
                    }

                    continue;
                }

                if (this->action_set.action_map.contains(map_it.action_name)) {
                    for (const auto& controller : s_supported_controllers) {
                        if (attempt_add_binding(controller, { this->action_set.action_map[map_it.action_name], p })) {
                            this->hands[index].profiles[controller].path_map[map_it.action_name] = p;
                        }
                    }
                }

                if (!wildcard) {
                    break;
                }
            }
        }
    }

    if (!has_pose_action) {
        return "json missing pose action";
    }

    // Check for json files that will override the default suggested bindings
    for (const auto& controller : s_supported_controllers) {
        // Create default action vector associations
        for (const auto& association : s_action_vector_associations) {
            auto& hand = this->hands[association.hand];
            auto& hand_profile = hand.profiles[controller];
            auto& action_map = this->action_set.action_map;
            const auto action_activator = action_map[association.action_activator];
            const auto action_modifier = action_map[association.action_modifier];

            hand_profile.action_vector_associations[action_activator] = action_modifier;

            for (const auto& vector_activator : association.vector_activators) {
                const auto output_action = action_map[vector_activator.action_name];
                hand_profile.vector_activators[action_activator].push_back({ vector_activator.value, output_action });
            }
        }

        auto profile_file = controller + ".json";

        // replace the slashes with underscores
        std::replace(profile_file.begin(), profile_file.end(), '/', '_');

        // If the json exists in the game's profile dir, use that.    
        auto filename = (Framework::get_persistent_dir() / profile_file).string();

        // If not, check for global profile in UEVR\Profiles dir
        if (!std::filesystem::exists(filename)) {
            filename = (Framework::get_persistent_dir() / ".." / "UEVR" / "Profiles" / profile_file).string();
            spdlog::info("[VR] Setting bindings file to {}", filename);
        }

        // check if the file exists
        if (std::filesystem::exists(filename)) {
            spdlog::info("[VR] Loading bindings for {}", filename);

            profile_bindings[controller].clear();

            this->hands[VRRuntime::Hand::LEFT].profiles[controller].vector_activators.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].vector_activators.clear();
            this->hands[VRRuntime::Hand::LEFT].profiles[controller].action_vector_associations.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].action_vector_associations.clear();
            this->hands[VRRuntime::Hand::LEFT].profiles[controller].path_map.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].path_map.clear();

            // load the json file
            auto j = nlohmann::json::parse(std::ifstream(filename));

            for (auto it : j["bindings"]) {
                auto action_str = it["action"].get<std::string>();
                auto path_str = it["path"].get<std::string>();

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, path_str.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", path_str);
                    continue;
                }

                const auto hand_idx = path_str.find("/left/") != std::string::npos ? VRRuntime::Hand::LEFT : VRRuntime::Hand::RIGHT;

                if (this->action_set.action_map.contains(action_str)) {
                    if (attempt_add_binding(controller, { this->action_set.action_map[action_str], p })) {
                        this->hands[hand_idx].profiles[controller].path_map[action_str] = p;
                    }
                }
            }

            for (auto it : j["vector2_associations"]) {
                const auto activator_name = it["activator"].get<std::string>();
                const auto modifier_name = it["modifier"].get<std::string>();
                const auto path_name = it["path"].get<std::string>();
                const auto path = this->get_path(path_name);

                for (auto output : it["outputs"]) {
                    const auto action_name = output["action"].get<std::string>();
                    
                    auto value_json = output["value"];
                    const auto value = Vector2f{value_json["x"].get<float>(), value_json["y"].get<float>()};

                    if (this->action_set.action_map.contains(action_name)) {
                        auto& hand = path == this->hands[VRRuntime::Hand::LEFT].path ? this->hands[VRRuntime::Hand::LEFT] : this->hands[VRRuntime::Hand::RIGHT];
                        auto& hand_profile = hand.profiles[controller];

                        spdlog::info("[VR] Adding vector2 association for {} {}", controller, path_name);

                        const auto output_action = this->action_set.action_map[action_name];
                        const auto action_modifier = this->action_set.action_map[modifier_name];
                        const auto action_activator = this->action_set.action_map[activator_name];

                        hand_profile.vector_activators[action_activator].push_back({ value, output_action });
                        hand_profile.action_vector_associations[action_activator] = action_modifier;
                    }
                }
            }
        }
    }

    // Create the action spaces for each hand
    // Grip space
    for (auto i = 0; i < 2; ++i) {
        spdlog::info("[VR] Creating grip action space for hand {}", i);
        
        XrActionSpaceCreateInfo action_space_create_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        action_space_create_info.action = this->action_set.action_map["grippose"];
        action_space_create_info.subactionPath = this->hands[i].path;
        action_space_create_info.poseInActionSpace.orientation.w = 1.0f;

        if (auto result = xrCreateActionSpace(this->session, &action_space_create_info, &this->hands[i].grip_space); result != XR_SUCCESS) {
            return "xrCreateActionSpace failed (" + std::to_string(i) + ")" + this->get_result_string(result);
        }
    }

    // Aim space
    for (auto i = 0; i < 2; ++i) {
        spdlog::info("[VR] Creating aim action space for hand {}", i);
        
        XrActionSpaceCreateInfo action_space_create_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        action_space_create_info.action = this->action_set.action_map["pose"];
        action_space_create_info.subactionPath = this->hands[i].path;
        action_space_create_info.poseInActionSpace.orientation.w = 1.0f;

        if (auto result = xrCreateActionSpace(this->session, &action_space_create_info, &this->hands[i].aim_space); result != XR_SUCCESS) {
            return "xrCreateActionSpace failed (" + std::to_string(i) + ")" + this->get_result_string(result);
        }
    }

    // Attach the action set to the session
    spdlog::info("[VR] Attaching action set to session");

    XrSessionActionSetsAttachInfo action_sets_attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    action_sets_attach_info.countActionSets = 1;
    action_sets_attach_info.actionSets = &this->action_set.handle;

    if (auto result = xrAttachSessionActionSets(this->session, &action_sets_attach_info); result != XR_SUCCESS) {
        return "xrAttachSessionActionSets failed: " + this->get_result_string(result);
    }

    return std::nullopt;
}

bool OpenXR::is_action_active(XrAction action, VRRuntime::Hand hand) const {
    if (hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    if (this->action_set.bool_actions.contains(action)) {
        XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
        auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
    } else if (this->action_set.float_actions.contains(action)) {
        XrActionStateFloat active{XR_TYPE_ACTION_STATE_FLOAT};
        auto result = xrGetActionStateFloat(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState > 0.0f;
    } // idk?

    return false;
}

bool OpenXR::is_action_active(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    auto action = this->action_set.action_map.find(action_name.data())->second;

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
}

bool OpenXR::is_action_active_once(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    auto action = this->action_set.action_map.find(action_name.data())->second;

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE && active.changedSinceLastSync == XR_TRUE;
}

Vector2f OpenXR::get_action_axis(XrAction action, VRRuntime::Hand hand) const {
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

std::string OpenXR::translate_openvr_action_name(std::string action_name) const {
    if (action_name.empty()) {
        return action_name;
    }

    if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
        action_name = action_name.substr(it + 1);
    }

    std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);
    return action_name;
}

Vector2f OpenXR::get_stick_axis(VRRuntime::Hand hand_idx) const {
    if (hand_idx != VRRuntime::Hand::LEFT && hand_idx != VRRuntime::Hand::RIGHT) {
        return Vector2f{};
    }

    if (!this->action_set.action_map.contains("joystick")) {
        return Vector2f{};
    }

    const auto& hand = this->hands[hand_idx];

    auto profile_it = hand.profiles.find(this->get_current_interaction_profile());

    if (profile_it == hand.profiles.end()) {
        return Vector2f{};
    }

    const auto& hand_profile = profile_it->second;

    XrAction action{XR_NULL_HANDLE};

    if (hand_profile.path_map.contains("joystick")) {
        auto it = this->action_set.action_map.find("joystick");

        if (it == this->action_set.action_map.end()) {
            return Vector2f{};
        }

        action = it->second;
    } else {
        auto it = this->action_set.action_map.find("touchpad");

        if (it == this->action_set.action_map.end()) {
            return Vector2f{};
        }

        action = it->second;
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand.path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

Vector2f OpenXR::get_left_stick_axis() const {
    return this->get_stick_axis(VRRuntime::Hand::LEFT);
}

Vector2f OpenXR::get_right_stick_axis() const {
    return this->get_stick_axis(VRRuntime::Hand::RIGHT);
}

void OpenXR::trigger_haptic_vibration(float duration, float frequency, float amplitude, VRRuntime::Hand source) const {
    if (!this->action_set.action_map.contains("haptic")) {
        return;
    }

    XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
    haptic_info.action = this->action_set.action_map.find("haptic")->second;
    haptic_info.subactionPath = this->hands[source].path;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.frequency = frequency;

    // cast the duration from seconds to nanoseconds
    vibration.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<float>(duration)).count();

    auto result = xrApplyHapticFeedback(this->session, &haptic_info, (XrHapticBaseHeader*)&vibration);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to apply haptic feedback: {}", this->get_result_string(result));
    }
}

void OpenXR::display_bindings_editor() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    ImGui::Text(localization::get("Interaction Profile: %s"), current_interaction_profile.c_str());

    if (ImGui::Button(localization::get("Restore Default Bindings"))) {
        auto filename = current_interaction_profile + ".json";
        
        // replace the slashes with underscores
        std::replace(filename.begin(), filename.end(), '/', '_');

        const auto persistent_filename = (Framework::get_persistent_dir() / filename).string();

        if (std::filesystem::exists(persistent_filename)) {
            // Delete the file
            std::filesystem::remove(persistent_filename);
            this->wants_reinitialize = true;
        }
    }

    if (ImGui::Button(localization::get("Save Bindings"))) {
        this->save_bindings();
    }

    auto display_hand = [&](const std::string& name, uint32_t index) {
        if (current_interaction_profile.empty() || current_interaction_profile == "XR_NULL_PATH") {
            ImGui::Text(localization::get("Interaction profile not loaded, try putting on your headset."));
            return;
        }

        if (ImGui::TreeNode(name.data())) {
            ImGui::PushID(name.data());

            auto& hand = this->hands[index];

            for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
                const auto activator = it.first;
                const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

                if (this->is_action_active(activator, (VRRuntime::Hand)index)) {
                    const auto axis = this->get_action_axis(modifier, (VRRuntime::Hand)index);
                    
                    for (const auto& output : it.second) {
                        const auto distance = glm::length(output.value - axis);
                        ImGui::Text(localization::get("%s: %.2f"), this->action_set.action_names[output.action].data(), distance);
                    }
                }
            }

            auto& path_map = hand.profiles[current_interaction_profile].path_map;

            std::vector<std::string> known_actions{};
            std::vector<const char*> known_actions_cstr{};

            std::vector<std::string> known_vector2_actions{};
            std::vector<const char*> known_vector2_actions_cstr{};

            for (auto action :this->action_set.actions) {
                known_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_actions) {
                known_actions_cstr.push_back(it.data());
            };

            for (auto action : this->action_set.vector2_actions) {
                known_vector2_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_vector2_actions) {
                known_vector2_actions_cstr.push_back(it.data());
            }
            
            for (auto& it : path_map) {
                ImGui::PushID(it.first.data());

                int current_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == it.first) {
                        current_combo_index = i;
                        break;
                    }
                }

                if (ImGui::Button(localization::get("X"))) {
                    path_map.erase(it.first);

                    this->save_bindings();
                    ImGui::PopID();
                    break;
                }
                
                auto combo_name = this->get_path_string(it.second) + ": " + known_actions[current_combo_index];

                ImGui::SameLine();
                if (ImGui::Combo(combo_name.c_str(), &current_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    path_map.erase(it.first);
                    path_map[known_actions[current_combo_index]] = it.second;

                    this->save_bindings();
                    ImGui::PopID();

                    break;
                }

                ImGui::PopID();
            }

            // Create a way to add a completely new binding
            // Create a textbox for inputting the path for the new binding
            ImGui::InputText(localization::get("New Binding (e.g. /user/hand/left/input/trigger)"), hand.ui.new_path_name, XR_MAX_PATH_LENGTH);
            ImGui::Combo(localization::get("Action"), &hand.ui.action_combo_index, known_actions_cstr.data(), known_actions_cstr.size());

            if (ImGui::Button(localization::get("Add Binding"))) {
                XrPath p{};
                if (xrStringToPath(this->instance, hand.ui.new_path_name, &p) != XR_SUCCESS) {
                    spdlog::error("[VR] Failed to convert path: {}", hand.ui.new_path_name);
                } else {
                    path_map[known_actions[hand.ui.action_combo_index]] = p;
                    this->save_bindings();
                }
            }

            ImGui::Text(localization::get("Vector2 Associations"));
            for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
                ImGui::PushID(&it.first);

                const auto activator = it.first;
                const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

                const auto activator_name = this->action_set.action_names[activator];
                const auto modifier_name = this->action_set.action_names[modifier];

                int activator_combo_index = 0;
                int modifier_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == activator_name) {
                        activator_combo_index = i;
                        break;
                    }
                }

                for (auto i = 0; i < known_vector2_actions.size(); i++) {
                    if (known_vector2_actions[i] == modifier_name) {
                        modifier_combo_index = i;
                        break;
                    }
                }

                ImGui::PushID(modifier_name.data());
                
                if (ImGui::Combo(modifier_name.data(), &modifier_combo_index, known_vector2_actions_cstr.data(), known_vector2_actions_cstr.size())) {
                    hand.profiles[current_interaction_profile].action_vector_associations[activator] = this->action_set.action_map[known_vector2_actions[modifier_combo_index]];
                }
                
                ImGui::PushID(activator_name.data());
                ImGui::Indent();

                if (ImGui::Combo(activator_name.data(), &activator_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    const auto old_outputs = hand.profiles[current_interaction_profile].vector_activators[activator];
                    const auto new_activator = this->action_set.action_map[known_actions[activator_combo_index]];

                    hand.profiles[current_interaction_profile].action_vector_associations.erase(activator);
                    hand.profiles[current_interaction_profile].action_vector_associations[new_activator] = modifier;
                    hand.profiles[current_interaction_profile].vector_activators.erase(activator);
                    hand.profiles[current_interaction_profile].vector_activators[new_activator] = old_outputs;
                }

                ImGui::Indent();
                for (auto& output : it.second) {
                    int output_combo_index = 0;
                    const auto output_name = this->action_set.action_names[output.action];

                    for (auto i = 0; i < known_actions.size(); i++) {
                        if (known_actions[i] == output_name) {
                            output_combo_index = i;
                            break;
                        }
                    }

                    ImGui::PushID(output_name.data());

                    if (ImGui::Combo(output_name.data(), &output_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                        output.action = this->action_set.action_map[known_actions[output_combo_index]];
                    }

                    ImGui::SliderFloat2(localization::get("Value"), &output.value[0], -1.0f, 1.0f);
                    ImGui::PopID();
                }

                if (ImGui::Button(localization::get("Insert New Output"))) {
                    hand.profiles[current_interaction_profile].vector_activators[activator].push_back({});
                }

                ImGui::Unindent();
                ImGui::Unindent();
                ImGui::PopID();
                ImGui::PopID();

                ImGui::PopID();
            }

            ImGui::Combo(localization::get("New Vector2 Activator"), &hand.ui.activator_combo_index, known_actions_cstr.data(), known_actions_cstr.size());
            ImGui::Combo(localization::get("New Vector2 Modifier"), &hand.ui.modifier_combo_index, known_vector2_actions_cstr.data(), known_vector2_actions_cstr.size());
            ImGui::Combo(localization::get("New Vector2 Output"), &hand.ui.output_combo_index, known_actions_cstr.data(), known_actions_cstr.size());
            ImGui::SliderFloat2(localization::get("New Vector2 Value"), &hand.ui.output_vector2[0], -1.0f, 1.0f);

            if (ImGui::Button(localization::get("Add Vector2 Association"))) {
                const auto activator = this->action_set.action_map[known_actions[hand.ui.activator_combo_index]];
                const auto modifier = this->action_set.action_map[known_vector2_actions[hand.ui.modifier_combo_index]];
                const auto output = this->action_set.action_map[known_actions[hand.ui.output_combo_index]];

                hand.profiles[current_interaction_profile].action_vector_associations[activator] = modifier;
                hand.profiles[current_interaction_profile].vector_activators[activator].push_back({hand.ui.output_vector2, output});
            }

            ImGui::PopID();
            ImGui::TreePop();
        }
    };

    display_hand("Left", 0);
    display_hand("Right", 1);
}

void OpenXR::save_bindings() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    nlohmann::json j;

    for (auto& hand : this->hands) {
        for (auto& it : hand.profiles[current_interaction_profile].path_map) {
            nlohmann::json binding{};
            binding["action"] = it.first;
            binding["path"] = this->get_path_string(it.second);
            j["bindings"].push_back(binding);
        }

        for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
            const auto activator = it.first;
            const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

            const auto activator_name = this->action_set.action_names[activator];
            const auto modifier_name = this->action_set.action_names[modifier];

            nlohmann::json vector2_association{};
            vector2_association["path"] = this->get_path_string(hand.path);
            vector2_association["activator"] = activator_name;
            vector2_association["modifier"] = modifier_name;

            nlohmann::json outputs{};
            for (auto& output : it.second) {
                const auto output_name = this->action_set.action_names[output.action];

                nlohmann::json output_json{};
                output_json["action"] = output_name;
                output_json["value"]["x"] = output.value.x;
                output_json["value"]["y"] = output.value.y;

                outputs.push_back(output_json);
            }

            vector2_association["outputs"] = outputs;
            j["vector2_associations"].push_back(vector2_association);
        }
    }

    auto filename = current_interaction_profile + ".json";
    
    // replace the slashes with underscores
    std::replace(filename.begin(), filename.end(), '/', '_');
    const auto persistent_filename = (Framework::get_persistent_dir() / filename).string();

    std::ofstream(persistent_filename) << j.dump(4);

    this->wants_reinitialize = true;
}

XrResult OpenXR::begin_frame() {
    std::scoped_lock _{sync_mtx};

    if (!this->ready() || !this->got_first_poses || !this->frame_synced) {
        //spdlog::info("VR: begin_frame: not ready");
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (this->frame_began) {
        spdlog::info("[VR] begin_frame called while frame already began");
        return XR_SUCCESS;
    }

    this->begin_profile();

    XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    auto result = xrBeginFrame(this->session, &frame_begin_info);

    this->end_profile("xrBeginFrame");

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrBeginFrame failed: {}", this->get_result_string(result));
    }

    if (result == XR_ERROR_CALL_ORDER_INVALID) {
        synchronize_frame();
        result = xrBeginFrame(this->session, &frame_begin_info);
    }

    this->frame_began = result == XR_SUCCESS || result == XR_FRAME_DISCARDED; // discarded means endFrame was not called

    return result;
}

XrResult OpenXR::end_frame(const std::vector<XrCompositionLayerBaseHeader*>& quad_layers, bool has_depth) {
    std::scoped_lock _{sync_mtx};

    if (!this->ready() || !this->got_first_poses || !this->frame_synced) {
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (!this->frame_began) {
        spdlog::info("[VR] end_frame called while frame not begun");
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    const auto is_afr = VR::get()->is_using_afr();

    if (is_afr) {
        if (!this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_LEFT_EYE) || !this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_RIGHT_EYE)) {
            spdlog::error("[VR] AFR swapchains not created");
            return XR_ERROR_VALIDATION_FAILURE;
        }

        /*if (has_depth && (!this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE) || !this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE))) {
            spdlog::error("[VR] AFR depth swapchains not created");
            return XR_ERROR_VALIDATION_FAILURE;
        }*/

        has_depth = has_depth && this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE) && this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE);
    } else {
        if (!this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::DOUBLE_WIDE)) {
            spdlog::error("[VR] Double wide swapchain not created");
            return XR_ERROR_VALIDATION_FAILURE;
        }

        /*if (has_depth && !this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::DEPTH)) {
            spdlog::error("[VR] Double wide depth swapchain not created");
            return XR_ERROR_VALIDATION_FAILURE;
        }*/

        has_depth = has_depth && this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::DEPTH);
    }

    const auto submit_state = this->get_submit_state();
    const auto& pipelined_stage_views = submit_state.stage_views;
    const auto& pipelined_frame_state = submit_state.frame_state;

    if (pipelined_stage_views.empty()) {
        spdlog::warn("[VR] No stage views to submit");
    }

    // Reset size of end frame data containers to 0.
    this->end_frame_data.clear();

    auto& projection_layer_cache = this->end_frame_data.projection_layer_cache;
    auto& projection_layer_views = this->end_frame_data.projection_layer_views;
    auto& depth_layers           = this->end_frame_data.depth_layers;
    auto& layers                 = this->end_frame_data.layers;

    // Dummy projection layers for Virtual Desktop. If we don't do this, timewarp does not work correctly on VD.
    // the reasoning from ggodin (VD dev) is that VD composites all layers using the top layer's pose (apparently)
    // I am actually not sure why this fixes the issue, but it does. and even makes the SteamVR overlay work completely fine.
    const auto should_push_dummy = this->push_dummy_projection == true && 
                                   !pipelined_stage_views.empty() && 
                                   this->ever_submitted == true;

    XrCompositionLayerProjection dummy_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> dummy_projection_layer_views{};

    if (should_push_dummy) {
        const auto& vd_swapchain = this->swapchains[(uint32_t)OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP];

        for (auto i = 0; i < std::min<size_t>(dummy_projection_layer_views.size(), pipelined_stage_views.size()); ++i) {
            auto& view = dummy_projection_layer_views[i];
            view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            view.pose = pipelined_stage_views[i].pose;
            view.fov = pipelined_stage_views[i].fov;

            view.subImage.swapchain = vd_swapchain.handle;
            view.subImage.imageRect.offset = {(vd_swapchain.width / 2) * i, 0};
            view.subImage.imageRect.extent = {(vd_swapchain.width / 2), vd_swapchain.height};
            view.subImage.imageArrayIndex = 0;
        }

        dummy_projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        dummy_projection_layer.viewCount = 2;
        dummy_projection_layer.views = dummy_projection_layer_views.data();
        dummy_projection_layer.space = this->stage_space;
        dummy_projection_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    }

    // we CANT push the layers every time, it cause some layer error
    // in xrEndFrame, so we must only do it when shouldRender is true
    if (pipelined_frame_state.shouldRender == XR_TRUE && !pipelined_stage_views.empty()) {
        projection_layer_views.resize(pipelined_stage_views.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        depth_layers.resize(projection_layer_views.size(), {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});

        for (auto i = 0; i < projection_layer_views.size(); ++i) {            
            Swapchain* swapchain = nullptr;

            if (is_afr) {
                if (i == 0) {
                    swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::AFR_LEFT_EYE];
                } else {
                    swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::AFR_RIGHT_EYE];
                }
            } else {
                swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::DOUBLE_WIDE];
            }

            projection_layer_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projection_layer_views[i].pose = pipelined_stage_views[i].pose;
            projection_layer_views[i].fov = pipelined_stage_views[i].fov;
            projection_layer_views[i].subImage.swapchain = swapchain->handle;

            int32_t offset_x = 0, offset_y = 0, extent_x = 0, extent_y = 0;
            // If we're working with a double-wide texture, the bounds apply within one eye slot.
            const int32_t texture_area_width = is_afr ? swapchain->width : swapchain->width / 2;

            if (is_desktop_spectator_active()) {
                const auto slot_offset = is_afr ? 0 : static_cast<int32_t>(i) * texture_area_width;
                const auto left = std::clamp(
                    static_cast<int32_t>(std::floor(view_bounds[i][0] * texture_area_width)),
                    0, texture_area_width - 1);
                const auto right = std::clamp(
                    static_cast<int32_t>(std::ceil(view_bounds[i][1] * texture_area_width)),
                    left + 1, texture_area_width);
                const auto top = std::clamp(
                    static_cast<int32_t>(std::floor(view_bounds[i][2] * swapchain->height)),
                    0, swapchain->height - 1);
                const auto bottom = std::clamp(
                    static_cast<int32_t>(std::ceil(view_bounds[i][3] * swapchain->height)),
                    top + 1, swapchain->height);

                offset_x = slot_offset + left;
                extent_x = right - left;
                offset_y = top;
                extent_y = bottom - top;
            } else {
                if (is_afr || i == 0) {
                    offset_x = view_bounds[i][0] * texture_area_width;
                    extent_x = view_bounds[i][1] * texture_area_width - offset_x;
                } else {
                    // Right eye in the double-wide texture.
                    offset_x = texture_area_width + view_bounds[i][0] * texture_area_width;
                    extent_x = view_bounds[i][1] * texture_area_width - (offset_x - texture_area_width);
                }
                offset_y = view_bounds[i][2] * swapchain->height;
                extent_y = view_bounds[i][3] * swapchain->height - offset_y;
            }
            
            // SPDLOG_INFO("image calc for eye {} {}, {}, {}, {}", i, offset_x, extent_x, offset_y, extent_y);
            projection_layer_views[i].subImage.imageRect.offset = {offset_x, offset_y};
            projection_layer_views[i].subImage.imageRect.extent = {extent_x, extent_y};

            if (is_desktop_spectator_active() && !desktop_spectator.submission_logged) {
                log_desktop_spectator(std::format(
                    "OpenXR submit eye={} uv=[{},{},{},{}] imageRect=[{},{},{},{}] swapchain={}x{}",
                    i == 0 ? "left" : "right",
                    view_bounds[i][0], view_bounds[i][1], view_bounds[i][2], view_bounds[i][3],
                    offset_x, offset_y, extent_x, extent_y, swapchain->width, swapchain->height));
                if (i + 1 == projection_layer_views.size()) {
                    desktop_spectator.submission_logged = true;
                }
            }
            if (has_depth) {
                Swapchain* depth_swapchain = nullptr;

                if (is_afr) {
                    if (i == 0) {
                        depth_swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE];
                    } else {
                        depth_swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE];
                    }
                } else {
                    depth_swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::DEPTH];
                }

                depth_layers[i].type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
                depth_layers[i].next = nullptr;
                depth_layers[i].subImage.swapchain = depth_swapchain->handle;

                depth_layers[i].subImage.imageRect.offset = {offset_x, offset_y};
                depth_layers[i].subImage.imageRect.extent = {std::min<int>(get_width(), extent_x), std::min<int>(get_height(), extent_y)};
                depth_layers[i].minDepth = 0.0f;
                depth_layers[i].maxDepth = 1.0f;
                auto wtm = VR::get()->get_world_to_meters();
                if (wtm < 0.0f || wtm == 0.0f) {
                    wtm = 1.0f;
                }

                const auto nearz = sdk::globals::get_near_clipping_plane() / wtm;

                depth_layers[i].nearZ = FLT_MAX;
                depth_layers[i].farZ = nearz * VR::get()->get_depth_scale();
                
                projection_layer_views[i].next = &depth_layers[i];
            }
        }

        auto& layer = projection_layer_cache.emplace_back();
        layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        layer.space = this->stage_space;
        layer.viewCount = (uint32_t)projection_layer_views.size();
        layer.views = projection_layer_views.data();
        layer.layerFlags = 0;

        if (should_push_dummy) {
            layers.push_back((XrCompositionLayerBaseHeader*)&dummy_projection_layer);
        }

        for (auto& l : projection_layer_cache) {
            layers.push_back((XrCompositionLayerBaseHeader*)&l);
        }

        for (auto& l : quad_layers) {   
            layers.push_back(l);
        }
    }

    XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
    frame_end_info.displayTime = pipelined_frame_state.predictedDisplayTime != 0 ? pipelined_frame_state.predictedDisplayTime : this->frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = this->blend_mode;
    frame_end_info.layerCount = (uint32_t)layers.size();
    frame_end_info.layers = layers.data();

    //spdlog::info("[VR] display time diff: {}", pipelined_frame_state.predictedDisplayTime - this->frame_state.predictedDisplayTime);
    //spdlog::info("[VR] Ending frame, {} layers", frame_end_info.layerCount);
    //spdlog::info("[VR] Ending frame, layer ptr: {:x}", (uintptr_t)frame_end_info.layers);

    this->begin_profile();
    auto result = xrEndFrame(this->session, &frame_end_info);
    this->end_profile("xrEndFrame");
    
    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrEndFrame failed: {}", this->get_result_string(result));

        if (result == XR_ERROR_TIME_INVALID) {
             spdlog::error("[VR] xrEndFrame time: submitted: {} vs frame_state: {}", frame_end_info.displayTime, this->frame_state.predictedDisplayTime);
             spdlog::error("[VR] display time diff: {}", frame_end_info.displayTime - this->frame_state.predictedDisplayTime);
        }
    } else {
        this->ever_submitted = true;
    }
    
    this->frame_began = false;
    this->frame_synced = false;

    return result;
}
}
