/**
 * @file        ui/overlay/install_wizard_overlay.cpp
 *
 * @brief       Generic pre-runtime installer dialog.
 */
#include <rex/ui/overlay/install_wizard_overlay.h>

#include <algorithm>
#include <utility>

#include <imgui.h>

#include "wizard_screen.h"

namespace rex::ui {

InstallWizardDialog::InstallWizardDialog(ImGuiDrawer* drawer, std::string title,
                                         std::string section_label, std::string intro,
                                         std::string install_directory,
                                         PickSourceCallback pick_source, InstallCallback install,
                                         CompleteCallback complete)
    : ImGuiDialog(drawer),
      title_(std::move(title)),
      section_label_(std::move(section_label)),
      intro_(std::move(intro)),
      install_directory_(std::move(install_directory)),
      pick_source_(std::move(pick_source)),
      install_(std::move(install)),
      complete_(std::move(complete)),
      status_(intro_) {}

void InstallWizardDialog::OnClose() {
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
}

void InstallWizardDialog::PickSourceAndInstall() {
  if (!pick_source_) {
    return;
  }
  auto source_path = pick_source_();
  // The modal picker swallows the release of whatever input activated this
  // action; balance ImGui's state so the stuck "down" doesn't eat the next
  // press.
  ImGuiIO& io = ImGui::GetIO();
  io.AddMouseButtonEvent(0, false);
  io.AddKeyEvent(ImGuiKey_Enter, false);
  io.AddKeyEvent(ImGuiKey_KeypadEnter, false);
  io.AddKeyEvent(ImGuiKey_Space, false);
  if (source_path.empty()) {
    return;
  }
  StartInstall(std::move(source_path));
}

void InstallWizardDialog::StartInstall(std::filesystem::path source_path) {
  if (install_thread_.joinable()) {
    install_thread_.join();
  }

  source_path_ = std::move(source_path);
  copied_bytes_ = 0;
  total_bytes_ = 0;
  install_done_ = false;
  install_ok_ = false;
  error_.clear();
  state_ = State::kInstalling;
  status_ = "Installing game files...";

  install_thread_ = std::thread([this]() {
    std::string error;
    const bool ok = install_ && install_(source_path_, copied_bytes_, total_bytes_, error);
    error_ = std::move(error);
    install_ok_ = ok;
    install_done_ = true;
  });
}

void InstallWizardDialog::FinishInstallIfNeeded() {
  if (state_ != State::kInstalling || !install_done_.load(std::memory_order_acquire)) {
    return;
  }

  if (install_thread_.joinable()) {
    install_thread_.join();
  }

  if (install_ok_.load(std::memory_order_acquire)) {
    state_ = State::kInstalled;
    status_ = "Installation complete.";
  } else {
    state_ = State::kFailed;
    status_ = "Installation failed.";
  }
}

void InstallWizardDialog::OnDraw(ImGuiIO& io) {
  FinishInstallIfNeeded();

  // The completion callback hands off to the (lengthy) game boot, freezing
  // the last presented frame. Acknowledge the activation visually first:
  // draw frames with the action row gone and a launch status, and only
  // invoke the callback once one has presented - AFTER this frame's draw,
  // so no empty frame flashes between this screen and whatever follows.
  bool run_complete = false;
  if (launch_frames_ >= 0) {
    if (launch_frames_ == 0) {
      launch_frames_ = -1;
      run_complete = true;
    } else {
      --launch_frames_;
    }
  }

  WizardScreenSpec spec;
  spec.title = title_.c_str();
  spec.section = section_label_.c_str();
  spec.paragraphs.push_back({status_, WizardScreenSpec::Emphasis::kNormal});
  if (state_ == State::kFailed && !error_.empty()) {
    spec.paragraphs.push_back({error_, WizardScreenSpec::Emphasis::kDanger});
  }
  spec.info_rows.push_back({"Install Directory", install_directory_});
  if (!source_path_.empty()) {
    spec.info_rows.push_back({"Source", source_path_.string()});
  }
  if (state_ == State::kInstalling) {
    spec.show_progress = true;
    spec.progress_copied = copied_bytes_.load(std::memory_order_relaxed);
    spec.progress_total = total_bytes_.load(std::memory_order_relaxed);
  }
  if (state_ == State::kWaitingForSource || state_ == State::kFailed) {
    spec.actions.push_back("Select ISO");
  } else if (state_ == State::kInstalled && launch_frames_ < 0) {
    spec.actions.push_back("Start Game");
  }

  const int activated =
      DrawWizardScreen(imgui_drawer(), io, spec, focus_index_, highlight_anim_y_);
  if (run_complete) {
    auto complete = std::move(complete_);
    Close();
    if (complete) {
      complete();
    }
    return;
  }
  if (activated == 0) {
    if (state_ == State::kInstalled) {
      status_ = "Starting the game...";
      launch_frames_ = 1;
    } else {
      PickSourceAndInstall();
    }
  }
}

}  // namespace rex::ui
