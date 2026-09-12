/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <mutex>

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

namespace {

constexpr size_t kMaxSyntheticInputSteps = 16;

std::atomic<bool> g_synthetic_input_active{false};
std::atomic<bool> g_synthetic_boot_automation_complete{false};
std::array<std::atomic<uint16_t>, 4> g_last_logged_buttons{};
std::mutex g_synthetic_input_mutex;
std::array<SyntheticInputStep, kMaxSyntheticInputSteps> g_synthetic_input_steps{};
size_t g_synthetic_input_step_count = 0;
size_t g_synthetic_input_step_index = 0;
uint32_t g_synthetic_input_step_remaining = 0;
uint16_t g_synthetic_auto_tap_buttons = 0;
bool g_synthetic_auto_tap_enabled = false;

void QueueSyntheticInputSequenceLocked(const SyntheticInputStep* steps, size_t step_count) {
  g_synthetic_input_step_count = std::min(step_count, kMaxSyntheticInputSteps);
  g_synthetic_input_step_index = 0;
  for (size_t i = 0; i < g_synthetic_input_step_count; ++i) {
    g_synthetic_input_steps[i] = steps[i];
  }
  while (g_synthetic_input_step_count != 0 &&
         g_synthetic_input_steps[g_synthetic_input_step_count - 1].poll_count == 0) {
    --g_synthetic_input_step_count;
  }
  g_synthetic_input_step_remaining =
      g_synthetic_input_step_count ? g_synthetic_input_steps[0].poll_count : 0;
  g_synthetic_input_active.store(g_synthetic_input_step_count != 0 &&
                                     g_synthetic_input_step_remaining != 0,
                                 std::memory_order_relaxed);
}

void EnsureSyntheticAutoTapLocked() {
  if (!g_synthetic_auto_tap_enabled || g_synthetic_auto_tap_buttons == 0 ||
      g_synthetic_input_active.load(std::memory_order_relaxed)) {
    return;
  }

  const SyntheticInputStep auto_tap[] = {
      {g_synthetic_auto_tap_buttons, 5},
      {0, 15},
  };
  QueueSyntheticInputSequenceLocked(auto_tap, std::size(auto_tap));
}

void ApplySyntheticInput(X_INPUT_STATE* input_state) {
  if (!input_state) {
    return;
  }

  std::lock_guard lock(g_synthetic_input_mutex);
  EnsureSyntheticAutoTapLocked();
  if (!g_synthetic_input_active.load(std::memory_order_relaxed) ||
      g_synthetic_input_step_index >= g_synthetic_input_step_count) {
    g_synthetic_input_active.store(false, std::memory_order_relaxed);
    return;
  }

  const auto& step = g_synthetic_input_steps[g_synthetic_input_step_index];
  if (step.buttons != 0 || step.left_trigger != 0 || step.right_trigger != 0) {
    input_state->gamepad.buttons =
        static_cast<uint16_t>(static_cast<uint16_t>(input_state->gamepad.buttons) | step.buttons);
    input_state->gamepad.left_trigger =
        std::max(input_state->gamepad.left_trigger, step.left_trigger);
    input_state->gamepad.right_trigger =
        std::max(input_state->gamepad.right_trigger, step.right_trigger);
    input_state->packet_number =
        static_cast<uint32_t>(static_cast<uint32_t>(input_state->packet_number) + 1);
  }

  if (g_synthetic_input_step_remaining > 0) {
    --g_synthetic_input_step_remaining;
  }

  while (g_synthetic_input_step_remaining == 0) {
    ++g_synthetic_input_step_index;
    if (g_synthetic_input_step_index >= g_synthetic_input_step_count) {
      g_synthetic_input_active.store(false, std::memory_order_relaxed);
      return;
    }
    g_synthetic_input_step_remaining =
        g_synthetic_input_steps[g_synthetic_input_step_index].poll_count;
  }
}

bool PopSyntheticKeystroke(X_INPUT_KEYSTROKE* out_keystroke) {
  (void)out_keystroke;
  return false;
}

}  // namespace

void QueueSyntheticInput(uint16_t buttons, uint32_t poll_count) {
  SyntheticInputStep step{buttons, poll_count};
  QueueSyntheticInputSequence(&step, 1);
}

void QueueSyntheticInput(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger,
                         uint32_t poll_count) {
  SyntheticInputStep step{buttons, poll_count, left_trigger, right_trigger};
  QueueSyntheticInputSequence(&step, 1);
}

