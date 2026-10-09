// Free camera, and console commands to look through guest memory (used to
// find the game's camera).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/draw_overrides.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/ui/overlay/quick_menu.h>

// burstlimit_online.cpp: online stall counter, every game tick.
void BurstLimitOnlineFrame(rex::memory::Memory* memory);
// burstlimit_buttons.cpp: the button icons follow the setting / controller.
void BurstLimitButtonsFrame(rex::memory::Memory* memory);

REXCVAR_DEFINE_BOOL(free_camera, false, "Patches",
                    "Free camera for screenshots: left stick moves, right stick looks, LB/RB "
                    "down/up, LT/RT slower/faster, D-pad up/down zoom, X freezes the game, Y "
                    "back to the game's view, B exits. The game doesn't get the controller "
                    "meanwhile. Works in cinematics too.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(freeze_game, false, "Patches",
                    "Freezes the fight and its cinematics (characters, effects, cutscene "
                    "timelines) while the game keeps drawing, for the free camera. X in the "
                    "free camera toggles it. Offline only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// The camera manager is a static at 0x841B2B20 (pointed to by 0x841B2B10).
// Its active camera at +0x150 is what the game renders from: eye and target as
// x, y, z, 1 at +0x00 and +0x10, the vertical FOV in radians at +0x34, flags
// at +0x38 (bit 0 = changed). The camera mode writes the eye and target it
// wants at +0x10 and +0x20 of the manager, and sub_8216E850 eases the active
// camera toward them every frame.
constexpr uint32_t kManagerPointer = 0x841B2B10;
// The game renders from its own copy of whichever camera is in charge - the
// battle camera, a cinematic's animated one or a bone camera: the [SYS]
// PREDRAW task (sub_8216CAF0) copies the latched source camera *(M+0x2D8)
// into the render camera *(M+0x2E0) and builds the view from it. The free
// camera writes the render camera right there (BurstLimitCameraApply), so it
// works in cinematics too.
constexpr uint32_t kManagerSourceCamera = 0x2D8;
constexpr uint32_t kManagerRenderCamera = 0x2E0;
constexpr uint32_t kCameraEye = 0x00;
constexpr uint32_t kCameraTarget = 0x10;
constexpr uint32_t kCameraRoll = 0x30;
constexpr uint32_t kCameraFov = 0x34;
constexpr uint32_t kCameraFlags = 0x38;

// The battle object ([0x841B5130] -> 0x841B5140) runs the fight's clock in
// the [SYS] MAIN task (sub_82178410): with bit 0x80000000 of its flags at
// +260 - part of the stop mask, and never set by the game - the clock, the
// characters, effects, cutscene scripts and super-attack timelines stop,
// while the drawing tasks and the camera's view keep running.
constexpr uint32_t kBattlePointer = 0x841B5130;
constexpr uint32_t kBattleStep = 252;
constexpr uint32_t kBattleStepRequest = 256;
constexpr uint32_t kBattleFlags = 260;
constexpr uint32_t kBattleFlagFreeze = 0x80000000u;
// Effect step and step / 60, only written by the clock (sub_82110A80).
constexpr uint32_t kEffectStep = 0x840D62C0;
constexpr uint32_t kEffectStepSeconds = 0x840D6AC8;

// X_INPUT_GAMEPAD_* bits.
constexpr uint16_t kPadUp = 0x0001;
constexpr uint16_t kPadDown = 0x0002;
constexpr uint16_t kPadLeft = 0x0004;
constexpr uint16_t kPadRight = 0x0008;
constexpr uint16_t kPadLeftShoulder = 0x0100;
constexpr uint16_t kPadRightShoulder = 0x0200;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;
constexpr uint16_t kPadY = 0x8000;

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 ReadVec3(const uint8_t* p) {
  return {rex::memory::load_and_swap<float>(p), rex::memory::load_and_swap<float>(p + 4),
          rex::memory::load_and_swap<float>(p + 8)};
}

void WriteVec3(uint8_t* p, const Vec3& v) {
  rex::memory::store_and_swap<float>(p, v.x);
  rex::memory::store_and_swap<float>(p + 4, v.y);
  rex::memory::store_and_swap<float>(p + 8, v.z);
  rex::memory::store_and_swap<float>(p + 12, 1.0f);
}

struct PadInput {
  uint16_t buttons = 0;
  float left_x = 0.0f, left_y = 0.0f, right_x = 0.0f, right_y = 0.0f;
  float left_trigger = 0.0f, right_trigger = 0.0f;
};

float StickAxis(int16_t raw) {
  constexpr float kDeadzone = 0.2f;
  const float value = std::clamp(float(raw) / 32767.0f, -1.0f, 1.0f);
  if (std::fabs(value) < kDeadzone) {
    return 0.0f;
  }
  return (value - std::copysign(kDeadzone, value)) / (1.0f - kDeadzone);
}

rex::input::InputSystem* GetInputSystem() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
}

