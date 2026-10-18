// Starting a match already transformed (Goku as a Super Saiyan, Frieza in his
// final form, ...), like some Z Chronicles battles do. The form is picked on
// the character select screen: RB / LB change it for the character under the
// cursor, next to Y's "Change Color", and a tag under the name shows it with
// the form's face (the battle HUD's, read from the game's archive).
// Online (lobby mode) each console only sees its own side's RB / LB (the host
// is side 0 / player 1 on both, the guest side 1 / player 2), so each one
// sends its side's forms to the other over the connection's side channel
// (kind 17) while the character select runs, and both load the same forms.
// Both consoles must start every fighter in the same form: a fighter one
// console starts higher can transform on the other console when it can't on
// this one, and the game's transformations wait for both consoles to load the
// new form - the transformation then repeats until the connection is dropped
// (GitHub issue #9). So online the forms come only from this match's
// exchange: the local side's own choice (sent every 250 ms, whatever was
// pressed) and the other side's table received during this character select -
// never what this process kept from earlier offline play or other opponents.
// The older direct LAN / VPN online keeps the normal forms.
//
// The match request (address at 0x841B5138, filled by the character select or
// a Z Chronicles battle) has an entry of 0x50 bytes per player:
//   +0x0 u16 character (0 Goku, 1 Kid Gohan, ... 20 Bardock)
//   +0x4 u16 highest form the character can transform into this match
//   +0x6 u16 form the character starts in - 0 in Versus / Training, set by
//          Z Chronicles battles (Goku vs Frieza: Goku 2 = SS, Frieza 3 = FINAL)
//   +0x12 u16 the start form again in those battles
// and the first entry has the battle mode at +196 (0 Z Chronicles, 2 Training).
// The character loader, sub_82181BB0, loads the model of form 0 and, when +6
// is above 0, the model of the start form too (other forms load when the
// character transforms). So the start form goes in right before it reads +6.
//
// Character select: sub_82249140 runs the screen (state at +0 of its object:
// 2 while the players pick) and each side's cursor (0 = left / 1P, 1 = right /
// 2P or CPU). Its object keeps, as 32-bit words:
//   [3 + side]                   cursor position in the side's list
//   [8 + side * 32 + position]   character in that slot (21 = RANDOM; the game
//                                takes 100 off values of 100 and up)
// The menu task that runs the screen (Versus, Training, ...) keeps a block of
// 0x60 bytes per side at 0x84249080 with the side's step at +8: 0 waiting,
// 1 picking the character, 2-7 the Ultimate / Drama Piece windows, 100 done.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/online.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/overlay_text.h>

#include "burstlimit_cpk.h"

// The character data gives Goku 4 costumes, Kid Gohan 4 and Teen Gohan 3, but
// the character select caps them at 2, 3 and 2: the others are only worn in Z
// Chronicles - Goku battle-damaged (vs 100% Frieza) and as Ginyu (green
// scouter), Kid Gohan in his Raditz-saga outfit, Teen Gohan battle-damaged
// (Cell Games). Their models, forms and motions are all in the game.
REXCVAR_DEFINE_BOOL(start_forms, true, "Patches",
                    "RB / LB on the character select pick the form a character starts the match "
                    "in. Online, the host's setting is used by both players");
REXCVAR_DEFINE_BOOL(story_costumes, true, "Patches",
                    "The Z Chronicles-only costumes on the character select (Y / Change Color): "
                    "Goku battle-damaged and as Ginyu, Kid Gohan's Raditz-saga outfit, Teen "
                    "Gohan battle-damaged");

// burstlimit_buttons.cpp: the button icons of the controller in use.
int BurstLimitButtonStyle();
bool BurstLimitRestyleWindowIcon(int index, int style, BurstLimitTexture& image);
// burstlimit_slots.cpp: an extra character's forms (its own highest form,
// start form 0) for a player that is one this match; true then.
bool BurstLimitSlotsStartForm(uint32_t player, uint32_t entry);

