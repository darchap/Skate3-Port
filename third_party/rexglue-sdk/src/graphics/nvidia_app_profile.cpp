#include <rex/graphics/nvidia_app_profile.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(nvidia_prefer_max_performance, true, "GPU",
                    "Write 'Power management mode = Prefer maximum performance' into the NVIDIA "
                    "driver's application profile for this executable at startup, preventing the "
                    "driver from parking the GPU in a low P-state after load-screen lulls (the "
                    "sticky low-fps state). Setting this to false only skips the write; it does "
                    "not remove an already-written profile");

#if REX_PLATFORM_WIN32

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <memory>
#include <string>

#include <Windows.h>

#include <rex/platform/dynlib.h>

namespace rex::graphics {

namespace {

// Minimal NvAPI DRS surface. nvapi64.dll ships with the NVIDIA driver and
// exports a single nvapi_QueryInterface; everything else is resolved through
// it by the stable public interface ids from the MIT-licensed NVAPI SDK.
using NvStatus = int;  // NVAPI_OK == 0.
using NvDrsSessionHandle = void*;
using NvDrsProfileHandle = void*;

constexpr uint32_t kIdInitialize = 0x0150E828;
constexpr uint32_t kIdDrsCreateSession = 0x0694D52E;
constexpr uint32_t kIdDrsDestroySession = 0xDAD9CFF8;
constexpr uint32_t kIdDrsLoadSettings = 0x375DBD6B;
constexpr uint32_t kIdDrsSaveSettings = 0xFCBC7E14;
constexpr uint32_t kIdDrsFindProfileByName = 0x7E4A9A0B;
constexpr uint32_t kIdDrsCreateProfile = 0xCC176068;
constexpr uint32_t kIdDrsFindApplicationByName = 0xEEE566B2;
constexpr uint32_t kIdDrsCreateApplication = 0x4347A9DE;
constexpr uint32_t kIdDrsGetSetting = 0x73BF8338;
constexpr uint32_t kIdDrsSetSetting = 0x577DD202;

// PREFERRED_PSTATE_ID / PREFERRED_PSTATE_PREFER_MAX from NvApiDriverSettings.h.
constexpr uint32_t kPreferredPstateSettingId = 0x1057EB71;
constexpr uint32_t kPreferredPstatePreferMax = 1;

constexpr uint32_t kNvapiUnicodeStringMax = 2048;
using NvApiUnicodeString = uint16_t[kNvapiUnicodeStringMax];

constexpr uint32_t MakeNvapiVersion(size_t struct_size, uint32_t version) {
  return uint32_t(struct_size) | (version << 16);
}

struct NvdrsApplicationV1 {
  uint32_t version;
  uint32_t is_predefined;
  NvApiUnicodeString app_name;
  NvApiUnicodeString user_friendly_name;
  NvApiUnicodeString launcher;
};

struct NvdrsProfileV1 {
  uint32_t version;
  NvApiUnicodeString profile_name;
  uint32_t gpu_support;
  uint32_t is_predefined;
  uint32_t num_of_apps;
  uint32_t num_of_settings;
};

// NVDRS_SETTING_V1: both value unions are the size of the largest member, the
// binary setting (4-byte length + 4096 payload).
struct NvdrsBinarySetting {
  uint32_t value_length;
  uint8_t value_data[4096];
};

struct NvdrsSettingV1 {
  uint32_t version;
  NvApiUnicodeString setting_name;
  uint32_t setting_id;
  uint32_t setting_type;      // NVDRS_SETTING_TYPE; 0 = DWORD.
  uint32_t setting_location;  // NVDRS_SETTING_LOCATION.
  uint32_t is_current_predefined;
  uint32_t is_predefined_valid;
  union {
    uint32_t u32_predefined_value;
    NvdrsBinarySetting binary_predefined_value;
    NvApiUnicodeString wsz_predefined_value;
  };
  union {
    uint32_t u32_current_value;
    NvdrsBinarySetting binary_current_value;
    NvApiUnicodeString wsz_current_value;
  };
};

static_assert(sizeof(NvdrsApplicationV1) == 8 + 3 * 4096, "NVDRS_APPLICATION_V1 layout");
static_assert(sizeof(NvdrsProfileV1) == 20 + 4096, "NVDRS_PROFILE_V1 layout");
static_assert(sizeof(NvdrsSettingV1) == 24 + 4096 + 2 * 4100, "NVDRS_SETTING_V1 layout");

void FillNvapiUnicodeString(NvApiUnicodeString& dest, const std::wstring& src) {
  std::memset(dest, 0, sizeof(NvApiUnicodeString));
  size_t count = std::min(src.size(), size_t(kNvapiUnicodeStringMax - 1));
  for (size_t i = 0; i < count; ++i) {
    dest[i] = uint16_t(src[i]);
  }
}

struct NvApiDrs {
  rex::platform::DynamicLibrary library;
  NvStatus (*initialize)() = nullptr;
  NvStatus (*create_session)(NvDrsSessionHandle*) = nullptr;
  NvStatus (*destroy_session)(NvDrsSessionHandle) = nullptr;
  NvStatus (*load_settings)(NvDrsSessionHandle) = nullptr;
  NvStatus (*save_settings)(NvDrsSessionHandle) = nullptr;
  NvStatus (*find_profile_by_name)(NvDrsSessionHandle, uint16_t*, NvDrsProfileHandle*) = nullptr;
  NvStatus (*create_profile)(NvDrsSessionHandle, NvdrsProfileV1*, NvDrsProfileHandle*) = nullptr;
  NvStatus (*find_application_by_name)(NvDrsSessionHandle, uint16_t*, NvDrsProfileHandle*,
                                       NvdrsApplicationV1*) = nullptr;
  NvStatus (*create_application)(NvDrsSessionHandle, NvDrsProfileHandle,
                                 NvdrsApplicationV1*) = nullptr;
  NvStatus (*get_setting)(NvDrsSessionHandle, NvDrsProfileHandle, uint32_t,
                          NvdrsSettingV1*) = nullptr;
  NvStatus (*set_setting)(NvDrsSessionHandle, NvDrsProfileHandle, NvdrsSettingV1*) = nullptr;