// All controllers, merged: the stick pushed the furthest wins.
PadInput ReadPad(rex::input::InputSystem* input) {
  using rex::X_RESULT;
  PadInput pad;
  float left_distance = 0.0f, right_distance = 0.0f;
  for (uint32_t user = 0; user < rex::input::kMaxGuestUsers; ++user) {
    rex::input::X_INPUT_STATE state = {};
    if (input->GetStateForUI(user, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    const auto& gamepad = state.gamepad;
    pad.buttons |= static_cast<uint16_t>(gamepad.buttons);
    const float lx = StickAxis(gamepad.thumb_lx), ly = StickAxis(gamepad.thumb_ly);
    const float rx = StickAxis(gamepad.thumb_rx), ry = StickAxis(gamepad.thumb_ry);
    if (std::fabs(lx) + std::fabs(ly) > left_distance) {
      left_distance = std::fabs(lx) + std::fabs(ly);
      pad.left_x = lx;
      pad.left_y = ly;
    }
    if (std::fabs(rx) + std::fabs(ry) > right_distance) {
      right_distance = std::fabs(rx) + std::fabs(ry);
      pad.right_x = rx;
      pad.right_y = ry;
    }
    pad.left_trigger = std::max(pad.left_trigger, float(gamepad.left_trigger) / 255.0f);
    pad.right_trigger = std::max(pad.right_trigger, float(gamepad.right_trigger) / 255.0f);
  }
  return pad;
}

// Game thread only (the camera update hook).
struct FreeCamera {
  bool active = false;
  bool blocking_input = false;
  bool show_hud = false;
  bool pad_seen = false;
  uint16_t last_buttons = 0;
  uint16_t exit_buttons = 0;
  Vec3 position;
  float yaw = 0.0f;    // 0 = looking down -Z, positive turns right (+X).
  float pitch = 0.0f;  // Positive looks up.
  float roll = 0.0f;   // Turns the picture (the GPU does it).
  float fov = 0.6f;
  // What BurstLimitCameraApply writes into the render camera.
  Vec3 eye;
  Vec3 target;
  std::chrono::steady_clock::time_point last_update;
} g_free_camera;

// A pose from the free_camera_pose command. The console posts it from its own
// thread and BurstLimitCameraFrame takes it on the game thread, on its first
// tick with the free camera running (it waits until then, e.g. while there's
// no camera yet).
struct PostedPose {
  bool pending = false;
  Vec3 position;
  float yaw = 0.0f;
  float pitch = 0.0f;
  float roll = 0.0f;
  bool has_fov = false;  // Without one the camera keeps its FOV.
  float fov = 0.0f;
};
std::mutex g_posted_pose_mutex;
PostedPose g_posted_pose;  // Guarded by g_posted_pose_mutex.
// Game thread only: free_camera on last tick, to drop a pose still waiting
// when the camera is turned off.
bool g_free_camera_was_on = false;

// Set by the free_camera_where command (console thread): BurstLimitCameraFrame
// logs the pose on its next tick, so it's all read on the game thread.
std::atomic<bool> g_where_requested{false};

// Game thread only. The battle object the freeze bit was set in (0 = none).
uint32_t g_frozen_battle = 0;

void Unfreeze(rex::memory::Memory* memory) {
  if (!g_frozen_battle) {
    return;
  }
  uint8_t* battle = memory->TranslateVirtual<uint8_t*>(g_frozen_battle);
  rex::memory::store_and_swap<uint32_t>(
      battle + kBattleFlags,
      rex::memory::load_and_swap<uint32_t>(battle + kBattleFlags) & ~kBattleFlagFreeze);
  rex::memory::store_and_swap<float>(battle + kBattleStep, 1.0f);
  rex::memory::store_and_swap<float>(battle + kBattleStepRequest, 1.0f);
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStep), 1.0f);
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStepSeconds),
                                     1.0f / 60.0f);
  g_frozen_battle = 0;
}

// Every tick, before the game's tasks run: keeps the fight stopped while
// freeze_game is on (re-applied each tick, so the game's own pause opening or
// closing can't drop it), and lets it go otherwise.
void ApplyFreeze(rex::memory::Memory* memory) {
  const uint32_t battle_address =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kBattlePointer));
  const bool online = rex::net::IsGameSessionOpen();
  if (!REXCVAR_GET(freeze_game) || online || !battle_address) {
    Unfreeze(memory);
    if (REXCVAR_GET(freeze_game) && (online || !battle_address)) {
      // Nothing to freeze (or not allowed): don't stay armed for later.
      rex::cvar::SetFlagByName("freeze_game", "false");
    }
    return;
  }
  if (g_frozen_battle && g_frozen_battle != battle_address) {
    Unfreeze(memory);
  }
  g_frozen_battle = battle_address;
  uint8_t* battle = memory->TranslateVirtual<uint8_t*>(battle_address);
  rex::memory::store_and_swap<uint32_t>(
      battle + kBattleFlags,
      rex::memory::load_and_swap<uint32_t>(battle + kBattleFlags) | kBattleFlagFreeze);
  rex::memory::store_and_swap<float>(battle + kBattleStep, 0.0f);
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStep), 0.0f);
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStepSeconds), 0.0f);
}

Vec3 Forward(float yaw, float pitch) {
  return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

// The free camera's yaw and pitch looking from `eye` to `target`; false (and
// left alone) when they're the same point.
bool LookAngles(const Vec3& eye, const Vec3& target, float& yaw, float& pitch) {
  Vec3 direction{target.x - eye.x, target.y - eye.y, target.z - eye.z};
  const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                 direction.z * direction.z);
  if (!(length > 1e-4f)) {
    return false;
  }
  pitch = std::asin(std::clamp(direction.y / length, -1.0f, 1.0f));
  yaw = std::atan2(direction.x, -direction.z);
  return true;
}

// Points the free camera like the game's camera from `eye` to `target`.
void AimFreeCamera(const Vec3& eye, const Vec3& target) {
  FreeCamera& camera = g_free_camera;
  camera.position = eye;
  LookAngles(eye, target, camera.yaw, camera.pitch);
}

// Game thread: puts the free camera at the pose free_camera_pose posted, if
// there's one, with the HUD hidden as when the free camera starts.
void TakePostedPose() {
  PostedPose pose;
  {
    std::lock_guard<std::mutex> lock(g_posted_pose_mutex);
    if (!g_posted_pose.pending) {
      return;
    }
    pose = g_posted_pose;
    g_posted_pose.pending = false;
  }
  FreeCamera& camera = g_free_camera;
  camera.position = pose.position;
  camera.yaw = pose.yaw;
  camera.pitch = pose.pitch;
  camera.roll = pose.roll;
  if (pose.has_fov) {
    camera.fov = pose.fov;
  }
  camera.show_hud = false;
  rex::graphics::SetHideHudDraws(true);
}