namespace {

constexpr uint32_t kRequestCharacter = 0x0;
constexpr uint32_t kRequestMaxForm = 0x4;
constexpr uint32_t kRequestStartForm = 0x6;
constexpr uint32_t kRequestStartForm2 = 0x12;
constexpr uint32_t kRequestStride = 0x50;
constexpr uint32_t kRequestMode = 196;
constexpr uint32_t kModeZChronicles = 0;

constexpr uint32_t kScreenPicking = 2;
constexpr uint32_t kSideBlocks = 0x84249080;
constexpr uint32_t kSideBlockStride = 0x60;
constexpr uint32_t kSideStep = 0x8;
constexpr uint32_t kStepPicking = 1;
constexpr uint32_t kStepDone = 100;

constexpr uint32_t kCharacterCount = 21;
constexpr uint32_t kButtonLB = 0x0100;
constexpr uint32_t kButtonRB = 0x0200;

// The forms each character can start in, in the game's order (its own names,
// in the tables at 0x826B9728: NORMAL, KAIOKEN, SS, ...).
constexpr int kMaxForms = 5;
constexpr const char* kForms[kCharacterCount][kMaxForms] = {
    {"Normal", "Kaioken", "Super Saiyan"},                                    // Goku
    {"Normal", "Unlocked Potential"},                                         // Kid Gohan
    {"Normal", "Super Saiyan", "Super Saiyan 2"},                             // Teen Gohan
    {"Normal", "Super Saiyan", "Super Vegeta"},                               // Vegeta
    {"Normal", "Unlocked Potential"},                                         // Krillin
    {"Normal", "Super Saiyan", "Super Trunks"},                               // Trunks
    {"Normal", "Fused with Nail", "Fused with Kami"},                         // Piccolo
    {}, {}, {}, {}, {}, {},                                                   // Tien - Recoome
    {"First Form", "Second Form", "Third Form", "Final Form", "Full Power"},  // Frieza
    {}, {}, {},                                                               // Androids
    {"First Form", "Second Form", "Perfect Form", "Super Perfect"},           // Cell
    {}, {}, {},                                                               // Saibamen - Bardock
};

int FormCount(uint32_t character) {
  int count = 0;
  while (character < kCharacterCount && count < kMaxForms && kForms[character][count]) {
    ++count;
  }
  return count;
}

// The form picked per side and character, kept while the game runs (like the
// colors the game keeps per slot). Offline only for the other side online:
// see RemoteForms.
std::array<std::array<std::atomic<uint8_t>, kCharacterCount>, 2> g_forms{};

constexpr uint8_t kSideForms = rex::net::online::kGameSideKindFirst + 1;  // 17
constexpr uint8_t kFormsVersion = 1;
// How often the local side's forms go out while the character select runs.
constexpr int64_t kFormsSendMs = 250;
// The character select's logic not running for this long = the next frame
// it runs is a new character select.
constexpr int64_t kNewSelectMs = 1500;

// The game's online session block (sub_82211D70 / sub_82211E40): +28 this
// console's player + 1 (1 host, 2 guest), +288 the other console's.
constexpr uint32_t kNetSession = 0x84245E50;
constexpr uint32_t kNetSessionLocalPlayer = 28;

// Online: the other side's forms received during this character select (the
// table of a side is only ever its own console's choice), and when the local
// side's forms were last sent.
std::array<std::atomic<uint8_t>, kCharacterCount> g_remote_forms{};
std::atomic<int> g_remote_side{-1};  // the side g_remote_forms belong to; -1 none received
std::atomic<int64_t> g_forms_sent_ms{0};

// Start forms can be used: switched on (online: the host's start_forms, synced
// to the guest), and offline or online through the lobby (where they're
// exchanged). Not on the direct LAN / VPN online.
bool FormsAllowed() {
  return REXCVAR_GET(start_forms) &&
         (!rex::net::IsGameSessionOpen() || rex::net::online::IsLobbyMode());
}
// The character select's object and when its logic last ran.
std::atomic<uint32_t> g_select_object{0};
std::atomic<int64_t> g_select_seen_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

uint32_t LoadU32(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address));
}

bool OnlineForms() {
  return rex::net::IsGameSessionOpen() && rex::net::online::IsLobbyMode();
}

// Online: this console's side (0 host, 1 guest) as the game keeps it; -1
// offline or unknown.
int LocalSide(rex::memory::Memory* memory) {
  if (!memory || !OnlineForms()) {
    return -1;
  }
  const uint32_t player = LoadU32(memory, kNetSession + kNetSessionLocalPlayer);
  return (player == 1 || player == 2) ? int(player) - 1 : -1;
}

