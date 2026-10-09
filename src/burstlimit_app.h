#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/quick_menu.h>

// burstlimit_patches.cpp
void BurstLimitApplyPostEffectSettings();
// burstlimit_branding.cpp
void BurstLimitBrandingSetup(const std::filesystem::path& game_data_root);
// burstlimit_buttons.cpp
void BurstLimitButtonsSetup(const std::filesystem::path& game_data_root);
// burstlimit_online.cpp
void BurstLimitOnlineSetup();
// burstlimit_saves.cpp
void BurstLimitSavesSetup(const std::filesystem::path& exe_directory,
                          const std::filesystem::path& user_data_root);
void BurstLimitSavesMenu(rex::ui::QuickMenuConfig& menu);
std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateOnlineNotice(rex::ui::ImGuiDrawer* drawer);
// burstlimit_forms.cpp
std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateStartFormTags(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
    const std::filesystem::path& game_data_root);
// burstlimit_mods.cpp
void BurstLimitModsSetup(const std::filesystem::path& exe_directory,
                         const std::filesystem::path& game_data_root);
std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateModsPanel(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
    const std::filesystem::path& game_data_root,
    std::function<void(std::function<void()>)> defer);
// burstlimit_install.cpp
std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateInstaller(
    rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& target,
    std::function<void(std::filesystem::path)> done,
    std::function<void(std::function<void()>)> defer);

class BurstlimitApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<BurstlimitApp>(
        new BurstlimitApp(ctx, "burstlimit", PPCImageConfig));
  }

  // Burst Limit only ships with the Xenos (D3D12) renderer, so use it when no
  // gpu_plugin was configured instead of starting headless.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    BurstLimitApplyPostEffectSettings();
    // Before the game's files are mounted (the mods change what the guest
    // reads) and before the online version string is made (it names them).
    BurstLimitModsSetup(ExeDirectory(), game_data_root());
    BurstLimitOnlineSetup();
    // "Online" instead of "Xbox LIVE": the title art is made in the background.
    BurstLimitBrandingSetup(game_data_root());
    // The button icons of the controller in use (PlayStation / Switch).
    BurstLimitButtonsSetup(game_data_root());
    // Save export / import in the settings menu.
    BurstLimitSavesSetup(ExeDirectory(), user_data_root());
  }

  // The start form tags of the character select (burstlimit_forms.cpp), and
  // the keyboard keys for the free camera (rebindable in the settings menu).
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    start_form_tags_ = BurstLimitCreateStartFormTags(drawer, immediate_drawer(), game_data_root());
    online_notice_ = BurstLimitCreateOnlineNotice(drawer);
    mods_panel_ = BurstLimitCreateModsPanel(
        drawer, immediate_drawer(), game_data_root(),
        [this](std::function<void()> function) {
          app_context().CallInUIThreadDeferred(std::move(function));
        });
    rex::ui::RegisterBind("bind_free_camera", "Insert", "Free camera on/off", [] {
      rex::cvar::SetFlagByName(
          "free_camera", rex::cvar::Query<bool>("free_camera") ? "false" : "true");
    });
    rex::ui::RegisterBind("bind_freeze_game", "Numpad0", "Freeze game on/off", [] {
      rex::cvar::SetFlagByName(
          "freeze_game", rex::cvar::Query<bool>("freeze_game") ? "false" : "true");
    });
  }

  // No game files yet (first start): ask for the disc image and copy its
  // files next to the exe (burstlimit_install.cpp), then start.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    if (!defaults.game_data_root.empty() || !imgui_drawer()) {
      return defaults;
    }
    installer_ = BurstLimitCreateInstaller(
        imgui_drawer(), ExeDirectory() / L"game_data_root",
        [this, defaults, resume](std::filesystem::path root) {
          rex::PathConfig paths = defaults;
          paths.game_data_root = std::move(root);
          installer_.reset();
          resume(paths);
        },
        [this](std::function<void()> function) {
          app_context().CallInUIThreadDeferred(std::move(function));
        });
    return std::nullopt;
  }

  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_free_camera");
    rex::ui::UnregisterBind("bind_freeze_game");
    start_form_tags_.reset();
    online_notice_.reset();
    mods_panel_.reset();
    installer_.reset();
  }

  // The settings menu (F1 or Back + Start on the controller).
  void OnConfigureQuickMenu(rex::ui::QuickMenuConfig& menu) override {
    using Item = rex::ui::QuickMenuItem;
    using Choices = std::vector<std::pair<std::string, std::string>>;
    auto toggle = [](std::string label, std::string cvar, std::string help) {
      Item item;
      item.kind = Item::Kind::kToggle;
      item.label = std::move(label);
      item.cvar = std::move(cvar);
      item.help = std::move(help);
      return item;
    };
    auto choice = [](std::string label, std::string cvar, Choices choices, std::string help) {
      Item item;
      item.kind = Item::Kind::kChoice;
      item.label = std::move(label);
      item.cvar = std::move(cvar);
      item.choices = std::move(choices);
      item.help = std::move(help);
      return item;
    };
    auto key = [](std::string label, std::string cvar, std::string help) {
      Item item;
      item.kind = Item::Kind::kKey;
      item.label = std::move(label);
      item.cvar = std::move(cvar);
      item.help = std::move(help);
      return item;
    };
    auto number = [](std::string label, std::string cvar, double min, double max, double step,
                     std::string help) {
      Item item;
      item.kind = Item::Kind::kNumber;
      item.label = std::move(label);
      item.cvar = std::move(cvar);
      item.min = min;
      item.max = max;
      item.step = step;
      item.help = std::move(help);
      return item;
    };
    const std::vector<std::string> fsr_effects = {"fsr", "fsr2", "fsr3"};

    menu.title = "RECOMP SETTINGS";

    rex::ui::QuickMenuSection& graphics = menu.sections.emplace_back();
    graphics.title = "GRAPHICS";
    Item& resolution = graphics.items.emplace_back(choice(
        "Resolution", "draw_resolution_scale_x",
        {{"1", "1280x720"}, {"2", "2560x1440"}, {"3", "3840x2160 (4K)"}, {"4", "5120x2880"}},
        "The resolution the game renders at. Higher is sharper but needs a faster GPU."));
    resolution.mirrored_cvars = {"draw_resolution_scale_y", "resolution_scale"};
    graphics.items.push_back(choice(
        "Upscaler", "present_effect",
        {{"bilinear", "Off"},
         {"cas", "AMD CAS"},
         {"fsr", "AMD FSR 1"},
         {"fsr2", "AMD FSR 2"},
         {"fsr3", "AMD FSR 3"}},
        "FSR renders below the resolution above and upscales to it, for more FPS. CAS keeps "
        "the resolution and only sharpens."));
    Item& quality = graphics.items.emplace_back(choice(
        "Upscaler quality", "present_fsr_quality_mode",
        {{"auto", "Native"},
         {"nativeaa", "Native AA"},
         {"quality", "Quality"},
         {"balanced", "Balanced"},
         {"performance", "Performance"},
         {"ultra_performance", "Ultra Performance"}},
        "How far below the resolution FSR renders. Lower settings give more FPS and a softer "
        "image."));
    quality.shown_if_cvar = "present_effect";
    quality.shown_if_values = fsr_effects;
    Item& cas_sharpness = graphics.items.emplace_back(
        number("Sharpness", "present_cas_additional_sharpness", 0.0, 1.0, 0.1,
               "Extra sharpening on top of AMD CAS."));
    cas_sharpness.display_scale = 100.0;
    cas_sharpness.format = "%.0f%%";
    cas_sharpness.shown_if_cvar = "present_effect";
    cas_sharpness.shown_if_values = {"cas"};
    // FSR takes a sharpness reduction in stops: 0 = sharpest.
    Item& fsr_sharpness = graphics.items.emplace_back(
        number("Sharpness", "present_fsr_sharpness_reduction", 0.0, 2.0, 0.2,
               "Sharpening after the FSR upscale."));
    fsr_sharpness.display_scale = -50.0;
    fsr_sharpness.display_offset = 100.0;
    fsr_sharpness.format = "%.0f%%";
    fsr_sharpness.shown_if_cvar = "present_effect";
    fsr_sharpness.shown_if_values = fsr_effects;
    graphics.items.push_back(choice(
        "Anti-aliasing", "swap_post_effect",
        {{"none", "Off"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA (strong)"}},
        "Smooths jagged edges. Softens the image a little."));
    graphics.items.push_back(choice(
        "NVIDIA DLSS", "dlss_mode",
        {{"off", "Off"},
         {"dlaa", "DLAA"},
         {"quality", "Quality"},
         {"balanced", "Balanced"},
         {"performance", "Performance"},
         {"ultra_performance", "Ultra Performance"}},
        "NVIDIA's AI anti-aliasing and upscaling for the 3D scene (RTX GPUs). DLAA keeps the "
        "resolution above; Quality to Ultra Performance render below it and upscale the scene "
        "to it, for more FPS, with the HUD drawn at the full resolution. Only whole steps "
        "exist: at 4K, Quality to Performance render at 2560x1440 and Ultra Performance at "
        "1280x720; at 2560x1440 every mode renders at 1280x720; at 1280x720 there's nothing "
        "lower, so every mode is DLAA."));
    Item& dlss_preset = graphics.items.emplace_back(choice(
        "DLSS model", "dlss_preset", {{"k", "K"}, {"l", "L"}, {"m", "M"}},
        "NVIDIA's DLSS models. M is sharper and more stable than K; L is like M but slower "
        "(NVIDIA's choice for Ultra Performance)."));
    dlss_preset.shown_if_cvar = "dlss_mode";
    dlss_preset.shown_if_values = {"dlaa", "quality", "balanced", "performance",
                                   "ultra_performance"};
    graphics.items.push_back(choice(
        "AMD FSR", "fsr_mode",
        {{"off", "Off"},
         {"native_aa", "Native AA"},
         {"quality", "Quality"},
         {"balanced", "Balanced"},
         {"performance", "Performance"},
         {"ultra_performance", "Ultra Performance"}},
        "AMD's anti-aliasing and upscaling for the 3D scene, like NVIDIA DLSS (turning one on "
        "turns the other off): FSR 4 on AMD RX 9000 and RX 7000 GPUs, FSR 3.1.5 on any other. "
        "Native AA keeps the resolution above; Quality to Ultra Performance render below it "
        "and upscale the scene to it, for more FPS, with the HUD drawn at the full resolution. "
        "Only whole steps exist, like DLSS's."));
    Item& fsr_scene_sharpness = graphics.items.emplace_back(
        number("FSR sharpness", "fsr_sharpness", 0.0, 1.0, 0.1,
               "Sharpening after AMD FSR (0 = off)."));
    fsr_scene_sharpness.display_scale = 100.0;
    fsr_scene_sharpness.format = "%.0f%%";
    fsr_scene_sharpness.shown_if_cvar = "fsr_mode";
    fsr_scene_sharpness.shown_if_values = {"native_aa", "quality", "balanced", "performance",
                                           "ultra_performance"};
    graphics.items.push_back(choice(
        "Texture filtering", "anisotropic_override",
        {{"-1", "Game"}, {"1", "1x"}, {"2", "2x"}, {"3", "4x"}, {"4", "8x"}, {"5", "16x"}},
        "Keeps textures sharp when seen at an angle, like the floor."));
    Item& texture_detail = graphics.items.emplace_back(
        number("Texture detail", "texture_lod_bias", -2.0, 0.0, 0.25,
               "Sharper textures on distant surfaces. NVIDIA recommends -1 with DLSS (its "
               "lower render resolution is made up for by itself); without DLSS, below -0.5 "
               "can shimmer."));
    texture_detail.format = "%.2f";
    graphics.items.push_back(toggle(
        "Texture pack", "texture_replace_enabled",
        "HD textures from the textures\\replace folder next to burstlimit.exe."));
    Item& preload = graphics.items.emplace_back(toggle(
        "Preload textures", "texture_replace_preload",
        "Loads the whole texture pack into memory at startup, so textures don't stutter the "
        "first time they show up. Applies after a restart."));
    preload.shown_if_cvar = "texture_replace_enabled";
    preload.shown_if_values = {"true"};

    rex::ui::QuickMenuSection& effects = menu.sections.emplace_back();
    effects.title = "EFFECTS";
    Item& fov = effects.items.emplace_back(
        number("Field of view", "field_of_view", 50.0, 200.0, 5.0,
               "How much of the stage you see. 100% is the original view, higher is wider."));
    fov.format = "%.0f%%";
    effects.items.push_back(toggle("Depth of field", "depth_of_field",
                                   "Blurs the background behind the fighters."));
    effects.items.push_back(toggle(
        "Glow blur", "glow_blur",
        "Soft glow around bright parts. Leaves a halo around the characters at high "
        "resolution."));
    effects.items.push_back(
        toggle("Motion blur", "motion_blur", "Directional blur during fast moves."));
    effects.items.push_back(toggle(
        "Soft filter", "soft_filter",
        "The game's soft look: a slight blur over the whole picture, made for 720p. Above "
        "1280x720 it blurs a lot - keep it off for the real sharpness of the resolution."));

    rex::ui::QuickMenuSection& game = menu.sections.emplace_back();
    game.title = "GAME";
    game.items.push_back(choice(
        "Frame rate", "frame_rate",
        {{"30", "30 FPS"},
         {"60", "60 FPS"},
         {"120", "120 FPS"},
         {"144", "144 FPS"},
         {"unlocked", "Unlocked"}},
        "The most frames per second the game runs at. 30 is the original. Above 60 needs a "
        "monitor with a high refresh rate to see the difference."));
    game.items.push_back(choice(
        "Online input delay", "online_input_delay",
        {{"2", "2 frames"},
         {"3", "3 frames"},
         {"4", "4 frames"},
         {"5", "5 frames"},
         {"6", "6 frames"},
         {"8", "8 frames"},
         {"off", "Game default"}},
        "Online matches (lobby): how many frames your button presses wait so both PCs stay in "
        "step. Lower feels snappier; the fight only pauses when the ping is above it (one "
        "frame = 17 ms: 2 frames ~ up to 25 ms ping one way, 4 frames ~ 55 ms, 6 frames ~ "
        "90 ms, 8 frames ~ 120 ms). The host's choice is used by both players and it can't "
        "change during a session. Game default brings back the older Online input speed "
        "setting (used over Radmin / LAN)."));
    Item& online = game.items.emplace_back(choice(
        "Online input speed", "online_tick_sleep",
        {{"0", "LAN"}, {"1", "Fast"}, {"2", "Normal"}, {"3", "Game default"}},
        "How often online matches exchange inputs over Radmin / LAN, or in the lobby with "
        "Online input delay on Game default: Fast for a good ping, Normal for an average one. "
        "Both players must pick the same."));
    // Only matters when no lobby input delay is picked (Radmin / LAN, or Game default).
    online.shown_if_cvar = "online_input_delay";
    online.shown_if_values = {"off"};
    game.items.push_back(toggle(
        "Ki charge (hold L3)", "ki_charge",
        "Hold L3 (press the left stick) in a fight to power up and fill the Ki gauge faster, "
        "like in Shin Budokai - on the ground or in the air. Let go, get hit or fill the gauge "
        "to stop. Online, the host's setting is used."));
    Item& ki_speed = game.items.emplace_back(choice(
        "Ki charge speed", "ki_charge_rate",
        {{"10", "Slow"}, {"16", "Normal"}, {"25", "Fast"}},
        "How fast holding L3 fills the Ki gauge (Normal: an empty gauge in about 2.4 s). "
        "Online, the host's setting is used."));
    ki_speed.shown_if_cvar = "ki_charge";
    ki_speed.shown_if_values = {"true"};
    game.items.push_back(toggle("Vibration", "vibration", "Controller vibration."));
    game.items.push_back(toggle(
        "Free camera", "free_camera",
        "Fly the camera, also in cinematics (Y in this menu): left stick moves, right stick "
        "looks, LB/RB down/up, LT/RT slower/faster, D-pad zoom and tilt, A HUD, X freeze, Y "
        "resets, B exits."));
    game.items.push_back(toggle(
        "Freeze game", "freeze_game",
        "Stops the fight and its cinematics - characters, effects, cutscenes - while the "
        "picture stays, for the free camera. Offline only."));
    game.items.push_back(key("Free camera key", "bind_free_camera",
                             "Keyboard key that turns the free camera on and off. Press A or "
                             "Enter, then the key to use."));
    game.items.push_back(key("Freeze key", "bind_freeze_game",
                             "Keyboard key that freezes and unfreezes the game. Press A or "
                             "Enter, then the key to use."));
    menu.quick_toggle_label = "Free camera";
    menu.quick_toggle_cvar = "free_camera";

    rex::ui::QuickMenuSection& display = menu.sections.emplace_back();
    display.title = "DISPLAY";
    display.items.push_back(toggle("Fullscreen", "fullscreen", "Borderless fullscreen."));
    display.items.push_back(toggle(
        "Show FPS", "debug_overlay",
        "Frame rate, frame time, render resolution and upscaler in a corner (F3)."));
    Item& fps_position = display.items.emplace_back(choice(
        "FPS position", "debug_overlay_position",
        {{"top-left", "Top left"},
         {"top-right", "Top right"},
         {"bottom-left", "Bottom left"},
         {"bottom-right", "Bottom right"}},
        "Which corner the frame rate panel sits in."));
    fps_position.shown_if_cvar = "debug_overlay";
    fps_position.shown_if_values = {"true"};
    display.items.push_back(choice(
        "Button icons", "button_icons",
        {{"auto", "Auto"},
         {"xbox", "Xbox"},
         {"playstation", "PlayStation"},
         {"switch", "Nintendo Switch"}},
        "Which controller's buttons the game shows in its prompts, tutorials and fight HUD (and "
        "this menu). Auto follows the controller in use (Xbox for the keyboard or an unknown "
        "pad). Applies right away."));
    display.items.push_back(choice(
        "Menu buttons", "quick_menu_buttons",
        {{"back+start", "Back + Start"}, {"l3+r3", "L3 + R3"}, {"none", "Keyboard only"}},
        "Controller buttons that open this menu. F1 on the keyboard always does."));

    // Export / import of the save (burstlimit_saves.cpp).
    BurstLimitSavesMenu(menu);
  }

  static std::filesystem::path ExeDirectory() {
    wchar_t exe_path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
      return std::filesystem::current_path();
    }
    return std::filesystem::path(exe_path).parent_path();
  }

  // Portable build: always load game files beside burstlimit.exe.
  // This intentionally ignores stale game_data_root values from old configs.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    wchar_t exe_path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    if (length == 0 || length >= MAX_PATH) {
      return;
    }

    const auto local_game_root =
        std::filesystem::path(exe_path).parent_path() / L"game_data_root";

    // The folder next to the executable, unless --game_data_root (or the
    // config) names another one.
    if (paths.game_data_root.empty() &&
        std::filesystem::exists(local_game_root / L"default.xex")) {
      paths.game_data_root = local_game_root;
    }
  }

 private:
  std::unique_ptr<rex::ui::ImGuiDialog> start_form_tags_;
  std::unique_ptr<rex::ui::ImGuiDialog> online_notice_;
  std::unique_ptr<rex::ui::ImGuiDialog> mods_panel_;
  std::unique_ptr<rex::ui::ImGuiDialog> installer_;
};