// Game thread: the answer to free_camera_where. The free camera's pose as
// free_camera_pose arguments, then the render camera (what the last frame was
// drawn from - the free camera's view while it's on) and the camera the game
// has in charge, with that one as a pose too.
void LogFreeCameraWhere(rex::memory::Memory* memory) {
  const FreeCamera& camera = g_free_camera;
  if (camera.active) {
    REXLOG_INFO("free_camera_where: free_camera_pose {} {} {} {} {} {} {}", camera.position.x,
                camera.position.y, camera.position.z, camera.yaw, camera.pitch, camera.fov,
                camera.roll);
  } else {
    bool pending;
    {
      std::lock_guard<std::mutex> lock(g_posted_pose_mutex);
      pending = g_posted_pose.pending;
    }
    REXLOG_INFO("free_camera_where: free camera off{}",
                pending ? " (a posted pose is waiting for it to start)" : "");
  }
  const uint32_t manager_address =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kManagerPointer));
  if (!manager_address) {
    REXLOG_INFO("free_camera_where: no game camera");
    rex::FlushLogging();
    return;
  }
  const uint8_t* manager = memory->TranslateVirtual<const uint8_t*>(manager_address);
  const std::pair<const char*, uint32_t> kCameras[] = {
      {"render", kManagerRenderCamera},
      {"game", kManagerSourceCamera},
  };
  for (const auto& [name, offset] : kCameras) {
    const uint32_t camera_address = rex::memory::load_and_swap<uint32_t>(manager + offset);
    if (!camera_address) {
      REXLOG_INFO("free_camera_where: no {} camera", name);
      continue;
    }
    const uint8_t* game_camera = memory->TranslateVirtual<const uint8_t*>(camera_address);
    const Vec3 eye = ReadVec3(game_camera + kCameraEye);
    const Vec3 target = ReadVec3(game_camera + kCameraTarget);
    const float fov = rex::memory::load_and_swap<float>(game_camera + kCameraFov);
    const float roll = rex::memory::load_and_swap<float>(game_camera + kCameraRoll);
    REXLOG_INFO("free_camera_where: {} camera {:08X}: eye {} {} {} target {} {} {} fov {} roll {}",
                name, camera_address, eye.x, eye.y, eye.z, target.x, target.y, target.z, fov,
                roll);
    float yaw = 0.0f, pitch = 0.0f;
    if (offset == kManagerSourceCamera && LookAngles(eye, target, yaw, pitch)) {
      // Its own roll left out: the free camera drops it.
      REXLOG_INFO("free_camera_where: game camera as a pose: free_camera_pose {} {} {} {} {} {}",
                  eye.x, eye.y, eye.z, yaw, pitch, fov);
    }
  }
  rex::FlushLogging();
}

void StopFreeCamera() {
  FreeCamera& camera = g_free_camera;
  rex::graphics::SetHideHudDraws(false);
  rex::graphics::SetSceneProjectionRoll(0.0f);
  camera.roll = 0.0f;
  if (camera.blocking_input) {
    if (auto* input = GetInputSystem()) {
      input->RemoveUIInputBlocker();
    }
    camera.blocking_input = false;
  }
  camera.active = false;
}