// The start form a side uses for a character: offline the side's own choice;
// online the local side's own choice, and the other side's only as received
// from its console during this character select (0 until then).
int FormFor(rex::memory::Memory* memory, uint32_t side, uint32_t character) {
  if (side > 1 || character >= kCharacterCount) {
    return 0;
  }
  if (!rex::net::IsGameSessionOpen()) {
    return g_forms[side][character].load(std::memory_order_relaxed);
  }
  const int local = LocalSide(memory);
  if (local < 0) {
    return 0;
  }
  if (int(side) == local) {
    return g_forms[side][character].load(std::memory_order_relaxed);
  }
  return g_remote_side.load(std::memory_order_acquire) == int(side)
             ? g_remote_forms[character].load(std::memory_order_relaxed)
             : 0;
}

void ForgetRemoteForms() {
  g_remote_side.store(-1, std::memory_order_release);
  for (auto& form : g_remote_forms) {
    form.store(0, std::memory_order_relaxed);
  }
}

// Online: the local side's forms to the other console.
void SendLocalForms(int local_side) {
  uint8_t packet[2 + kCharacterCount];
  packet[0] = kFormsVersion;
  packet[1] = uint8_t(local_side);
  for (uint32_t c = 0; c < kCharacterCount; ++c) {
    packet[2 + c] = g_forms[local_side][c].load(std::memory_order_relaxed);
  }
  g_forms_sent_ms.store(NowMs(), std::memory_order_relaxed);
  rex::net::online::SendGameSide(kSideForms, packet, sizeof(packet), 1);
}

// Whether guest memory can be read (the tags read the screen's object from
// the UI thread, which may run a frame after the game freed it).
bool Committed(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  auto* heap = memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info = {};
  return heap && heap->QueryRegionInfo(address, &info) &&
         (info.state & rex::memory::kMemoryAllocationCommit) &&
         uint64_t(address) + size <= uint64_t(info.base_address) + info.region_size;
}

// The character under a side's cursor; false on RANDOM or a bad slot.
bool CursorCharacter(rex::memory::Memory* memory, uint32_t object, uint32_t side,
                     uint32_t& character) {
  if (!object || side > 1) {
    return false;
  }
  const uint32_t position = LoadU32(memory, object + (3 + side) * 4);
  if (position >= 32) {
    return false;
  }
  uint32_t value = LoadU32(memory, object + (8 + side * 32 + position) * 4);
  if (value >= 100) {
    value -= 100;
  }
  character = value;
  return value < kCharacterCount;
}

uint32_t SideStep(rex::memory::Memory* memory, uint32_t side) {
  return LoadU32(memory, kSideBlocks + side * kSideBlockStride + kSideStep);
}

}  // namespace

// Mid-asm hook at 0x82249174 in sub_82249140 (the character select's logic):
// r31 = the screen's object.
void BurstLimitCharSelectFrame(PPCRegister& r31) {
  const int64_t now = NowMs();
  const int64_t last = g_select_seen_ms.exchange(now, std::memory_order_relaxed);
  g_select_object.store(r31.u32, std::memory_order_relaxed);
  if (!FormsAllowed() || !rex::net::IsGameSessionOpen()) {
    return;
  }
  auto* memory = GuestMemory();
  const int local = LocalSide(memory);
  if (now - last > kNewSelectMs) {
    // A new character select online: only what the other console sends from
    // now on counts for its side (not an earlier opponent's or match's table).
    ForgetRemoteForms();
    REXLOG_INFO("Character select (online): this console is side {}; start forms are exchanged",
                local);
  }
  // The local side's forms, a few times a second for as long as the screen
  // runs - also when nothing but A was pressed (the other console then loads
  // this side in its normal form, as this console does).
  if (local >= 0 && now - g_forms_sent_ms.load(std::memory_order_relaxed) >= kFormsSendMs) {
    SendLocalForms(local);
  }
}

