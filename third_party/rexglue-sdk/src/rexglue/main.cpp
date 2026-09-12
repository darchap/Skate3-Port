/**
 * @file        rexglue/main.cpp
 * @brief       ReXGlue CLI tool entry point
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "cli_utils.h"
#include "commands/codegen_command.h"
#include "commands/init_command.h"
#include "commands/test_recompiler.h"
#include "ui/ui.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/result.h>
#include <rex/runtime.h>
#include <rex/version.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

std::string TitleString() {
  return fmt::format("ReXGlue v{} - Xbox 360 Recompilation Toolkit", REXGLUE_VERSION_STRING);
}

bool IsStderrTty() {
#ifdef _WIN32
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr)) != 0;
#endif
}

bool ColorEnabled(bool tty) {
  if (const char* nc = std::getenv("NO_COLOR"); nc && *nc)
    return false;
  return tty;
}

void ConfigureLogging(const std::string& level, const std::string& log_file, bool verbose) {
  std::map<std::string, std::string> category_levels;
  auto config = rex::BuildLogConfig(log_file.empty() ? nullptr : log_file.c_str(),
                                    verbose ? "trace" : level, category_levels);
  config.log_to_console = true;
  rex::InitLogging(config);
  rex::RegisterLogLevelCallback();
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{TitleString(), "rexglue"};
  app.set_version_flag("--version", REXGLUE_VERSION_STRING);
  app.require_subcommand(1);

  rexglue::cli::CliContext ctx;
  std::string log_level = "info";
  std::string log_file;
  bool verbose = false;
  bool force = false;

  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error)")
      ->type_name("LEVEL");
  app.add_option("--log-file", log_file, "Append diagnostics to file")->type_name("PATH");
  app.add_flag("-v,--verbose", verbose, "Equivalent to --log-level=trace");
  app.add_flag("-f,--force", force, "Skip confirmations and proceed");

  rex::InitLoggingEarly();
  rex::cvar::ApplyEnvironment();

  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterCodegen(app, ctx, pending);
  rexglue::cli::RegisterInit(app, ctx, pending);
  rexglue::cli::RegisterRecompileTests(app, ctx, pending);

  std::string dump_xex_path;
  std::string dump_output_dir;
  auto* dump_xex =
      app.add_subcommand("dump-xex", "Load, decrypt/decompress and dump a XEX image");
  dump_xex->add_option("xex", dump_xex_path, "Path to the XEX file")->required();
  dump_xex->add_option("output", dump_output_dir, "Directory for decoded image dumps")->required();
  dump_xex->callback([&] {
    pending = [&]() -> rex::Result<void> {
      namespace fs = std::filesystem;
      std::error_code ec;
      fs::path xex_path = fs::weakly_canonical(fs::absolute(dump_xex_path), ec);
      if (ec || !fs::is_regular_file(xex_path)) {
        return rex::Err<void>(rex::ErrorCategory::IO,
                              fmt::format("XEX file not found: {}", dump_xex_path));
      }
      fs::path output_dir = fs::weakly_canonical(fs::absolute(dump_output_dir), ec);
      if (ec) {
        output_dir = fs::absolute(dump_output_dir);
      }

#ifdef _WIN32
      _putenv_s("REXGLUE_DUMP_XEX_IMAGE_DIR", output_dir.string().c_str());
#else
      setenv("REXGLUE_DUMP_XEX_IMAGE_DIR", output_dir.string().c_str(), 1);
#endif

      rex::Runtime runtime(xex_path.parent_path(), {}, {}, output_dir);
      rex::X_STATUS status = runtime.Setup(rex::RuntimeConfig{
          .kernel_init = rex::kernel::InitializeKernel,
          .tool_mode = true,
      });
      if (status != 0) {
        return rex::Err<void>(rex::ErrorCategory::IO,
                              fmt::format("Failed to initialize runtime: {:#x}", status));
      }

      const std::string guest_path = "game:\\" + xex_path.filename().string();
      status = runtime.LoadXexImage(guest_path);
      runtime.Shutdown();
      if (status != 0) {
        return rex::Err<void>(rex::ErrorCategory::IO,
                              fmt::format("Failed to load XEX image: {:#x}", status));
      }
      return rex::Ok();
    };
  });

  CLI11_PARSE(app, argc, argv);

  ConfigureLogging(log_level, log_file, verbose);
  ctx.verbose = verbose;
  ctx.overwrite_existing = force;
  ctx.generate_despite_errors = force;
  ctx.skip_upgrade_consent = force;

  bool tty = IsStderrTty();
  rexglue::ui::Init({.tty = tty, .color = ColorEnabled(tty)});
  rexglue::ui::Banner(TitleString());

  auto start = std::chrono::steady_clock::now();
  rex::Result<void> result = pending ? pending() : rex::Ok();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  int exit_code = 0;
  if (!result) {
    rexglue::ui::FailureSummary(result.error().what(), elapsed);
    exit_code = result.error().category == rex::ErrorCategory::UserAbort ? 2 : 1;
  } else {
    rexglue::ui::DoneSummary(elapsed);
  }
  rexglue::ui::Shutdown();
  return exit_code;
}