void QueueSyntheticInputSequence(const SyntheticInputStep* steps, size_t step_count) {
  if (!steps || step_count == 0) {
    return;
  }

  std::lock_guard lock(g_synthetic_input_mutex);
  QueueSyntheticInputSequenceLocked(steps, step_count);
}

void SetSyntheticAutoTap(uint16_t buttons, bool enabled) {
  std::lock_guard lock(g_synthetic_input_mutex);
  if (enabled && g_synthetic_boot_automation_complete.load(std::memory_order_relaxed)) {
    return;
  }
  g_synthetic_auto_tap_buttons = buttons;
  g_synthetic_auto_tap_enabled = enabled && buttons != 0;
}

void ClearSyntheticInput() {
  std::lock_guard lock(g_synthetic_input_mutex);
  g_synthetic_input_step_count = 0;
  g_synthetic_input_step_index = 0;
  g_synthetic_input_step_remaining = 0;
  g_synthetic_auto_tap_buttons = 0;
  g_synthetic_auto_tap_enabled = false;
  g_synthetic_input_active.store(false, std::memory_order_relaxed);
}

void CompleteSyntheticBootAutomation() {
  g_synthetic_boot_automation_complete.store(true, std::memory_order_relaxed);
  ClearSyntheticInput();
}

bool IsSyntheticBootAutomationComplete() {
  return g_synthetic_boot_automation_complete.load(std::memory_order_relaxed);
}

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

void XamResetInactivity_entry() {
  // Do we need to do anything?
}

u32 XamEnableInactivityProcessing_entry(u32 unk, u32 enable) {
  return X_ERROR_SUCCESS;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  REXKRNL_TRACE("[XAM] XamInputGetCapabilities called: user={}, flags=0x{:X}", (uint32_t)user_index,
                (uint32_t)flags);
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.
  static int call_count = 0;
  if (++call_count <= 5) {
    REXKRNL_TRACE("[XAM] XamInputGetState called: user={}, flags=0x{:X}", (uint32_t)user_index,
                  (uint32_t)flags);
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  const u32 result = is->GetState(actual_user_index, input_state);
  if (result == X_ERROR_SUCCESS) {
    ApplySyntheticInput(input_state);
    if (input_state && actual_user_index < g_last_logged_buttons.size()) {
      const uint16_t buttons = static_cast<uint16_t>(input_state->gamepad.buttons);
      const uint16_t previous =
          g_last_logged_buttons[actual_user_index].exchange(buttons, std::memory_order_relaxed);
      if (buttons != previous) {
        REXKRNL_INFO("XAM input bridge edge: user={} buttons=0x{:04X} packet={}",
                     actual_user_index, buttons,
                     static_cast<uint32_t>(input_state->packet_number));
      }
    }
  }
  return result;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
u32 XamInputSetState_entry(u32 user_index, u32 unk, ppc_ptr_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->SetState(actual_user_index, vibration);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  if (actual_user_index == 0 && PopSyntheticKeystroke(keystroke)) {
    return X_ERROR_SUCCESS;
  }

  auto* is = input_system();
  return is->GetKeystroke(actual_user_index, flags, keystroke);
}

// Same as non-ex, just takes a pointer to user index.
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    user_index = 0;
  }

  auto* is = input_system();
  auto result = is->GetKeystroke(user_index, flags, keystroke);
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if (!user_index || (user_index & 0xFF) == 0xFF) {
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
REX_EXPORT(__imp__XamEnableInactivityProcessing,
           rex::kernel::xam::XamEnableInactivityProcessing_entry)
REX_EXPORT(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
REX_EXPORT(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
REX_EXPORT(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
REX_EXPORT(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
REX_EXPORT(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)

REX_EXPORT_STUB(__imp__XamInputControl);
REX_EXPORT_STUB(__imp__XamInputEnableAutobind);
REX_EXPORT_STUB(__imp__XamInputGetDeviceStats);
REX_EXPORT_STUB(__imp__XamInputGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XamInputGetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHud);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHudEx);
REX_EXPORT_STUB(__imp__XamInputGetUserVibrationLevel);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRawEx);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRawEx);
REX_EXPORT_STUB(__imp__XamInputRawState);
REX_EXPORT_STUB(__imp__XamInputResetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XamInputSendXenonButtonPress);
REX_EXPORT_STUB(__imp__XamInputSetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputSetKeyboardTranslationHud);
REX_EXPORT_STUB(__imp__XamInputSetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSetMinMaxAuthDelay);
REX_EXPORT_STUB(__imp__XamInputSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XamInputToggleKeyLocks);