// Mid-asm hook at 0x8224A860 in sub_82249140, where a side's cursor checks Y
// (Change Color) when up / down weren't pressed: r19 = buttons pressed this
// frame, r30 = side, r31 = the screen's object.
void BurstLimitCharSelectButtons(PPCRegister& r19, PPCRegister& r30, PPCRegister& r31) {
  const uint32_t side = r30.u32;
  const uint32_t pressed = r19.u32;
  if (side > 1 || !FormsAllowed() || !(pressed & (kButtonRB | kButtonLB))) {
    return;
  }
  auto* memory = GuestMemory();
  // Online only this console's side picks its forms here; the other side's
  // come from its console.
  const int local = LocalSide(memory);
  if (rex::net::IsGameSessionOpen() && int(side) != local) {
    return;
  }
  uint32_t character;
  if (!memory || !CursorCharacter(memory, r31.u32, side, character)) {
    return;
  }
  const int count = FormCount(character);
  if (count < 2) {
    return;
  }
  int form = g_forms[side][character].load(std::memory_order_relaxed);
  form = (pressed & kButtonRB) ? (form + 1) % count : (form + count - 1) % count;
  g_forms[side][character].store(uint8_t(form), std::memory_order_relaxed);
  REXLOG_INFO("Character select: side {} character {} start form {} ({})", side, character, form,
              kForms[character][form]);
  if (local >= 0) {
    SendLocalForms(local);
  }
}

// burstlimit_netinput.cpp's side-channel handler, for kind 17: the other PC's
// side and its start forms (network thread).
bool BurstLimitFormsOnSide(uint8_t kind, const uint8_t* body, size_t size) {
  if (kind != kSideForms) {
    return false;
  }
  const int local = LocalSide(GuestMemory());
  if (size < 2 + kCharacterCount || body[0] != kFormsVersion || local < 0 ||
      int(body[1]) != 1 - local) {
    return true;
  }
  bool changed = g_remote_side.load(std::memory_order_acquire) != int(body[1]);
  for (uint32_t c = 0; c < kCharacterCount; ++c) {
    const uint8_t form = std::min<uint8_t>(body[2 + c], uint8_t(std::max(FormCount(c) - 1, 0)));
    changed |= g_remote_forms[c].exchange(form, std::memory_order_relaxed) != form;
  }
  g_remote_side.store(int(body[1]), std::memory_order_release);
  if (changed) {
    std::string forms;
    for (uint32_t c = 0; c < kCharacterCount; ++c) {
      forms += char('0' + g_remote_forms[c].load(std::memory_order_relaxed));
    }
    REXLOG_INFO("Character select: the other player's (side {}) start forms received: {}", body[1],
                forms);
  }
  return true;
}

// Mid-asm hook at 0x8224530C in sub_82244F60 (the character select's setup),
// after it caps the color counts: r30 = the screen's object, whose words at
// +720 / +724 / +728 are the color counts of Goku, Kid Gohan and Teen Gohan.
void BurstLimitStoryCostumes(PPCRegister& r30) {
  auto* memory = GuestMemory();
  if (!memory || !r30.u32 || !REXCVAR_GET(story_costumes)) {
    return;
  }
  uint8_t* object = memory->TranslateVirtual<uint8_t*>(r30.u32);
  rex::memory::store_and_swap<uint32_t>(object + 720, 4);
  rex::memory::store_and_swap<uint32_t>(object + 724, 4);
  rex::memory::store_and_swap<uint32_t>(object + 728, 3);
}

// The battle mode of the last match loaded, for the Discord presence.
std::atomic<int> g_last_battle_mode{-1};

int BurstLimitLastBattleMode() {
  return g_last_battle_mode.load();
}

