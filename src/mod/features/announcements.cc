// [ui] announcements: "!NGO <text>" in alliance chat from the admiral or a commodore becomes a banner at the top of
// the HUD, under the chat preview, until "!NGO-ENDE" or [ui] announcement_max_days. It is kept on disk across
// restarts. Messages are read as the game processes them and as chat shows them, so ones posted while the player was
// away count once the history arrives. [shortcuts] toggle_fleet_bar hides the banner with the fleet list until a new
// announcement comes. The mod never sends a message.

#include "config.h"
#include "features/announcements.h"
#include "features/features.h"
#include "file.h"

#include "game/alliance.h"
#include "game/app.h"
#include "game/chat.h"
#include "game/hud.h"

#include <il2cpp/hook.h>
#include <il2cpp/il2cpp.h>
#include <spdlog/spdlog.h>
#include <unity/core.h>
#include <unity/ui.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
auto& announcement_state = *new announcements::State;
bool  announcement_loaded = false;
// When this session took up the current announcement, in Unix seconds; 0 when it came from disk.
int64_t announcement_received = 0;
// Leadership checks by sender and time, since chat binds the same message again and again.
auto& announcement_checked = *new std::unordered_map<std::string, bool>;
// Senders whose rank has been logged.
auto& announcement_logged = *new std::unordered_set<std::string>;

std::filesystem::path AnnouncementFile()
{
  auto name = File::Vars();
  if (const auto dot = name.rfind('.'); dot != std::string::npos)
    name.resize(dot);
  return File::Path(name + ".announcement");
}

void LoadAnnouncement()
{
  if (std::exchange(announcement_loaded, true))
    return;
  std::ifstream in(AnnouncementFile(), std::ios::binary);
  if (!in)
    return;
  const std::string file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  announcement_state = announcements::Load(file);
  if (announcement_state.current)
    spdlog::info("[Announcements] kept from {}: {}", announcement_state.current->sender,
                 announcement_state.current->text);
}

void SaveAnnouncement()
{
  std::ofstream out(AnnouncementFile(), std::ios::binary | std::ios::trunc);
  out << announcements::Save(announcement_state);
  if (!out)
    spdlog::warn("[Announcements] could not save {}", AnnouncementFile().string());
}

// The admiral, or a member whose rank is next to the admiral's.
bool AnnouncementLeader(ChatMessage* message, const std::string& sender_id)
{
  auto* manager = AllianceManager::Existing();
  auto* profile = manager ? manager->GetLocalAllianceProfile() : nullptr;
  auto* leader  = profile ? profile->LeaderUserId() : nullptr;
  if (!leader || sender_id.empty()) {
    spdlog::info("[Announcements] the alliance's admiral is not known yet; {} is not checked", message->SenderName());
    return false;
  }
  if (il2cpp::ToUtf8(leader) == sender_id)
    return true;
  auto* member  = profile->Member(message->SenderId());
  auto* admiral = profile->Member(leader);
  if (!member || !admiral) {
    spdlog::info("[Announcements] the ranks of {} and the admiral are not known", message->SenderName());
    return false;
  }
  const bool accepted = announcements::LeaderRank(member->Rank(), admiral->Rank());
  if (announcement_logged.insert(sender_id).second)
    spdlog::info("[Announcements] {} has rank {}, the admiral {}: {}", message->SenderName(), member->Rank(),
                 admiral->Rank(), accepted ? "may announce" : "may not announce");
  return accepted;
}

void ChatManager_PostProcessMessage_Hook(auto original, ChatManager* _this, ChatMessage* message)
{
  original(_this, message);
  ConsiderAnnouncement(message);
}

// --- Banner ---

auto&       announcement_label = *new il2cpp::GcHandle;
auto&       announcement_panel = *new il2cpp::GcHandle;
std::string announcement_shown_text;
float       announcement_placed_scale = 0;

constexpr float kBannerWidth    = 620;
constexpr float kBannerFontSize = 15;
// Two lines and the padding; a longer announcement ends in an ellipsis.
constexpr float kBannerHeight = 50;
// From the top of the HUD, below the two lines of the chat preview.
constexpr float kBannerTop = 58;

unity::TMP_Text* CreateAnnouncementBanner(unity::Transform* drawer, unity::Transform* parent)
{
  auto* object = drawer->gameObject();
  auto* source = object ? object->GetComponentInChildren<unity::TextMeshProUGUI>() : nullptr;
  auto* font   = source ? source->font() : nullptr;
  if (!font)
    return nullptr;

  il2cpp::GcHandle panel, text;
  auto*            panel_rect = unity::CreateUiObject("CommunityMod_Announcement", parent, panel);
  if (!panel_rect)
    return nullptr;
  if (auto* image = panel.Get<unity::GameObject>()->AddComponent<unity::Image>()) {
    image->SetColor({0.04f, 0.09f, 0.16f, 0.82f});
    image->SetRaycastTarget(false);
  }
  auto* text_rect = unity::CreateUiObject("AnnouncementText", panel_rect, text);
  auto* label     = text_rect ? text.Get<unity::GameObject>()->AddComponent<unity::TextMeshProUGUI>() : nullptr;
  if (!label)
    return nullptr;
  text_rect->Place({0, 0}, {1, 1}, {0.5f, 0.5f}, {-16, -8}, {0, 0});
  label->SetFont(font);
  label->SetAlignment(unity::TMP_Text::Alignment::Center);
  label->SetColor({0.89f, 0.93f, 0.96f, 1});
  label->SetRaycastTarget(false);
  label->SetEnableWordWrapping(true);
  label->SetOverflowMode(unity::TMP_Text::Overflow::Ellipsis);
  label->SetRichText(true);
  text.Get<unity::GameObject>()->SetActive(true);
  announcement_label = il2cpp::GcHandle::Strong(label);
  announcement_panel = il2cpp::GcHandle::Strong(panel_rect);
  announcement_shown_text.clear();
  announcement_placed_scale = 0;
  return label;
}