  bool Load() {
    if (!library.Load("nvapi64.dll")) {
      return false;
    }
    using QueryInterfaceFn = void* (*)(uint32_t);
    auto query_interface = library.GetSymbol<QueryInterfaceFn>("nvapi_QueryInterface");
    if (!query_interface) {
      return false;
    }
    initialize = reinterpret_cast<decltype(initialize)>(query_interface(kIdInitialize));
    create_session =
        reinterpret_cast<decltype(create_session)>(query_interface(kIdDrsCreateSession));
    destroy_session =
        reinterpret_cast<decltype(destroy_session)>(query_interface(kIdDrsDestroySession));
    load_settings =
        reinterpret_cast<decltype(load_settings)>(query_interface(kIdDrsLoadSettings));
    save_settings =
        reinterpret_cast<decltype(save_settings)>(query_interface(kIdDrsSaveSettings));
    find_profile_by_name =
        reinterpret_cast<decltype(find_profile_by_name)>(query_interface(kIdDrsFindProfileByName));
    create_profile =
        reinterpret_cast<decltype(create_profile)>(query_interface(kIdDrsCreateProfile));
    find_application_by_name = reinterpret_cast<decltype(find_application_by_name)>(
        query_interface(kIdDrsFindApplicationByName));
    create_application =
        reinterpret_cast<decltype(create_application)>(query_interface(kIdDrsCreateApplication));
    get_setting = reinterpret_cast<decltype(get_setting)>(query_interface(kIdDrsGetSetting));
    set_setting = reinterpret_cast<decltype(set_setting)>(query_interface(kIdDrsSetSetting));
    return initialize && create_session && destroy_session && load_settings && save_settings &&
           find_profile_by_name && create_profile && find_application_by_name &&
           create_application && get_setting && set_setting;
  }
};

std::wstring GetExecutableBaseNameLower() {
  wchar_t module_path[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, module_path, MAX_PATH)) {
    return {};
  }
  std::wstring path(module_path);
  size_t separator = path.find_last_of(L"\\/");
  std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](wchar_t c) { return wchar_t(std::towlower(c)); });
  return name;
}

}  // namespace