// Every tick, after ApplyFreeze: moves the free camera by the controller (or
// to a pose free_camera_pose posted) for BurstLimitCameraApply to write later
// in the tick.
void UpdateFreeCamera(rex::memory::Memory* memory) {
  FreeCamera& camera = g_free_camera;
  const bool on = REXCVAR_GET(free_camera);
  if (!on && g_free_camera_was_on) {
    // Turned off: a pose that never got its camera (no camera yet) isn't kept
    // for the next time. The command turns the camera on after posting, so a
    // new pose is never dropped here.
    std::lock_guard<std::mutex> lock(g_posted_pose_mutex);
    g_posted_pose.pending = false;
  }
  g_free_camera_was_on = on;
  if (!on) {
    if (camera.active) {
      StopFreeCamera();
    }
    return;
  }
  auto* input = GetInputSystem();
  if (!input) {
    return;
  }
  const uint32_t manager_address =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kManagerPointer));
  if (!manager_address) {
    return;
  }
  uint8_t* manager = memory->TranslateVirtual<uint8_t*>(manager_address);
  const uint32_t render_camera_address =
      rex::memory::load_and_swap<uint32_t>(manager + kManagerRenderCamera);
  if (!render_camera_address) {
    return;
  }
  uint8_t* render_camera = memory->TranslateVirtual<uint8_t*>(render_camera_address);

  const auto now = std::chrono::steady_clock::now();
  if (!camera.active) {
    // Start where the game's camera is - the last view it rendered.
    AimFreeCamera(ReadVec3(render_camera + kCameraEye), ReadVec3(render_camera + kCameraTarget));
    camera.fov = rex::memory::load_and_swap<float>(render_camera + kCameraFov);
    camera.active = true;
    camera.show_hud = false;
    rex::graphics::SetHideHudDraws(true);
    camera.pad_seen = false;
    camera.exit_buttons = 0;
    camera.last_update = now;
    input->AddUIInputBlocker();
    camera.blocking_input = true;
  }
  // After the start, so a pose posted before the camera was on isn't replaced
  // by the game's view.
  TakePostedPose();
  const float dt = std::clamp(
      std::chrono::duration<float>(now - camera.last_update).count(), 0.0f, 0.1f);
  camera.last_update = now;

  // The settings menu has the controllers while it's open.
  PadInput pad;
  if (!rex::ui::QuickMenuDialog::IsOpen()) {
    pad = ReadPad(input);
    if (!camera.pad_seen) {
      // What's held when the camera takes over isn't a press.
      camera.pad_seen = true;
      camera.last_buttons = pad.buttons;
    }
  } else {
    camera.pad_seen = false;
  }
  const uint16_t pressed = pad.buttons & ~camera.last_buttons;
  camera.last_buttons = pad.buttons;

  if (pressed & kPadB) {
    camera.exit_buttons |= kPadB;
  }
  // Exit once B is let go, so the game doesn't get the press.
  if (camera.exit_buttons && !(pad.buttons & camera.exit_buttons)) {
    rex::cvar::SetFlagByName("free_camera", "false");
    StopFreeCamera();
    return;
  }
  if (pressed & kPadY) {
    // Back to the camera the game has in charge (battle or cinematic).
    const uint32_t source_address =
        rex::memory::load_and_swap<uint32_t>(manager + kManagerSourceCamera);
    uint8_t* source = source_address ? memory->TranslateVirtual<uint8_t*>(source_address)
                                     : render_camera;
    AimFreeCamera(ReadVec3(source + kCameraEye), ReadVec3(source + kCameraTarget));
    camera.roll = 0.0f;
  }
  if (pressed & kPadX) {
    rex::cvar::SetFlagByName("freeze_game", REXCVAR_GET(freeze_game) ? "false" : "true");
  }

  // Roll.
  constexpr float kRollSpeed = 1.0f;  // Radians per second.
  if (pad.buttons & kPadLeft) {
    camera.roll += kRollSpeed * dt;
  }
  if (pad.buttons & kPadRight) {
    camera.roll -= kRollSpeed * dt;
  }
  camera.roll = std::clamp(camera.roll, -3.1416f, 3.1416f);
  rex::graphics::SetSceneProjectionRoll(camera.roll);
  if (pressed & kPadA) {
    camera.show_hud = !camera.show_hud;
    rex::graphics::SetHideHudDraws(!camera.show_hud);
  }

  // Look.
  constexpr float kLookSpeed = 1.8f;  // Radians per second.
  constexpr float kMaxPitch = 1.45f;
  camera.yaw += pad.right_x * kLookSpeed * dt;
  camera.pitch = std::clamp(camera.pitch + pad.right_y * kLookSpeed * dt, -kMaxPitch, kMaxPitch);

  // Move, relative to where the camera looks.
  float speed = 20.0f;  // Units per second (the fighters start 30 apart).
  speed *= 1.0f + 3.0f * pad.right_trigger;
  speed *= 1.0f - 0.75f * pad.left_trigger;
  const Vec3 forward = Forward(camera.yaw, camera.pitch);
  const Vec3 right{std::cos(camera.yaw), 0.0f, std::sin(camera.yaw)};
  float rise = 0.0f;
  if (pad.buttons & kPadRightShoulder) {
    rise += 1.0f;
  }
  if (pad.buttons & kPadLeftShoulder) {
    rise -= 1.0f;
  }
  camera.position.x += (forward.x * pad.left_y + right.x * pad.left_x) * speed * dt;
  camera.position.y += (forward.y * pad.left_y + rise) * speed * dt;
  camera.position.z += (forward.z * pad.left_y + right.z * pad.left_x) * speed * dt;

  // Zoom.
  if (pad.buttons & kPadUp) {
    camera.fov -= 0.6f * dt;
  }
  if (pad.buttons & kPadDown) {
    camera.fov += 0.6f * dt;
  }
  camera.fov = std::clamp(camera.fov, 0.1f, 1.6f);

  // Written into the render camera by BurstLimitCameraApply later this tick.
  camera.eye = camera.position;
  camera.target = {camera.position.x + forward.x * 10.0f, camera.position.y + forward.y * 10.0f,
                   camera.position.z + forward.z * 10.0f};
}

}  // namespace

// Mid-asm hook in the main loop (sub_822197D0, 0x822198F8), once per game
// tick, right before the task scheduler runs the frame's update and drawing
// tasks (0x8221993C). Also where the freeze is applied.
void BurstLimitCameraFrame(PPCRegister& r30) {
  (void)r30;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  ApplyFreeze(memory);
  UpdateFreeCamera(memory);
  BurstLimitOnlineFrame(memory);
  BurstLimitButtonsFrame(memory);
  // After the update, so it shows a pose free_camera_pose has just posted.
  if (g_where_requested.exchange(false)) {
    LogFreeCameraWhere(memory);
  }
}

// Mid-asm hook in the [SYS] PREDRAW task (sub_8216CAF0, 0x8216CB94), after the
// game has copied its camera in charge into the render camera (r30, view r31)
// and before it builds the view from it: the free camera replaces it there,
// so it works for the battle camera, cinematics and bone cameras alike, and
// effects and billboards follow it.
void BurstLimitCameraApply(PPCRegister& r30, PPCRegister& r31) {
  const FreeCamera& camera = g_free_camera;
  if (!camera.active || r31.u32 != 0 || !r30.u32) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  uint8_t* render_camera = memory->TranslateVirtual<uint8_t*>(r30.u32);
  WriteVec3(render_camera + kCameraEye, camera.eye);
  WriteVec3(render_camera + kCameraTarget, camera.target);
  // The free camera's roll is done on the GPU; drop a cinematic's own.
  rex::memory::store_and_swap<float>(render_camera + kCameraRoll, 0.0f);
  rex::memory::store_and_swap<float>(render_camera + kCameraFov, camera.fov);
  // Changed: the game rebuilds the orientation from eye and target.
  rex::memory::store_and_swap<uint32_t>(
      render_camera + kCameraFlags,
      rex::memory::load_and_swap<uint32_t>(render_camera + kCameraFlags) | 1u);
}

