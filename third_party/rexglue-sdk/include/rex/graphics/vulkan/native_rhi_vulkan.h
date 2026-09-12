#pragma once

// Vulkan implementation of the native-render RHI (rex/graphics/native_rhi.h).
// Constructed and owned by the Vulkan command processor; built on the CP's
// deferred command buffer, its barrier queue and its submission counters.
// Unlike the D3D12 backend (a thin passthrough), this backend owns all the
// translation the interface promises: render-pass scoping, descriptor sets
// per the frozen set/binding plan, explicit-state -> (stage, access,
// layout) mapping, and the
// negative-viewport y-flip.

#include <cstdint>

#include <rex/graphics/native_rhi.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

// Creates the device wrapper (call once; destroy with DestroyNativeRhiDevice
// after all GPU work completed - AwaitAllQueueOperationsCompletion).
nrhi::Device* CreateNativeRhiDevice(VulkanCommandProcessor* command_processor);
void DestroyNativeRhiDevice(nrhi::Device* device);

// Per-frame, inside the guest-output refresher (submission open): wraps the
// presenter's guest output image (cached by VkImage identity), drains
// completed retirement, advances the internal root-constant ring region and
// resets the frame Cmd latches. guest_output_ever_written_previously selects
// the wrapper's initial tracked layout (UNDEFINED on the image's first ever
// write, the presenter's internal SHADER_READ_ONLY_OPTIMAL otherwise).
// guest_output_out receives the wrapped texture.
nrhi::Cmd* NativeRhiBeginFrame(nrhi::Device* device, VkImage guest_output_image,
                               VkImageView guest_output_image_view,
                               bool guest_output_ever_written_previously, uint32_t width,
                               uint32_t height, nrhi::Texture** guest_output_out);

// Vulkan-only frame epilogue: ends any render pass still open in the frame
// Cmd, flushes pending clears that no draw consumed, and submits the command
// processor's queued barriers (including the app's release barrier of the
// guest output back to nrhi::ResourceState::kGuestOutput). Call after the
// native render callback, before EndSubmission.
void NativeRhiEndFrame(nrhi::Device* device);

}  // namespace rex::graphics::vulkan
