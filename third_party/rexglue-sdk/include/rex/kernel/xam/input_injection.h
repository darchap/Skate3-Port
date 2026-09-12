#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::kernel::xam {

struct SyntheticInputStep {
  uint16_t buttons;
  uint32_t poll_count;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
};

void QueueSyntheticInput(uint16_t buttons, uint32_t poll_count);
void QueueSyntheticInput(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger,
                         uint32_t poll_count);
void QueueSyntheticInputSequence(const SyntheticInputStep* steps, size_t step_count);
void SetSyntheticAutoTap(uint16_t buttons, bool enabled);
void ClearSyntheticInput();
// Permanently hands input back to the physical controller for this process.
// Unlike ClearSyntheticInput, a later boot-flow context update cannot re-arm
// the dialog auto-tap after this has been called.
void CompleteSyntheticBootAutomation();
bool IsSyntheticBootAutomationComplete();

}  // namespace rex::kernel::xam