// Mid-asm hook in the [SYS] D_SEQ0 task (sub_82128E40, 0x82128F30): in its
// two-camera branch (split views), r3 is the camera the main view is drawn
// from - the render camera of view 0 while the free camera is on.
void BurstLimitDualCameraView(PPCRegister& r3) {
  if (!g_free_camera.active) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  const uint32_t manager_address =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kManagerPointer));
  if (!manager_address) {
    return;
  }
  const uint32_t render_camera = rex::memory::load_and_swap<uint32_t>(
      memory->TranslateVirtual<uint8_t*>(manager_address) + kManagerRenderCamera);
  if (render_camera) {
    r3.u64 = render_camera;
  }
}

namespace {

std::vector<std::string> SplitArgs(std::string_view args) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < args.size()) {
    while (start < args.size() && args[start] == ' ') {
      ++start;
    }
    size_t end = start;
    while (end < args.size() && args[end] != ' ') {
      ++end;
    }
    if (end > start) {
      parts.emplace_back(args.substr(start, end - start));
    }
    start = end;
  }
  return parts;
}

// Calls `visit(guest_address, host_pointer, size)` for each committed range of
// guest memory.
template <typename Visit>
void ForEachCommittedRange(Visit&& visit) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  uint64_t address = 0x10000;
  while (address < 0x100000000ull) {
    auto* heap = memory->LookupHeap(uint32_t(address));
    if (!heap) {
      address += 0x10000;
      continue;
    }
    rex::memory::HeapAllocationInfo info = {};
    if (!heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
      address += heap->page_size();
      continue;
    }
    const uint64_t region_end = uint64_t(info.base_address) + info.region_size;
    if ((info.state & rex::memory::kMemoryAllocationCommit) &&
        (info.protect & rex::memory::kMemoryProtectRead)) {
      visit(uint32_t(address), memory->TranslateVirtual<const uint8_t*>(uint32_t(address)),
            size_t(region_end - address));
    }
    address = std::max(region_end, address + 4);
  }
}

constexpr size_t kMaxMatches = 40;