// Mid-asm hook at 0x82181E24 in sub_82181BB0 (lhz r28,6(r20)): r20 is the
// player's match request entry, r24 the player (0 or 1).
void BurstLimitStartFormLoad(PPCRegister& r20, PPCRegister& r24) {
  const uint32_t player = r24.u32;
  auto* memory = GuestMemory();
  if (player > 1 || !r20.u32 || !memory) {
    return;
  }
  uint8_t* entry = memory->TranslateVirtual<uint8_t*>(r20.u32);
  const uint16_t character = rex::memory::load_and_swap<uint16_t>(entry + kRequestCharacter);
  const uint32_t mode = LoadU32(memory, r20.u32 - player * kRequestStride + kRequestMode);
  g_last_battle_mode.store(int(mode));
  // An extra character (burstlimit_slots.cpp) plays as this character but has
  // its own forms: its RB / LB choice isn't used.
  if (BurstLimitSlotsStartForm(player, r20.u32)) {
    return;
  }
  // Z Chronicles battles set their own forms; the direct LAN / VPN online stays
  // as it is.
  if (mode == kModeZChronicles || character >= kCharacterCount || !FormsAllowed()) {
    return;
  }
  const int wanted = FormFor(memory, player, character);
  if (rex::net::IsGameSessionOpen()) {
    REXLOG_INFO("Start form (online): player {} character {} form {} ({} side)", player + 1,
                character, wanted, int(player) == LocalSide(memory) ? "this console's" : "the other");
  }
  if (wanted <= 0) {
    return;
  }
  uint16_t max_form = rex::memory::load_and_swap<uint16_t>(entry + kRequestMaxForm);
  // The highest form is the player's own (its save's unlocked forms - 0 with a
  // new save; the game exchanges it, so it's the same on both consoles).
  // Online, 0 lets the character's own forms be the limit, like before.
  if (max_form == 0 && rex::net::IsGameSessionOpen()) {
    max_form = uint16_t(std::max(FormCount(character) - 1, 0));
  }
  const uint16_t form = uint16_t(std::min<int>(wanted, max_form));
  rex::memory::store_and_swap<uint16_t>(entry + kRequestStartForm, form);
  // +0x12 is a byte; +0x13 next to it is the CPU AI level (sub_82165260), so write one byte only.
  entry[kRequestStartForm2] = uint8_t(form);
  REXLOG_INFO("Start form: player {} character {} mode {} form {}", player + 1, character, mode,
              form);
}

namespace {

// The battle HUD's faces, one per form (BUFAC_<code>.NUT), for the characters
// with forms.
constexpr const char* kFaceFiles[kCharacterCount] = {
    "GOK", "GHS", "GHM", "VGT", "KLL", "TRX", "PIC", nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, "FRZ", nullptr, nullptr, nullptr, "CEL", nullptr, nullptr, nullptr,
};

constexpr const char* kRegions[] = {"US", "UK", "FR", "IT", "SP", "DU", "JP"};

// The game's look: the plates' orange, its outlined italic font
// (RPro_EB_fuchi64_alpha, also in the Drama Piece window) and its RB button
// (UICMN_WINDOW.NUT, beside A, B, X, Y, LB...).
constexpr ImU32 kPlateOrange = IM_COL32(243, 146, 0, 255);
constexpr int kButtonRBTexture = 5;

// The tags under the name plates of the character select: the form's face,
// RB while the side picks, and the form's name.
class StartFormTags : public rex::ui::ImGuiDialog {
 public:
  StartFormTags(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                std::filesystem::path game_data_root)
      : ImGuiDialog(drawer),
        immediate_drawer_(immediate_drawer),
        game_data_root_(std::move(game_data_root)) {}