void AnnouncementsFrame()
{
  static Throttle throttle(std::chrono::milliseconds(500));
  if (!throttle.Due())
    return;
  const auto& ui = Config::Get().ui;
  LoadAnnouncement();

  const int64_t now    = std::time(nullptr);
  const auto    shown  = ui.announcements ? announcements::Shown(announcement_state, now, ui.announcement_max_days)
                                          : std::nullopt;
  const auto    hidden = ModOverlaysHiddenAt();
  auto*         drawer_view = shown && (hidden == 0 || announcement_received > hidden)
                                  ? ScreenManager::View<HudAllianceAndNewsViewController>()
                                  : nullptr;
  auto*         promotion = drawer_view ? drawer_view->chestPromotion() : nullptr;
  auto*         drawer    = promotion ? promotion->transform() : nullptr;
  auto*         parent    = drawer ? unity::CanvasRoot(drawer) : nullptr;

  auto* label = announcement_label.Get<unity::TMP_Text>();
  auto* panel = announcement_panel.Get<unity::RectTransform>();
  if (!unity::Alive(label) || !unity::Alive(panel))
    label = nullptr;
  if (label && (!parent || panel->parent() != parent)) {
    if (auto* object = label->ParentObject())
      object->SetActive(false);
    if (parent)
      label = nullptr;
  }
  if (!parent)
    return;
  if (!label && !(label = CreateAnnouncementBanner(drawer, parent)))
    return;
  panel = announcement_panel.Get<unity::RectTransform>();

  if (auto text = announcements::BannerText(*shown); text != announcement_shown_text) {
    announcement_shown_text = std::move(text);
    label->SetText(announcement_shown_text);
  }
  if (const float scale = ui.fleet_bar_scale; scale != announcement_placed_scale) {
    announcement_placed_scale = scale;
    panel->Place({0.5f, 1}, {0.5f, 1}, {0.5f, 1}, {kBannerWidth * scale, kBannerHeight * scale}, {0, -kBannerTop});
    label->SetFontSize(kBannerFontSize * scale);
  }
  if (auto* object = label->ParentObject())
    object->SetActive(true);
}

bool InstallAnnouncements()
{
  // Without this detour, announcements are still read as chat shows them.
  il2cpp::HookSet hooks("Announcements");
  IL2CPP_STAGE(hooks, ChatManager::Class::Method<void(ChatMessage*)>("PostProcessMessage"),
               ChatManager_PostProcessMessage_Hook);
  if (!hooks.Commit())
    spdlog::info("[Announcements] reading announcements only as chat shows them");
  return true;
}
} // namespace

bool ConsiderAnnouncement(ChatMessage* message)
{
  const auto& ui = Config::Get().ui;
  if (!ui.announcements || !message || !message->IsAlliance())
    return false;
  const auto parsed = announcements::Parse(message->RawText());
  if (parsed.command == announcements::Command::kNone)
    return false;
  LoadAnnouncement();

  const int64_t now       = std::time(nullptr);
  const int64_t stamp     = announcements::UnixSeconds(message->TimeStampDouble());
  const int64_t posted    = stamp > 0 ? stamp : now;
  auto*         sender    = message->SenderId();
  const auto    sender_id = sender ? il2cpp::ToUtf8(sender) : std::string();
  const auto    key       = sender_id + "|" + std::to_string(posted);
  auto          checked   = announcement_checked.find(key);
  if (checked == announcement_checked.end()) {
    if (announcement_checked.size() > 256)
      announcement_checked.clear();
    checked = announcement_checked.emplace(key, AnnouncementLeader(message, sender_id)).first;
  }
  if (!checked->second)
    return false;

  const auto name = message->SenderName();
  if (announcements::Apply(announcement_state, parsed, name, posted)) {
    announcement_received = now;
    SaveAnnouncement();
    if (parsed.command == announcements::Command::kEnd)
      spdlog::info("[Announcements] ended by {}", name);
    else
      spdlog::info("[Announcements] from {}: {}", name, parsed.text);
  }
  return true;
}

const Feature kAnnouncements{"Announcements", &Config::Patches::chatpatches, InstallAnnouncements,
                             AnnouncementsFrame};