// mem_find_words: guest addresses where these big-endian 32-bit words (hex)
// follow each other.
void MemFindWords(std::string_view args) {
  std::vector<uint32_t> words;
  for (const std::string& part : SplitArgs(args)) {
    words.push_back(uint32_t(std::strtoul(part.c_str(), nullptr, 16)));
  }
  if (words.empty()) {
    REXLOG_WARN("mem_find_words: usage: mem_find_words <hex word> [<hex word> ...]");
    return;
  }
  size_t matches = 0;
  ForEachCommittedRange([&](uint32_t address, const uint8_t* host, size_t size) {
    const size_t needed = words.size() * 4;
    for (size_t offset = 0; offset + needed <= size && matches < kMaxMatches; offset += 4) {
      bool match = true;
      for (size_t i = 0; i < words.size(); ++i) {
        if (rex::memory::load_and_swap<uint32_t>(host + offset + i * 4) != words[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        REXLOG_WARN("mem_find_words: {:08X}", address + uint32_t(offset));
        ++matches;
      }
    }
  });
  REXLOG_WARN("mem_find_words: {} match(es)", matches);
  rex::FlushLogging();
}

// mem_find_floats: guest addresses where these floats follow each other, each
// within `tolerance` (the last argument when it starts with ~, default 0.01).
void MemFindFloats(std::string_view args) {
  std::vector<float> values;
  float tolerance = 0.01f;
  for (const std::string& part : SplitArgs(args)) {
    if (part[0] == '~') {
      tolerance = std::strtof(part.c_str() + 1, nullptr);
    } else {
      values.push_back(std::strtof(part.c_str(), nullptr));
    }
  }
  if (values.empty()) {
    REXLOG_WARN("mem_find_floats: usage: mem_find_floats <value> [<value> ...] [~tolerance]");
    return;
  }
  size_t matches = 0;
  ForEachCommittedRange([&](uint32_t address, const uint8_t* host, size_t size) {
    const size_t needed = values.size() * 4;
    for (size_t offset = 0; offset + needed <= size && matches < kMaxMatches; offset += 4) {
      bool match = true;
      for (size_t i = 0; i < values.size(); ++i) {
        const float value = rex::memory::load_and_swap<float>(host + offset + i * 4);
        if (!(std::fabs(value - values[i]) <= tolerance)) {
          match = false;
          break;
        }
      }
      if (match) {
        REXLOG_WARN("mem_find_floats: {:08X}", address + uint32_t(offset));
        ++matches;
      }
    }
  });
  REXLOG_WARN("mem_find_floats: {} match(es)", matches);
  rex::FlushLogging();
}

// mem_dump: <hex address> [count]: 32-bit words as hex and float.
void MemDump(std::string_view args) {
  const std::vector<std::string> parts = SplitArgs(args);
  if (parts.empty()) {
    REXLOG_WARN("mem_dump: usage: mem_dump <hex address> [count]");
    return;
  }
  const uint32_t address = uint32_t(std::strtoul(parts[0].c_str(), nullptr, 16)) & ~3u;
  const uint32_t count =
      std::min<uint32_t>(parts.size() > 1 ? uint32_t(std::strtoul(parts[1].c_str(), nullptr, 0))
                                          : 16u,
                         512u);
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t word_address = address + i * 4;
    auto* heap = memory->LookupHeap(word_address);
    rex::memory::HeapAllocationInfo info = {};
    if (!heap || !heap->QueryRegionInfo(word_address, &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit)) {
      REXLOG_WARN("mem_dump: {:08X}: not committed", word_address);
      break;
    }
    const uint8_t* host = memory->TranslateVirtual<const uint8_t*>(word_address);
    const uint32_t word = rex::memory::load_and_swap<uint32_t>(host);
    float value;
    std::memcpy(&value, &word, sizeof(value));
    REXLOG_WARN("mem_dump: {:08X} (+{:3}): {:08X} {}", word_address, i * 4, word, value);
  }
  rex::FlushLogging();
}

// mem_write: <hex address> <hex value> [1|2|4|f]: writes one big-endian value
// into guest memory (bytes, halfword, word - the default - or a float given as
// a decimal number), for testing.
void MemWrite(std::string_view args) {
  const std::vector<std::string> parts = SplitArgs(args);
  if (parts.size() < 2) {
    REXLOG_WARN("mem_write: usage: mem_write <hex address> <hex value | float> [1|2|4|f]");
    return;
  }
  const uint32_t address = uint32_t(std::strtoul(parts[0].c_str(), nullptr, 16));
  const std::string kind = parts.size() > 2 ? parts[2] : "4";
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  auto* heap = memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info = {};
  if (!heap || !heap->QueryRegionInfo(address, &info) ||
      !(info.state & rex::memory::kMemoryAllocationCommit)) {
    REXLOG_WARN("mem_write: {:08X}: not committed", address);
    return;
  }
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(address);
  if (kind == "f") {
    const float value = std::strtof(parts[1].c_str(), nullptr);
    rex::memory::store_and_swap<float>(host, value);
    REXLOG_WARN("mem_write: {:08X} = {}", address, value);
    return;
  }
  const uint32_t value = uint32_t(std::strtoul(parts[1].c_str(), nullptr, 16));
  if (kind == "1") {
    *host = uint8_t(value);
  } else if (kind == "2") {
    rex::memory::store_and_swap<uint16_t>(host, uint16_t(value));
  } else {
    rex::memory::store_and_swap<uint32_t>(host, value);
  }
  REXLOG_WARN("mem_write: {:08X} = {:X} ({} bytes)", address, value, kind);
}

// mem_save: <hex address> <hex size> <file>: raw guest memory (big-endian, as
// the game sees it) to a file; pages that aren't committed are written as
// zeros, so file offsets stay address - start.
void MemSave(std::string_view args) {
  const std::vector<std::string> parts = SplitArgs(args);
  if (parts.size() < 3) {
    REXLOG_WARN("mem_save: usage: mem_save <hex address> <hex size> <file>");
    return;
  }
  const uint32_t start = uint32_t(std::strtoul(parts[0].c_str(), nullptr, 16)) & ~0xFFFu;
  const uint32_t size = uint32_t(std::strtoul(parts[1].c_str(), nullptr, 16));
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory || !size) {
    return;
  }
  std::ofstream file(parts[2], std::ios::binary | std::ios::trunc);
  if (!file) {
    REXLOG_WARN("mem_save: can't open {}", parts[2]);
    return;
  }
  static const uint8_t kZeros[0x1000] = {};
  uint32_t committed = 0;
  for (uint64_t page = start; page < uint64_t(start) + size; page += 0x1000) {
    const uint32_t address = uint32_t(page);
    auto* heap = memory->LookupHeap(address);
    rex::memory::HeapAllocationInfo info = {};
    if (heap && heap->QueryRegionInfo(address, &info) &&
        (info.state & rex::memory::kMemoryAllocationCommit)) {
      file.write(memory->TranslateVirtual<const char*>(address), 0x1000);
      ++committed;
    } else {
      file.write(reinterpret_cast<const char*>(kZeros), 0x1000);
    }
  }
  REXLOG_WARN("mem_save: {:08X}+{:X} -> {} ({} committed pages)", start, size, parts[2], committed);
  rex::FlushLogging();
}