  ~StartFormTags() override {
    if (loader_.joinable()) {
      loader_.join();
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (NowMs() - g_select_seen_ms.load(std::memory_order_relaxed) > 150 || !FormsAllowed()) {
      return;
    }
    auto* memory = GuestMemory();
    const uint32_t object = g_select_object.load(std::memory_order_relaxed);
    if (!memory || !object || !Committed(memory, object, 0x400) ||
        LoadU32(memory, object) != kScreenPicking) {
      return;
    }
    LoadArt();
    RefreshButton();
    // ImGui is linked into both the runtime and this exe; draw in the
    // runtime's context (the one being drawn), not this copy's empty one.
    ImGui::SetCurrentContext(io.Ctx);
    // The game's 16:9 picture, letterboxed in the window.
    const float width = std::min(io.DisplaySize.x, io.DisplaySize.y * 16.0f / 9.0f);
    const float height = width * 9.0f / 16.0f;
    const ImVec2 origin((io.DisplaySize.x - width) * 0.5f, (io.DisplaySize.y - height) * 0.5f);
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    // Right edge and top of each side's tag, under the name on its plate.
    static constexpr float kAnchors[2][2] = {{0.505f, 0.556f}, {0.812f, 0.648f}};
    for (uint32_t side = 0; side < 2; ++side) {
      // Only while the side picks (with RB to change it) and once it's done,
      // not while it waits its turn or has the Ultimate / Drama Piece windows.
      const uint32_t step = SideStep(memory, side);
      const bool picking = step == kStepPicking;
      uint32_t character;
      if ((!picking && step < kStepDone) || !CursorCharacter(memory, object, side, character) ||
          FormCount(character) < 2) {
        continue;
      }
      const int form = FormFor(memory, side, character);
      if (!picking && form == 0) {
        continue;
      }
      const auto& faces = faces_[character];
      rex::ui::ImmediateTexture* face = size_t(form) < faces.size() ? faces[form].get() : nullptr;
      const ImVec2 anchor(origin.x + width * kAnchors[side][0],
                          origin.y + height * kAnchors[side][1]);
      DrawTag(draw_list, anchor, height, kForms[character][form], face, picking);
    }
  }

 private:
  struct Glyph {
    ImVec2 uv0, uv1;
    float width = 0.0f, height = 0.0f;  // the cell, in pixels
    float left = 0.0f, right = 0.0f;    // the visible glyph in the cell
  };

  bool ReadFixFile(const char* file, bool regional, std::vector<uint8_t>& data) const {
    if (!regional) {
      return BurstLimitReadGameFile(game_data_root_, std::string("PAC/CMN/FIX/") + file, data);
    }
    for (const char* region : kRegions) {
      if (BurstLimitReadGameFile(game_data_root_,
                                 std::string("PAC/") + region + "/FIX/" + file, data)) {
        return true;
      }
    }
    return false;
  }

  // On the loader thread: the faces, the font and the RB button, decoded from
  // the game's archive.
  void DecodeArt() {
    for (uint32_t character = 0; character < kCharacterCount; ++character) {
      if (!kFaceFiles[character]) {
        continue;
      }
      std::vector<uint8_t> nut;
      const std::string file = std::string("BUFAC/BUFAC_") + kFaceFiles[character] + ".NUT";
      if (!ReadFixFile(file.c_str(), false, nut) ||
          !BurstLimitDecodeNut(nut, decoded_faces_[character])) {
        REXLOG_WARN("Start form tags: can't read {} from the game's files", file);
      }
    }
    std::vector<uint8_t> nfh, nut;
    std::vector<BurstLimitTexture> atlas;
    if (BurstLimitReadGameFile(game_data_root_, "PAC/CMN/CMN/RPRO_EB_FUCHI64_ALPHA.NFH", nfh) &&
        BurstLimitReadGameFile(game_data_root_, "PAC/CMN/CMN/RPRO_EB_FUCHI64_ALPHA.NUT", nut) &&
        BurstLimitParseNfh(nfh, decoded_glyphs_) && BurstLimitDecodeNut(nut, atlas) &&
        !atlas.empty()) {
      decoded_font_ = std::move(atlas[0]);
    }
    std::vector<BurstLimitTexture> window;
    if (ReadFixFile("UICMN_WINDOW.NUT", true, nut) && BurstLimitDecodeNut(nut, window) &&
        window.size() > kButtonRBTexture) {
      decoded_button_ = std::move(window[kButtonRBTexture]);
    }
  }

  std::unique_ptr<rex::ui::ImmediateTexture> MakeTexture(const BurstLimitTexture& image) const {
    if (image.rgba.empty()) {
      return nullptr;
    }
    return immediate_drawer_->CreateTexture(image.width, image.height,
                                            rex::ui::ImmediateTextureFilter::kLinear, false,
                                            image.rgba.data());
  }

  // Starts decoding the first time the screen shows, then makes the textures
  // here on the UI thread.
  void LoadArt() {
    if (!immediate_drawer_) {
      return;
    }
    if (!loader_.joinable() && !art_uploaded_) {
      loader_ = std::thread([this] {
        DecodeArt();
        art_decoded_.store(true, std::memory_order_release);
      });
    }
    if (art_uploaded_ || !art_decoded_.load(std::memory_order_acquire)) {
      return;
    }
    loader_.join();
    art_uploaded_ = true;
    for (uint32_t character = 0; character < kCharacterCount; ++character) {
      for (const BurstLimitTexture& face : decoded_faces_[character]) {
        faces_[character].push_back(MakeTexture(face));
      }
      decoded_faces_[character].clear();
    }
    font_ = MakeTexture(decoded_font_);
    if (font_) {
      for (const BurstLimitGlyph& glyph : decoded_glyphs_) {
        if (glyph.code < glyphs_.size()) {
          Glyph& g = glyphs_[glyph.code];
          g.uv0 = ImVec2(float(glyph.x) / float(font_->width),
                         float(glyph.y) / float(font_->height));
          g.uv1 = ImVec2(float(glyph.x + glyph.width) / float(font_->width),
                         float(glyph.y + glyph.height) / float(font_->height));
          g.width = glyph.width;
          g.height = glyph.height;
          g.left = glyph.left;
          g.right = glyph.right;
        }
      }
    }
    button_ = MakeTexture(decoded_button_);
    button_style_ = -1;
    decoded_font_ = {};
  }

  // RB in the button_icons style (R1, R...); decoded_button_ is the game's.
  void RefreshButton() {
    const int style = BurstLimitButtonStyle();
    if (!art_uploaded_ || style == button_style_ || decoded_button_.rgba.empty()) {
      return;
    }
    button_style_ = style;
    BurstLimitTexture image = decoded_button_;
    BurstLimitRestyleWindowIcon(kButtonRBTexture, style, image);
    if (auto texture = MakeTexture(image)) {
      button_ = std::move(texture);
    }
  }

  // Text in the game's font, `cell` tall (the font's glyph cells); returns its
  // width, drawing it only with a draw list.
  float GameText(ImDrawList* draw_list, ImVec2 position, float cell, const char* text,
                 ImU32 color) const {
    const float scale = cell / 64.0f;
    // The outlines of neighbors overlap a little, like the game's text.
    const float overlap = 3.0f * scale;
    float pen = 0.0f;
    for (const char* c = text; *c; ++c) {
      const uint8_t code = uint8_t(*c);
      if (code == ' ') {
        pen += 14.0f * scale;
        continue;
      }
      if (code >= glyphs_.size() || glyphs_[code].width == 0.0f) {
        continue;
      }
      const Glyph& g = glyphs_[code];
      if (draw_list) {
        const float x = position.x + pen - g.left * scale;
        draw_list->AddImage(reinterpret_cast<ImTextureID>(font_.get()), ImVec2(x, position.y),
                            ImVec2(x + g.width * scale, position.y + g.height * scale), g.uv0,
                            g.uv1, color);
      }
      pen += (g.right - g.left) * scale - overlap;
    }
    return pen + overlap;
  }

  // A small plate: slanted like the game's, orange frame and black outline,
  // the form's face rising out of its left end, the RB button while the side
  // picks, then the form's name.
  void DrawTag(ImDrawList* draw_list, ImVec2 right_top, float screen_height, const char* name,
               rex::ui::ImmediateTexture* face, bool with_button) const {
    const bool game_font = font_ != nullptr;
    const float cell = screen_height * 0.052f;
    const float text_size = screen_height * 0.034f;
    const float name_width = game_font ? GameText(nullptr, ImVec2(), cell, name, 0)
                                       : rex::ui::overlay_text::Measure(text_size, name).x;
    const float tag_height = cell * 0.78f;
    const float pad = tag_height * 0.28f;
    const float skew = tag_height * 0.32f;
    const float face_height = face ? tag_height * 1.95f : 0.0f;
    const float face_width = face ? face_height * float(face->width) / float(face->height) : 0.0f;
    float button_height = tag_height * 0.66f;
    float button_width = 0.0f;
    if (with_button) {
      button_width = button_ ? button_height * float(button_->width) / float(button_->height)
                             : rex::ui::overlay_text::Measure(button_height * 0.8f, "RB").x +
                                   pad * 1.2f;
    }
    const float tag_width = skew + pad + (face ? face_width + pad * 0.5f : 0.0f) +
                            (with_button ? button_width + pad * 0.6f : 0.0f) + name_width + pad +
                            skew * 0.5f;
    const ImVec2 min(right_top.x - tag_width, right_top.y);
    const ImVec2 max(right_top.x, right_top.y + tag_height);
    // Leaning right like the plates: the top edge sits `skew` further right.
    const ImVec2 corners[4] = {ImVec2(min.x + skew, min.y), ImVec2(max.x, min.y),
                               ImVec2(max.x - skew, max.y), ImVec2(min.x, max.y)};
    const float frame = std::max(2.0f, screen_height * 0.0035f);
    draw_list->AddQuad(corners[0], corners[1], corners[2], corners[3], IM_COL32(0, 0, 0, 255),
                       frame * 3.0f);
    draw_list->AddQuadFilled(corners[0], corners[1], corners[2], corners[3],
                             IM_COL32(18, 8, 4, 225));
    draw_list->AddQuad(corners[0], corners[1], corners[2], corners[3], kPlateOrange, frame);

    float x = min.x + skew * 0.5f + pad;
    if (face) {
      const ImVec2 face_max(x + face_width, max.y - frame);
      draw_list->AddImage(reinterpret_cast<ImTextureID>(face),
                          ImVec2(face_max.x - face_width, face_max.y - face_height), face_max);
      x = face_max.x + pad * 0.5f;
    }
    if (with_button) {
      const float top = min.y + (tag_height - button_height) * 0.5f;
      if (button_) {
        draw_list->AddImage(reinterpret_cast<ImTextureID>(button_.get()), ImVec2(x, top),
                            ImVec2(x + button_width, top + button_height));
      } else {
        draw_list->AddRectFilled(ImVec2(x, top), ImVec2(x + button_width, top + button_height),
                                 IM_COL32(70, 70, 70, 255), button_height * 0.45f);
        rex::ui::overlay_text::Draw(draw_list, button_height * 0.8f,
                                    ImVec2(x + pad * 0.6f, top + button_height * 0.1f),
                                    IM_COL32(255, 255, 255, 255), "RB");
      }
      x += button_width + pad * 0.6f;
    }
    if (game_font) {
      GameText(draw_list, ImVec2(x, min.y + (tag_height - cell) * 0.5f), cell, name,
               IM_COL32(255, 255, 255, 255));
    } else {
      rex::ui::overlay_text::Draw(draw_list, text_size,
                                  ImVec2(x, min.y + (tag_height - text_size * 1.2f) * 0.5f),
                                  IM_COL32(255, 255, 255, 255), name);
    }
  }

  rex::ui::ImmediateDrawer* immediate_drawer_;
  std::filesystem::path game_data_root_;
  std::thread loader_;
  std::atomic<bool> art_decoded_{false};
  bool art_uploaded_ = false;
  // Filled by the loader thread, read here once it's done.
  std::array<std::vector<BurstLimitTexture>, kCharacterCount> decoded_faces_;
  std::vector<BurstLimitGlyph> decoded_glyphs_;
  BurstLimitTexture decoded_font_;
  BurstLimitTexture decoded_button_;
  // UI thread.
  std::array<std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>, kCharacterCount> faces_;
  std::unique_ptr<rex::ui::ImmediateTexture> font_;
  std::unique_ptr<rex::ui::ImmediateTexture> button_;
  int button_style_ = -1;  // the button_icons style button_ shows (-1: the game's)
  std::array<Glyph, 128> glyphs_{};
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateStartFormTags(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
    const std::filesystem::path& game_data_root) {
  return std::make_unique<StartFormTags>(drawer, immediate_drawer, game_data_root);
}

// Whether the character select is on screen (its logic ran in the last 300 ms).
bool BurstLimitCharacterSelectActive() {
  return NowMs() - g_select_seen_ms.load(std::memory_order_relaxed) < 300;
}

// For burstlimit_quickfight.cpp: a character's forms, and the start form of a
// side as if RB / LB had picked it on the character select.
int BurstLimitFormCount(uint32_t character) {
  return FormCount(character);
}

const char* BurstLimitFormName(uint32_t character, int form) {
  return (form >= 0 && form < FormCount(character)) ? kForms[character][form] : nullptr;
}

void BurstLimitSetStartForm(uint32_t side, uint32_t character, int form) {
  if (side > 1 || character >= kCharacterCount) {
    return;
  }
  const int count = FormCount(character);
  form = std::clamp(form, 0, std::max(count - 1, 0));
  g_forms[side][character].store(uint8_t(form), std::memory_order_relaxed);
}
