#include <rex/graphics/gpu_clock_telemetry.h>

#include <mutex>

#include <rex/platform.h>
#include <rex/platform/dynlib.h>

namespace rex::graphics {

namespace {

// Minimal NVML surface, resolved dynamically so there is no build-time
// dependency. nvml.dll ships with the NVIDIA driver into System32; Linux
// driver packages install libnvidia-ml.so.1. Other vendors simply fail the
// load and sampling stays disabled.
using NvmlReturn = int;  // NVML_SUCCESS == 0.
using NvmlDevice = void*;

struct NvmlUtilization {
  unsigned int gpu;
  unsigned int memory;
};

// nvmlClockType_t.
constexpr int kNvmlClockSm = 1;
constexpr int kNvmlClockMem = 2;

struct NvmlApi {
  rex::platform::DynamicLibrary library;
  NvmlReturn (*init)() = nullptr;
  NvmlReturn (*device_get_handle_by_index)(unsigned int, NvmlDevice*) = nullptr;
  NvmlReturn (*device_get_clock_info)(NvmlDevice, int, unsigned int*) = nullptr;
  NvmlReturn (*device_get_utilization_rates)(NvmlDevice, NvmlUtilization*) = nullptr;
  NvmlReturn (*device_get_performance_state)(NvmlDevice, int*) = nullptr;
  NvmlReturn (*device_get_current_clocks_throttle_reasons)(NvmlDevice,
                                                           unsigned long long*) = nullptr;
  NvmlDevice device = nullptr;
  bool ready = false;
};

NvmlApi& GetNvml() {
  static NvmlApi api;
  static std::once_flag once;
  std::call_once(once, [] {
#if REX_PLATFORM_WIN32
    bool loaded = api.library.Load("nvml.dll");
#elif REX_PLATFORM_LINUX
    bool loaded = api.library.Load("libnvidia-ml.so.1");
#else
    bool loaded = false;
#endif
    if (!loaded) {
      return;
    }
    api.init = api.library.GetSymbol<decltype(api.init)>("nvmlInit_v2");
    if (!api.init) {
      api.init = api.library.GetSymbol<decltype(api.init)>("nvmlInit");
    }
    api.device_get_handle_by_index =
        api.library.GetSymbol<decltype(api.device_get_handle_by_index)>(
            "nvmlDeviceGetHandleByIndex_v2");
    if (!api.device_get_handle_by_index) {
      api.device_get_handle_by_index =
          api.library.GetSymbol<decltype(api.device_get_handle_by_index)>(
              "nvmlDeviceGetHandleByIndex");
    }
    api.device_get_clock_info =
        api.library.GetSymbol<decltype(api.device_get_clock_info)>("nvmlDeviceGetClockInfo");
    api.device_get_utilization_rates =
        api.library.GetSymbol<decltype(api.device_get_utilization_rates)>(
            "nvmlDeviceGetUtilizationRates");
    api.device_get_performance_state =
        api.library.GetSymbol<decltype(api.device_get_performance_state)>(
            "nvmlDeviceGetPerformanceState");
    api.device_get_current_clocks_throttle_reasons =
        api.library.GetSymbol<decltype(api.device_get_current_clocks_throttle_reasons)>(
            "nvmlDeviceGetCurrentClocksThrottleReasons");
    if (!api.init || !api.device_get_handle_by_index || !api.device_get_clock_info) {
      return;
    }
    if (api.init() != 0) {
      return;
    }
    // Device 0: NVML enumerates NVIDIA GPUs only, so on single-GPU and hybrid
    // systems alike this is the discrete GPU doing the rendering.
    if (api.device_get_handle_by_index(0, &api.device) != 0 || !api.device) {
      return;
    }
    api.ready = true;
  });
  return api;
}

}  // namespace

GpuClockSample SampleGpuClocks() {
  GpuClockSample sample;
  NvmlApi& api = GetNvml();
  if (!api.ready) {
    return sample;
  }
  unsigned int sm_mhz = 0;
  unsigned int mem_mhz = 0;
  if (api.device_get_clock_info(api.device, kNvmlClockSm, &sm_mhz) != 0) {
    return sample;
  }
  api.device_get_clock_info(api.device, kNvmlClockMem, &mem_mhz);
  sample.sm_mhz = sm_mhz;
  sample.mem_mhz = mem_mhz;
  if (api.device_get_utilization_rates) {
    NvmlUtilization utilization = {};
    if (api.device_get_utilization_rates(api.device, &utilization) == 0) {
      sample.gpu_util_percent = utilization.gpu;
      sample.mem_util_percent = utilization.memory;
    }
  }
  if (api.device_get_performance_state) {
    int pstate = 0;
    if (api.device_get_performance_state(api.device, &pstate) == 0 && pstate >= 0) {
      sample.performance_state = uint32_t(pstate);
    }
  }
  if (api.device_get_current_clocks_throttle_reasons) {
    unsigned long long reasons = 0;
    if (api.device_get_current_clocks_throttle_reasons(api.device, &reasons) == 0) {
      sample.throttle_reasons = reasons;
    }
  }
  sample.valid = true;
  return sample;
}

}  // namespace rex::graphics