// pad_press: <buttons> [milliseconds] [user]: holds controller buttons for the
// game, e.g. "pad_press rb" or "pad_press lb+y 300". Names: a b x y lb rb back
// start l3 r3 up down left right, and lt rt (the triggers, pulled fully).
void PadPress(std::string_view args) {
  const std::vector<std::string> parts = SplitArgs(args);
  if (parts.empty()) {
    REXLOG_WARN("pad_press: usage: pad_press <a|b|x|y|lb|rb|lt|rt|back|start|l3|r3|up|down|left|"
                "right>[+...] [ms] [user]");
    return;
  }
  static const std::pair<const char*, uint16_t> kButtons[] = {
      {"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
      {"start", 0x0010}, {"back", 0x0020}, {"l3", 0x0040},   {"r3", 0x0080},
      {"lb", 0x0100},    {"rb", 0x0200},   {"a", 0x1000},    {"b", 0x2000},
      {"x", 0x4000},     {"y", 0x8000},
  };
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  size_t start = 0;
  const std::string& names = parts[0];
  while (start <= names.size()) {
    const size_t end = std::min(names.find('+', start), names.size());
    const std::string name = names.substr(start, end - start);
    bool found = false;
    if (name == "lt") {
      left_trigger = 255;
      found = true;
    } else if (name == "rt") {
      right_trigger = 255;
      found = true;
    }
    for (const auto& [button_name, bit] : kButtons) {
      if (name == button_name) {
        buttons |= bit;
        found = true;
      }
    }
    if (!found) {
      REXLOG_WARN("pad_press: unknown button '{}'", name);
      return;
    }
    start = end + 1;
  }
  const uint32_t ms = parts.size() > 1 ? uint32_t(std::strtoul(parts[1].c_str(), nullptr, 0)) : 150;
  const uint32_t user = parts.size() > 2 ? uint32_t(std::strtoul(parts[2].c_str(), nullptr, 0)) : 0;
  if (auto* input = GetInputSystem()) {
    input->InjectButtons(user, buttons, ms, left_trigger, right_trigger);
    REXLOG_WARN("pad_press: {:04X}{}{} for {} ms on user {}", buttons, left_trigger ? " LT" : "",
                right_trigger ? " RT" : "", ms, user);
  }
}

bool ParseFloat(const std::string& text, float& value) {
  char* end = nullptr;
  value = std::strtof(text.c_str(), &end);
  return end != text.c_str() && *end == '\0' && std::isfinite(value);
}

// free_camera_pose: <x> <y> <z> <yaw> <pitch> [fov|-] [roll]: turns the free
// camera on and puts it there, HUD hidden, for screenshots without a
// controller. Radians, as the free camera has them: yaw 0 looks down -Z and
// positive turns right, positive pitch looks up. Without a FOV (or with -) it
// keeps its own - the game's when it starts; roll is 0 unless given.
// free_camera_where prints these arguments for the current view. The game
// thread takes the pose on its next tick.
void FreeCameraPose(std::string_view args) {
  const std::vector<std::string> parts = SplitArgs(args);
  float values[7] = {};
  bool valid = parts.size() >= 5 && parts.size() <= 7;
  for (size_t i = 0; valid && i < parts.size(); ++i) {
    valid = (i == 5 && parts[i] == "-") || ParseFloat(parts[i], values[i]);
  }
  if (!valid) {
    REXLOG_WARN("free_camera_pose: usage: free_camera_pose <x> <y> <z> <yaw> <pitch> [fov|-] "
                "[roll] (radians)");
    return;
  }
  const bool has_fov = parts.size() > 5 && parts[5] != "-";
  {
    std::lock_guard<std::mutex> lock(g_posted_pose_mutex);
    g_posted_pose.pending = true;
    g_posted_pose.position = {values[0], values[1], values[2]};
    g_posted_pose.yaw = values[3];
    g_posted_pose.pitch = values[4];
    g_posted_pose.has_fov = has_fov;
    g_posted_pose.fov = values[5];
    g_posted_pose.roll = values[6];
  }
  // Posted first: if the camera starts now, it starts there.
  if (!REXCVAR_GET(free_camera)) {
    rex::cvar::SetFlagByName("free_camera", "true");
  }
  if (has_fov) {
    REXLOG_INFO("free_camera_pose: posted {} {} {} {} {} {} {}", values[0], values[1], values[2],
                values[3], values[4], values[5], values[6]);
  } else {
    REXLOG_INFO("free_camera_pose: posted {} {} {} {} {} - {}", values[0], values[1], values[2],
                values[3], values[4], values[6]);
  }
}

// free_camera_where: logs the free camera's pose as free_camera_pose arguments,
// and the game's cameras (on the game thread's next tick).
void FreeCameraWhere() {
  g_where_requested.store(true);
}

// Memory snapshots for finding a value by how it changes (like a cheat
// search): mem_snap <slot>, then mem_diff <slot a> <slot b> <size 1|2|4>
// <value in a|*> <value in b|*>, then mem_narrow <slot> <size> <value> to keep
// the candidates that have that value in another snapshot ("now" = live).
struct SnapshotRange {
  uint32_t address;
  std::vector<uint8_t> bytes;
};
std::vector<std::vector<SnapshotRange>> g_snapshots(8);
std::vector<uint32_t> g_candidates;

void ForEachWritableGuestRange(
    const std::function<void(uint32_t, const uint8_t*, size_t)>& visit) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  // 0x40000000-0xBFFFFFFF: the virtual heaps, the image's data and one view
  // of physical memory (0xC0000000+ are more views of the same memory).
  uint64_t address = 0x40000000;
  while (address < 0xC0000000ull) {
    auto* heap = memory->LookupHeap(uint32_t(address));
    if (!heap) {
      address += 0x10000;
      continue;
    }
    rex::memory::HeapAllocationInfo info = {};
    if (!heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
      address += heap->page_size();
      continue;
    }
    const uint64_t region_end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size,
                                                   0xC0000000ull);
    if ((info.state & rex::memory::kMemoryAllocationCommit) &&
        (info.protect & rex::memory::kMemoryProtectWrite)) {
      visit(uint32_t(address), memory->TranslateVirtual<const uint8_t*>(uint32_t(address)),
            size_t(region_end - address));
    }
    address = std::max(region_end, address + 4);
  }
}

uint32_t LoadValue(const uint8_t* p, uint32_t size) {
  switch (size) {
    case 1:
      return *p;
    case 2:
      return rex::memory::load_and_swap<uint16_t>(p);
    default:
      return rex::memory::load_and_swap<uint32_t>(p);
  }
}

// Value of `address` in snapshot `slot` (-1 = live memory).
bool SnapshotValue(int slot, uint32_t address, uint32_t size, uint32_t& value) {
  if (slot < 0) {
    auto* runtime = rex::Runtime::instance();
    auto* memory = runtime ? runtime->memory() : nullptr;
    if (!memory) {
      return false;
    }
    value = LoadValue(memory->TranslateVirtual<const uint8_t*>(address), size);
    return true;
  }
  const auto& ranges = g_snapshots[size_t(slot)];
  auto it = std::upper_bound(ranges.begin(), ranges.end(), address,
                             [](uint32_t a, const SnapshotRange& r) { return a < r.address; });
  if (it == ranges.begin()) {
    return false;
  }
  --it;
  if (address + size > it->address + it->bytes.size()) {
    return false;
  }
  value = LoadValue(it->bytes.data() + (address - it->address), size);
  return true;
}

int ParseSlot(const std::string& text) {
  if (text == "now") {
    return -1;
  }
  const int slot = std::atoi(text.c_str());
  return slot >= 0 && slot < int(g_snapshots.size()) ? slot : -2;
}

