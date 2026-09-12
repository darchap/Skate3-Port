/**
 * @file        rex/ui/fonts_inter.h
 *
 * @brief       Embedded Inter font (Latin subset) for styled overlays.
 *
 * Inter is licensed under the SIL Open Font License 1.1 - full license text
 * in fonts_inter.cpp. The data is in Dear ImGui's compressed base85 format
 * for ImFontAtlas::AddFontFromMemoryCompressedBase85TTF.
 */
#pragma once

namespace rex::ui {

const char* GetInterRegularCompressedBase85();
const char* GetInterSemiBoldCompressedBase85();
const char* GetInterBoldCompressedBase85();

}  // namespace rex::ui
