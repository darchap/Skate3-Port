/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>

#include <algorithm>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/flags.h>
#include <rex/logging.h>

namespace rex::filesystem {

namespace {

bool LooksLikeSkaterPreviewAssetPath(std::string_view path) {
  std::string lower(path);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static constexpr std::string_view kNeedles[] = {
      "import_skater",    "team_management", "preset",          "skater",
      "preview",          "fedynamic",       "fedata",          "fetexture",
      "createacharacter", "db.big",          "data/fe",         "data\\fe",
  };
  for (std::string_view needle : kNeedles) {
    if (lower.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool LooksLikeTeamProfileBackgroundPath(std::string_view path) {
  std::string lower(path);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find("team_profile_background_0") != std::string::npos;
}

bool ConsumeFeAssetLogBudget() {
  const int32_t remaining = REXCVAR_GET(filesystem_debug_log_fe_asset_ops_remaining);
  if (remaining <= 0) {
    return false;
  }
  REXCVAR_SET(filesystem_debug_log_fe_asset_ops_remaining, remaining - 1);
  return true;
}

bool ConsumeTeamProfileBackgroundLogBudget() {
  const int32_t remaining = REXCVAR_GET(filesystem_debug_log_team_profile_background_remaining);
  if (remaining <= 0) {
    return false;
  }
  REXCVAR_SET(filesystem_debug_log_team_profile_background_remaining, remaining - 1);
  return true;
}

}  // namespace

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {}

HostPathFile::~HostPathFile() = default;

void HostPathFile::Destroy() {
  delete this;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {
    if (entry_ && LooksLikeTeamProfileBackgroundPath(entry_->absolute_path()) &&
        ConsumeTeamProfileBackgroundLogBudget()) {
      REXFS_WARN(
          "Team profile BG diagnostic: op=read, path='{}', offset={:X}, requested={:X}, read={:X}",
          entry_->absolute_path(), byte_offset, buffer.size(),
          out_bytes_read ? *out_bytes_read : 0);
    }
    if (entry_ && LooksLikeSkaterPreviewAssetPath(entry_->absolute_path()) &&
        ConsumeFeAssetLogBudget()) {
      REXFS_WARN(
          "FE asset diagnostic: op=read, path='{}', offset={:X}, requested={:X}, read={:X}",
          entry_->absolute_path(), byte_offset, buffer.size(),
          out_bytes_read ? *out_bytes_read : 0);
    }
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace rex::filesystem