void MemSnap(std::string_view args) {
  const std::vector<std::string> parts = SplitArgs(args);
  const int slot = parts.empty() ? 0 : ParseSlot(parts[0]);
  if (slot < 0) {
    REXLOG_WARN("mem_snap: usage: mem_snap <slot 0-7>");
    return;
  }
  auto& ranges = g_snapshots[size_t(slot)];
  ranges.clear();
  size_t total = 0;
  ForEachWritableGuestRange([&](uint32_t address, const uint8_t* host, size_t size) {
    ranges.push_back({address, std::vector<uint8_t>(host, host + size)});
    total += size;
  });
  REXLOG_WARN("mem_snap: slot {}: {} ranges, {} MB", slot, ranges.size(), total >> 20);
  rex::FlushLogging();
}

bool ParseWanted(const std::string& text, bool& any, uint32_t& value) {
  any = text == "*";
  value = any ? 0 : uint32_t(std::strtoul(text.c_str(), nullptr, 0));
  return true;
}

void PrintCandidates(const char* what) {
  REXLOG_WARN("{}: {} candidate(s)", what, g_candidates.size());
  for (size_t i = 0; i < g_candidates.size() && i < 60; ++i) {
    REXLOG_WARN("{}: {:08X}", what, g_candidates[i]);
  }
  rex::FlushLogging();
}

void MemDiff(std::string_view args) {
  const std::vector<std::string> parts = SplitArgs(args);
  if (parts.size() < 5) {
    REXLOG_WARN("mem_diff: usage: mem_diff <slot a> <slot b> <size 1|2|4> <value a|*> <value b|*>");
    return;
  }
  const int slot_a = ParseSlot(parts[0]), slot_b = ParseSlot(parts[1]);
  const uint32_t size = uint32_t(std::strtoul(parts[2].c_str(), nullptr, 0));
  bool any_a, any_b;
  uint32_t want_a, want_b;
  ParseWanted(parts[3], any_a, want_a);
  ParseWanted(parts[4], any_b, want_b);
  if (slot_a < 0 || slot_b < -1 || (size != 1 && size != 2 && size != 4)) {
    REXLOG_WARN("mem_diff: bad slots or size");
    return;
  }
  g_candidates.clear();
  for (const SnapshotRange& range : g_snapshots[size_t(slot_a)]) {
    for (size_t offset = 0; offset + size <= range.bytes.size(); offset += size) {
      const uint32_t a = LoadValue(range.bytes.data() + offset, size);
      if (!any_a && a != want_a) {
        continue;
      }
      uint32_t b;
      if (!SnapshotValue(slot_b, range.address + uint32_t(offset), size, b) || a == b ||
          (!any_b && b != want_b)) {
        continue;
      }
      g_candidates.push_back(range.address + uint32_t(offset));
    }
  }
  PrintCandidates("mem_diff");
}

void MemNarrow(std::string_view args) {
  const std::vector<std::string> parts = SplitArgs(args);
  if (parts.size() < 3) {
    REXLOG_WARN("mem_narrow: usage: mem_narrow <slot|now> <size 1|2|4> <value>");
    return;
  }
  const int slot = ParseSlot(parts[0]);
  const uint32_t size = uint32_t(std::strtoul(parts[1].c_str(), nullptr, 0));
  const uint32_t want = uint32_t(std::strtoul(parts[2].c_str(), nullptr, 0));
  std::vector<uint32_t> kept;
  for (uint32_t address : g_candidates) {
    uint32_t value;
    if (slot >= -1 && SnapshotValue(slot, address, size, value) && value == want) {
      kept.push_back(address);
    }
  }
  g_candidates = std::move(kept);
  PrintCandidates("mem_narrow");
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(pad_press, PadPress, "Debug",
                            "Hold controller buttons for the game: <a|rb|lb+y...> [ms] [user]");
REXCVAR_DEFINE_COMMAND_ARGS(free_camera_pose, FreeCameraPose, "Debug",
                            "Free camera on, at a pose: <x> <y> <z> <yaw> <pitch> [fov|-] [roll] "
                            "(radians)");
REXCVAR_DEFINE_COMMAND(free_camera_where, FreeCameraWhere, "Debug",
                       "Log the free camera's pose as free_camera_pose arguments, and the game's "
                       "cameras");
REXCVAR_DEFINE_COMMAND_ARGS(mem_snap, MemSnap, "Debug", "Snapshot writable guest memory: <slot>");
REXCVAR_DEFINE_COMMAND_ARGS(mem_diff, MemDiff, "Debug",
                            "Values changed between snapshots: <a> <b> <size> <value a|*> "
                            "<value b|*>");
REXCVAR_DEFINE_COMMAND_ARGS(mem_narrow, MemNarrow, "Debug",
                            "Keep mem_diff candidates with a value: <slot|now> <size> <value>");
REXCVAR_DEFINE_COMMAND_ARGS(mem_save, MemSave, "Debug",
                            "Save guest memory to a file: <hex address> <hex size> <file>");
REXCVAR_DEFINE_COMMAND_ARGS(mem_find_words, MemFindWords, "Debug",
                            "Find big-endian 32-bit words (hex) in guest memory");
REXCVAR_DEFINE_COMMAND_ARGS(mem_find_floats, MemFindFloats, "Debug",
                            "Find floats in guest memory (last argument ~tolerance)");
REXCVAR_DEFINE_COMMAND_ARGS(mem_dump, MemDump, "Debug",
                            "Dump guest memory words: <hex address> [count]");
REXCVAR_DEFINE_COMMAND_ARGS(mem_write, MemWrite, "Debug",
                            "Write guest memory: <hex address> <hex value | float> [1|2|4|f]");
