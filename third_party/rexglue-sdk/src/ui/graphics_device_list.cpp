/**
 * @file        ui/graphics_device_list.cpp
 *
 * @brief       Registry of the graphics devices the active backend enumerated.
 */
#include <rex/ui/graphics_device_list.h>

#include <mutex>
#include <utility>

namespace rex::ui {
namespace {

std::mutex g_graphics_device_list_mutex;
GraphicsDeviceList g_graphics_device_list;

}  // namespace

void SetGraphicsDeviceList(GraphicsDeviceList list) {
  std::lock_guard<std::mutex> lock(g_graphics_device_list_mutex);
  g_graphics_device_list = std::move(list);
}

GraphicsDeviceList GetGraphicsDeviceList() {
  std::lock_guard<std::mutex> lock(g_graphics_device_list_mutex);
  return g_graphics_device_list;
}

}  // namespace rex::ui