void ApplyNvidiaMaxPerformanceProfile() {
  if (!REXCVAR_GET(nvidia_prefer_max_performance)) {
    return;
  }

  NvApiDrs api;
  if (!api.Load()) {
    // Not an NVIDIA system (or a very old driver) - nothing to do.
    return;
  }
  if (api.initialize() != 0) {
    return;
  }

  std::wstring app_name = GetExecutableBaseNameLower();
  if (app_name.empty()) {
    return;
  }

  NvDrsSessionHandle session = nullptr;
  if (api.create_session(&session) != 0 || !session) {
    REXLOG_WARN("NVIDIA app profile: failed to create a driver settings session");
    return;
  }

  // Everything from here on cleans up through this single exit path.
  do {
    NvStatus status = api.load_settings(session);
    if (status != 0) {
      REXLOG_WARN("NVIDIA app profile: DRS_LoadSettings failed ({})", status);
      break;
    }

    NvApiUnicodeString app_name_nvapi;
    FillNvapiUnicodeString(app_name_nvapi, app_name);

    // Find the profile that owns this executable, creating one if needed.
    NvDrsProfileHandle profile = nullptr;
    auto application = std::make_unique<NvdrsApplicationV1>();
    application->version = MakeNvapiVersion(sizeof(NvdrsApplicationV1), 1);
    status = api.find_application_by_name(session, app_name_nvapi, &profile, application.get());
    if (status != 0 || !profile) {
      profile = nullptr;
      // No profile references the executable yet. Reuse a profile previously
      // created by us if present, otherwise create one.
      NvApiUnicodeString profile_name_nvapi;
      FillNvapiUnicodeString(profile_name_nvapi, app_name);
      if (api.find_profile_by_name(session, profile_name_nvapi, &profile) != 0 || !profile) {
        profile = nullptr;
        auto new_profile = std::make_unique<NvdrsProfileV1>();
        new_profile->version = MakeNvapiVersion(sizeof(NvdrsProfileV1), 1);
        FillNvapiUnicodeString(new_profile->profile_name, app_name);
        status = api.create_profile(session, new_profile.get(), &profile);
        if (status != 0 || !profile) {
          REXLOG_WARN("NVIDIA app profile: DRS_CreateProfile failed ({})", status);
          break;
        }
      }
      auto new_application = std::make_unique<NvdrsApplicationV1>();
      new_application->version = MakeNvapiVersion(sizeof(NvdrsApplicationV1), 1);
      std::memcpy(new_application->app_name, app_name_nvapi, sizeof(NvApiUnicodeString));
      status = api.create_application(session, profile, new_application.get());
      if (status != 0) {
        REXLOG_WARN("NVIDIA app profile: DRS_CreateApplication failed ({})", status);
        break;
      }
    }

    // Skip the save when the profile already requests maximum performance.
    auto setting = std::make_unique<NvdrsSettingV1>();
    setting->version = MakeNvapiVersion(sizeof(NvdrsSettingV1), 1);
    status = api.get_setting(session, profile, kPreferredPstateSettingId, setting.get());
    if (status == 0 && setting->u32_current_value == kPreferredPstatePreferMax &&
        setting->setting_location == 0 /* current profile, not inherited */) {
      REXLOG_INFO(
          "NVIDIA app profile: power management already 'Prefer maximum performance' for {}",
          std::string(app_name.begin(), app_name.end()));
      break;
    }

    auto new_setting = std::make_unique<NvdrsSettingV1>();
    new_setting->version = MakeNvapiVersion(sizeof(NvdrsSettingV1), 1);
    new_setting->setting_id = kPreferredPstateSettingId;
    new_setting->setting_type = 0;  // NVDRS_DWORD_TYPE.
    new_setting->u32_current_value = kPreferredPstatePreferMax;
    status = api.set_setting(session, profile, new_setting.get());
    if (status != 0) {
      REXLOG_WARN("NVIDIA app profile: DRS_SetSetting failed ({})", status);
      break;
    }
    status = api.save_settings(session);
    if (status != 0) {
      REXLOG_WARN("NVIDIA app profile: DRS_SaveSettings failed ({})", status);
      break;
    }
    REXLOG_INFO(
        "NVIDIA app profile: set power management to 'Prefer maximum performance' for {} "
        "(prevents the driver parking the GPU in a low P-state; disable with "
        "nvidia_prefer_max_performance=false)",
        std::string(app_name.begin(), app_name.end()));
  } while (false);

  api.destroy_session(session);
}

}  // namespace rex::graphics

#else  // !REX_PLATFORM_WIN32

namespace rex::graphics {

void ApplyNvidiaMaxPerformanceProfile() {}

}  // namespace rex::graphics

#endif  // REX_PLATFORM_WIN32
