// Settings parsing and shortcut matching, run against production code with Unity input replaced by a fake.
#include "config.h"
#include "features/chat_startup.h"
#include "features/defense_schedule.h"
#include "features/mouse_navigation.h"
#include "features/notification_audio.h"
#include "features/ship_shortcut_hint.h"
#include "features/toast_text.h"
#include "features/fleet_bar.h"
#include "features/window_placement.h"
#include "reload.h"
#include "shortcuts.h"
#include "text.h"

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>
#include <toml++/toml.h>
#include <unity/input.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <source_location>
#include <utility>

namespace
{
std::array<bool, std::to_underlying(unity::KeyCode::Max)> held{};
int                                                       failures = 0;

void Check(bool condition, const char* what, std::source_location where = std::source_location::current())
{
  if (!condition) {
    std::cerr << where.file_name() << ":" << where.line() << ": " << what << '\n';
    ++failures;
  }
}

void Hold(std::initializer_list<unity::KeyCode> keys)
{
  held.fill(false);
  for (auto key : keys)
    held[std::to_underlying(key)] = true;
}

toml::table Load(std::string_view settings, bool extended = true, bool experimental = false)
{
  toml::table parsed;
  Shortcut::Set(Shortcut::Parse(toml::parse(settings), parsed, extended, experimental));
  return parsed;
}

std::string Value(const toml::table& parsed, std::string_view section, std::string_view key)
{ return parsed[section][key].value_or(std::string("<missing>")); }

void ShipHints()
{
  // A locked dock F leaves native G at the sixth selectable position (the reported 1,2,3,4,5,7 layout).
  const std::array gapped_fleets{0, 1, 2, 3, 4, 6};
  auto             ship_hint = [&](Action action) {
    return ShipShortcutHintAction(action, gapped_fleets.size(), [&](std::size_t i) { return gapped_fleets[i]; });
  };
  Check(ship_hint(Action::kSelectShip7) == Action::kSelectShip6,
        "Native ship G must advertise the sixth mod shortcut when dock F is locked");
  Check(ship_hint(Action::kSelectShip5) == Action::kSelectShip5, "Earlier ship hints must stay unchanged");
  Check(ship_hint(Action::kSelectShip6) == Action::kCount, "Missing dock must not advertise a shortcut");
  const std::array reordered_fleets{7, 0, 2, -1, 6, 4, 1, 3, 5};
  auto             reordered_hint = [&](Action action) {
    return ShipShortcutHintAction(action, reordered_fleets.size(), [&](std::size_t i) { return reordered_fleets[i]; });
  };
  Check(reordered_hint(Action::kSelectShip8) == Action::kSelectShip1, "Ship hints must follow bar order");
  Check(reordered_hint(Action::kSelectShip7) == Action::kSelectShip5,
        "Null fleet entries must preserve the positions passed to RequestSelect");
  Check(reordered_hint(Action::kSelectShip6) == Action::kCount,
        "Fleet beyond the eight supported mod shortcuts must not advertise a shortcut");
  Shortcut::Bindings custom{};
  custom[std::to_underlying(Action::kSelectShip6)].push_back(KeyChord::Parse("CTRL-F8"));
  Shortcut::Set(std::move(custom));
  Check(Shortcut::Hint(ship_hint(Action::kSelectShip7)) == "^F8",
        "Gapped ship hint must use the actual custom binding, not a hardcoded number");
}

void StringHelpers()
{
  Check(StrSplit(" a, ,b ,", ',') == std::vector<std::string>{"a", "b"}, "StrSplit trims and drops empty tokens");
  Check(StrSplit("", ',').empty(), "StrSplit of nothing");
  // A view into a longer string must not pick up the text after it.
  constexpr std::string_view padded = " warp  ";
  Check(AsciiStrToUpper(StripAsciiWhitespace(padded)) == "WARP", "upper-casing a trimmed view");
  Check(AsciiStrToLower("FeDeRaTiOn") == "federation", "lower-casing");
}

void Timestamps()
{
  Check(ParseUtcTimestamp("1970-01-01T00:00:00") == 0, "epoch");
  Check(ParseUtcTimestamp("2024-05-01T12:34:56Z") == 1714566896, "UTC with zone suffix");
  Check(ParseUtcTimestamp("2024-05-01T12:34:56.789Z") == 1714566896, "fraction ignored");
  Check(ParseUtcTimestamp("2024-02-30T00:00:00") == std::nullopt, "invalid date");
  Check(ParseUtcTimestamp("2024-05-01 12:34:56") == std::nullopt, "wrong separator");
  Check(ParseUtcTimestamp("") == std::nullopt, "empty");
}

void ToastText()
{
  using namespace toast_text;
  const std::vector<std::string> values{"Kirk", "7", ""};
  Check(FormatPlaceholders("{0} scored {1}", values) == "Kirk scored 7", "plain placeholders");
  Check(FormatPlaceholders("Lv.{1:000}", values) == "Lv.007", "zero-padded placeholder");
  Check(FormatPlaceholders("{0}{0}", values) == "KirkKirk", "repeated placeholder");
  Check(FormatPlaceholders("{10} and {1}", values) == "{10} and 7", "{1} does not match {10}");
  Check(FormatPlaceholders("{2} stays", values) == "{2} stays", "empty values are left alone");
  Check(FormatPlaceholders("{1:x2}", values) == "7", "other formats are filled unpadded");
  const std::vector<std::string> literal_values{"{1}", "Enterprise"};
  Check(FormatPlaceholders("{0} vs {1}", literal_values) == "{1} vs Enterprise",
        "replacement text stays literal instead of being interpreted as another placeholder");
  Check(FormatPlaceholders("{00} {+1} {99999999999999999999999999} {0", values)
            == "{00} {+1} {99999999999999999999999999} {0",
        "malformed, overflowing and unclosed placeholders remain unchanged");
  Check(StripRichText("<color=#FF0000>Red</color> <b>bold</b>") == "Red bold", "rich text tags removed");
  Check(StripRichText("a < b") == "a < b", "an unclosed '<' is text");
  Check(TitleCase("USS ENTERPRISE-D") == "Uss Enterprise-D", "title case");
  Check(HullKeyName("Hull_L30_Destroyer_Klingon_LIVE") == "Lv.30 Destroyer Klingon", "hull key with level");
  Check(HullKeyName("Hull_BORG_CUBE") == "BORG CUBE", "hull key without level");
}

void ShipNames()
{
  Check(NormalizeShipName("  U.S.S. Enterprise-D ") == "ENTERPRISE D", "ship names drop USS and punctuation");
  Check(NormalizeShipName("d'deridex__b") == "DDERIDEX B", "ship names collapse separators");
  const auto academy = ShipNameWords("Academy Athena");
  Check(ShipNameMatches(academy, ShipNameWords("athena")), "a ship name matches its last word");
  Check(ShipNameMatches(academy, ShipNameWords("ACADEMY ATHENA")), "a ship name matches all its words");
  Check(!ShipNameMatches(academy, ShipNameWords("academy")), "a ship name does not match its first word");
  Check(!ShipNameMatches(academy, ShipNameWords("")), "an empty pattern matches nothing");
  Check(!ShipNameMatches(ShipNameWords("Thena"), ShipNameWords("Athena")), "words match whole");
}

void Chords()
{
  const auto chord = KeyChord::Parse("ctrl-shift-f1");
  Check(chord.key == unity::KeyCode::F1, "chord key");
  Check(chord.modifiers.size() == 2, "chord modifiers");
  Check(chord.Text() == "CTRL-SHIFT-F1", "chord text");
  Check(chord.Hint() == "^+F1", "chord badge");
  Check(KeyChord::Parse("SHIFT-+").Hint() == "+PLS", "plus key as the primary key");
  Check(KeyChord::Parse("CTRL").key == unity::KeyCode::None, "modifier alone has no key");
  Check(KeyChord::Parse("NONSENSE").key == unity::KeyCode::None, "unknown key");
  Check(KeyChord::Parse("CTRL-A-B").key == unity::KeyCode::B && KeyChord::Parse("CTRL-A-B").Text() == "CTRL-B",
        "with two keys the last counts, and the text shows it");
  Check(KeyChord::Parse("pgup-lshift").Hint() == "<+PU", "modifiers come first in the badge");
}

void KeyRanges()
{
  for (const auto [name, key] : {std::pair{"A", unity::KeyCode::A},
                                 {"Z", unity::KeyCode::Z},
                                 {"0", unity::KeyCode::Alpha0},
                                 {"9", unity::KeyCode::Alpha9},
                                 {"F1", unity::KeyCode::F1},
                                 {"F9", unity::KeyCode::F9},
                                 {"F10", unity::KeyCode::F10},
                                 {"F15", unity::KeyCode::F15},
                                 {"KEY0", unity::KeyCode::Keypad0},
                                 {"KEY9", unity::KeyCode::Keypad9}}) {
    const auto chord = KeyChord::Parse(name);
    Check(chord.key == key && chord.Text() == name && chord.Hint() == name, "key range and display compatibility");
  }
  for (const auto name : {"F0", "F16", "F01", "F1X", "KEY00", "KEY10", "KEY", "KEY1X", "F999999999999"})
    Check(KeyChord::Parse(name).key == unity::KeyCode::None, "out-of-range and noncanonical names stay invalid");
}

void DefaultBindings()
{
  const auto fixture = toml::parse_file(STFC_SHORTCUT_DEFAULTS);
  for (const auto [extended, experimental] : {std::pair{false, false}, {false, true}, {true, false}, {true, true}}) {
    const auto parsed = Load("", extended, experimental);
    for (const auto& [tier, entries] : fixture) {
      const bool enabled = tier == "always" || (extended && (tier == "extended" || experimental));
      for (const auto& [key, expected] : *entries.as_table()) {
        if (enabled) {
          Check(Value(parsed, "shortcuts", key.str()) == expected.value_or(std::string{}),
                "default chord compatibility");
          Check(Value(parsed, "shortcuts_source", key.str()) == "default", "default binding source compatibility");
        } else
          Check(!parsed["shortcuts"][key.str()], "optional shortcut gates stay unchanged");
      }
    }
    // Every shortcut's default is pinned here, so the parser never meets a default without a valid chord.
    for (const auto& [key, value] : *parsed["shortcuts"].as_table())
      Check(fixture["always"][key] || fixture["extended"][key] || fixture["experimental"][key],
            "every shortcut has a default in shortcut_defaults.toml");
  }
}

void Loading()
{
  auto parsed = Load(R"(
    [shortcuts]
    show_galaxy = "CTRL-G | F8"
    show_system = ""
    show_events = "BOGUS"
    quit = 42
    set_hotkeys_disble = "F3"
  )");
  Check(Shortcut::Describe(Action::kShowGalaxy) == "CTRL-G | F8", "both alternatives bound");
  Check(Shortcut::Hint(Action::kShowGalaxy) == "^G", "badge shows the first alternative");
  Check(Value(parsed, "shortcuts", "show_galaxy") == "CTRL-G | F8", "vars records the bindings");
  Check(Value(parsed, "shortcuts_source", "show_galaxy") == "config", "source is the settings file");

  Check(Shortcut::Describe(Action::kShowSystem).empty(), "an empty string disables the binding");
  Check(Value(parsed, "shortcuts", "show_system") == "NONE", "disabled binding recorded as NONE");

  Check(Shortcut::Describe(Action::kShowEvents) == std::string("SHIFT-E"),
        "a binding with no valid key falls back to the default");
  Check(Value(parsed, "shortcuts_source", "show_events") == "default", "fallback recorded as default");
  Check(Shortcut::Describe(Action::kQuit) == std::string("F10"), "a non-string falls back too");

  Check(Shortcut::Describe(Action::kDisableHotKeys) == "F3", "deprecated alias still binds");
  Check(Value(parsed, "shortcuts_source", "set_hotkeys_disabled") == "alias:set_hotkeys_disble", "alias source");

  Check(!Shortcut::Describe(Action::kShowBookmarks).empty(), "extended shortcuts load when extended");
  parsed = Load("");
  Check(Shortcut::Describe(Action::kQuit) == "F10", "loading again replaces earlier bindings");
  Check(Shortcut::Describe(Action::kShowLookup).empty(), "experimental shortcuts need experimental");
  parsed = Load("", false);
  Check(Shortcut::Describe(Action::kShowBookmarks).empty(), "extended shortcuts are skipped otherwise");
  Check(Value(parsed, "shortcuts", "show_bookmarks") == "<missing>", "skipped shortcuts are not recorded");
}

// The English example documents every setting. Each key it sets must be one the mod reads, and none may be
// rejected: otherwise the example and the reader have drifted apart.
void ExampleSettings()
{
  // TODO(alex): auto_confirm_discovery is documented but has never been read by any release; drop it from the
  // examples, or implement it.
  static const std::set<std::string> kDocumentedButUnread{"ui.auto_confirm_discovery"};

  std::vector<std::string> rejected;
  auto                     logger = std::make_shared<spdlog::logger>(
      "settings", std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg& message) {
        if (message.level >= spdlog::level::warn)
          rejected.emplace_back(message.payload.data(), message.payload.size());
      }));
  const auto previous = spdlog::default_logger();
  spdlog::set_default_logger(logger);

  auto example = toml::parse_file(STFC_EXAMPLE_SETTINGS);
  // Experimental settings and shortcuts are only read with this on.
  example["control"].as_table()->insert_or_assign("enable_experimental", true);
  toml::table effective;
  const auto  config = Config::Parse(example, effective, false);
  spdlog::set_default_logger(previous);

  for (const auto& message : rejected)
    std::cerr << "rejected: " << message << '\n';
  Check(rejected.empty(), "the example parses without warnings");

  // Every value the example sets appears among the values the mod used.
  const std::function<void(const toml::table&, const toml::table*, const std::string&)> compare =
      [&](const toml::table& documented, const toml::table* used, const std::string& path) {
        for (const auto& [key, node] : documented) {
          const auto  name = path.empty() ? std::string(key.str()) : path + "." + std::string(key.str());
          const auto* next = used ? used->get(key) : nullptr;
          if (const auto* table = node.as_table()) {
            compare(*table, next ? next->as_table() : nullptr, name);
          } else if (!next && !kDocumentedButUnread.contains(name)) {
            std::cerr << "unread setting: " << name << '\n';
            Check(false, "every example setting is read");
          }
        }
      };
  compare(example, &effective, "");

  Check(config->graphics.ui_scale > 0.0f, "example values are applied");
  Check(config->ui.hud_q_trials == MissionHudVisibility::kAuto
            && config->ui.hud_field_training == MissionHudVisibility::kAuto
            && config->ui.hud_outposts == MissionHudVisibility::kAuto
            && config->ui.hud_missions == MissionHudVisibility::kAuto,
        "example HUD settings use native visibility");
  Check(effective["sync"]["fleets"].value<bool>() == false, "the example documents fleet sync, off by default");
}

void NotificationAudio()
{
  struct Fixture {
    NotificationSound sound;
    std::string_view  name;
    size_t            bytes;
  };
  constexpr Fixture fixtures[] = {
      {NotificationSound::kNone, "none", 0},           {NotificationSound::kDefault, "default", 17240},
      {NotificationSound::kInfo, "info", 18918},       {NotificationSound::kSuccess, "success", 26148},
      {NotificationSound::kWarning, "warning", 31264}, {NotificationSound::kAlarm, "alarm", 37440},
      {NotificationSound::kArrival, "arrival", 26146}, {NotificationSound::kSoft, "soft", 19976},
      {NotificationSound::kPing, "ping", 8422},        {NotificationSound::kRepair, "repair", 27912},
  };
  for (const auto& fixture : fixtures) {
    toml::table effective;
    const auto  config =
        Config::Parse(toml::parse(std::string("[audio]\nalert_victory = \"") + AsciiStrToUpper(fixture.name) + "\""),
                      effective, false);
    Check(NotificationSoundName(fixture.sound) == fixture.name && config->audio.alert_victory == fixture.sound,
          "cue names round-trip");
    const auto data = NotificationSoundBytes(fixture.sound);
    Check(data.size() == fixture.bytes, "cue length matches the independent pre-refactor fixture");
    Check(NotificationSoundBytes(fixture.sound).data() == data.data(), "cue data stays cached");
    if (data.empty())
      continue;
    const auto word = [&](size_t at) {
      return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16
             | uint32_t(data[at + 3]) << 24;
    };
    Check(std::string_view(reinterpret_cast<const char*>(data.data()), 4) == "RIFF"
              && std::string_view(reinterpret_cast<const char*>(data.data() + 8), 8) == "WAVEfmt ",
          "WAV signature");
    Check(word(4) == data.size() - 8 && word(40) == data.size() - 44, "WAV chunk lengths");
    Check(word(24) == 44100 && word(28) == 88200 && data[20] == 1 && data[22] == 1 && data[34] == 16,
          "cue data is 44.1kHz mono 16-bit PCM");
  }
  // Settings parsing trims and lower-cases, so " none " is a valid name there.
  for (const auto& [name, expected] :
       std::initializer_list<std::pair<std::string_view, NotificationSound>>{{"off", NotificationSound::kNone},
                                                                             {"silent", NotificationSound::kNone},
                                                                             {"victory", NotificationSound::kSuccess},
                                                                             {"warn", NotificationSound::kWarning},
                                                                             {"attack", NotificationSound::kAlarm},
                                                                             {"arrive", NotificationSound::kArrival},
                                                                             {"quiet", NotificationSound::kSoft},
                                                                             {"repaired", NotificationSound::kRepair},
                                                                             {"unknown", NotificationSound::kNone}}) {
    toml::table effective;
    const auto config = Config::Parse(toml::parse(std::string("[audio]\nalert_defeat = \"") + std::string(name) + "\""),
                                      effective, false);
    Check(config->audio.alert_defeat == expected, "legacy cue aliases remain valid, unknown names fall back to none");
  }
}

void DailyFactionSettings()
{
  const std::vector<std::string> all{"federation", "klingon", "romulan"};
  toml::table                    effective;
  for (const auto text : {"", "[ui]\ndaily_bulk_claim_factions = \"\"", "[ui]\ndaily_bulk_claim_factions = \"   \""}) {
    const auto config = Config::Parse(toml::parse(text), effective, false);
    Check(config->ui.daily_bulk_claim_factions == all, "missing and blank faction filters select all main factions");
    Check(effective["ui"]["daily_bulk_claim_factions"].value<std::string>() == "federation,klingon,romulan",
          "effective settings materialize the explicit daily faction default");
    Check(!config->ui.auto_confirm_alliance_contribution, "alliance contribution confirmation is opt-in");
  }
  for (const auto filter : {"none", "NONE,federation"}) {
    const auto config = Config::Parse(toml::parse(std::string("[ui]\ndaily_bulk_claim_factions = \"") + filter + "\""),
                                      effective, false);
    Check(config->ui.daily_bulk_claim_factions == std::vector<std::string>{"none"}, "none overrides named factions");
  }
  const auto config = Config::Parse(
      toml::parse(
          "[ui]\ndaily_bulk_claim_factions = \"Federation,romulan\"\nauto_confirm_alliance_contribution = true"),
      effective, false);
  Check(config->ui.daily_bulk_claim_factions == std::vector<std::string>{"federation", "romulan"},
        "named faction filters remain case-insensitive");
  Check(config->ui.auto_confirm_alliance_contribution, "alliance contribution setting survives parsing");
}

void DefenseSchedule()
{
  using namespace defense_schedule;
  LoaderDefense parsed;
  Check(ParseDefenseTime("27.11. 13:00", parsed) && parsed.year == 0 && parsed.month == 11 && parsed.day == 27
            && parsed.hour == 13 && parsed.minute == 0,
        "a defense time without a year");
  Check(ParseDefenseTime(" 1.2.2027  9:05 ", parsed) && parsed.year == 2027 && parsed.month == 2 && parsed.day == 1
            && parsed.hour == 9 && parsed.minute == 5,
        "single digits, a year and extra spaces");
  Check(ParseDefenseTime("2026-11-28 14:00", parsed) && parsed.year == 2026 && parsed.month == 11 && parsed.day == 28,
        "an ISO defense date");
  for (const auto* bad : {"", "27.11.", "Fr 13:00", "27.11. 24:00", "32.11. 13:00", "27.13. 13:00", "27.11. 13",
                          "27.11. 13:00x", "-1.11. 13:00", "27.11.26 13:00"}) {
    LoaderDefense untouched;
    Check(!ParseDefenseTime(bad, untouched) && untouched.month == 0, "a malformed defense time is rejected");
  }

  Check(Countdown(30) == "in 1 Min", "a countdown never shows less than a minute");
  Check(Countdown(20 * 60) == "in 20 Min", "minutes");
  Check(Countdown(2 * 3600 + 20 * 60) == "in 2 Std 20 Min", "hours and minutes");
  Check(Countdown(26 * 3600) == "in 1 T 2 Std", "days and hours");

  const auto at = [](const char* text) {
    LoaderDefense defense;
    ParseDefenseTime(text, defense);
    return DefenseStart(defense, 0);
  };
  std::vector<LoaderDefense> defenses(3);
  ParseDefenseTime("27.11.2026 18:00", defenses[0]);
  ParseDefenseTime("27.11.2026 13:00", defenses[1]);
  ParseDefenseTime("20.11.2026 13:00", defenses[2]);
  defenses[0].system = "Tazolka";
  defenses[0].level  = 1;
  defenses[1].system = "Aylus";
  defenses[1].level  = 1;
  defenses[2].system = "Vorbei";
  Check(DefenseTip(defenses, at("27.11.2026 10:40"))
            == "Gebietsverteidigung: Aylus (Stufe 1) am Fr 27.11. um 13:00 Uhr, in 2 Std 20 Min. Danach: Tazolka "
               "(Stufe 1) am Fr 27.11. um 18:00 Uhr.",
        "the next defense with its countdown, then the one after it");
  Check(DefenseTip(defenses, at("27.11.2026 13:30"))
            == "Gebietsverteidigung: Aylus (Stufe 1) läuft gerade (seit 13:00 Uhr). Danach: Tazolka (Stufe 1) am Fr "
               "27.11. um 18:00 Uhr.",
        "a running defense");
  Check(DefenseTip(defenses, at("27.11.2026 18:30"))
            == "Gebietsverteidigung: Tazolka (Stufe 1) läuft gerade (seit 18:00 Uhr).",
        "the last running defense");
  Check(!DefenseTip(defenses, at("27.11.2026 19:00")), "no tip once every defense has ended");
  defenses[1].level = 0;
  Check(DefenseTip(defenses, at("27.11.2026 13:30"))->starts_with("Gebietsverteidigung: Aylus läuft gerade"),
        "a defense without a level");

  LoaderDefense new_year;
  ParseDefenseTime("02.01. 13:00", new_year);
  Check(DefenseStart(new_year, at("31.12.2026 20:00")) == at("02.01.2027 13:00"),
        "a date without a year is the one closest to now");

  toml::table effective;
  const auto  config = Config::Parse(toml::parse(R"([graphics]
loader_tips = ["Erste Regel", "   ", 7, "Zweite Regel"]
loader_tips_game_share = 1.5
loader_defenses = [
  { system = "Aylus", level = 1, time = "27.11. 13:00" },
  { system = "Ohne Stufe", time = "2026-11-28 14:00" },
  { system = "Kaputt", level = 1, time = "Fr 13:00" },
  { level = 2, time = "27.11. 13:00" },
])"),
                                    effective, false);
  Check(config->graphics.loader_tips == std::vector<std::string>{"Erste Regel", "Zweite Regel"},
        "blank and non-string tips are skipped");
  Check(config->graphics.loader_tips_game_share == 0.5f, "a game share outside 0 to 1 keeps the default");
  Check(config->graphics.loader_defenses.size() == 2 && config->graphics.loader_defenses[0].system == "Aylus"
            && config->graphics.loader_defenses[1].level == 0,
        "valid defenses are kept and the level is optional");
  const auto* recorded = effective["graphics"]["loader_defenses"].as_array();
  Check(recorded && recorded->size() == 2, "the vars file lists the defenses used");
  Check(config->graphics.defense_timer, "the defense panel is on by default");

  defenses[1].level = 1;
  Check(DefensePanelText(defenses, at("27.11.2026 10:40"))
            == "Verteidigung: Aylus (Stufe 1)\nFr 13:00 Uhr, in 2 Std 20 Min",
        "the panel shows the next defense and its countdown");
  Check(DefensePanelText(defenses, at("27.11.2026 13:05")) == "Verteidigung: Aylus (Stufe 1)\nläuft gerade",
        "the panel shows a running defense");
  Check(!DefensePanelText(defenses, at("27.11.2026 19:00")), "no panel once every defense has ended");

  const std::vector<int32_t> leads{30, 10};
  int32_t                    sent = std::numeric_limits<int32_t>::max();
  Check(!DueReminder(31 * 60, leads, sent), "no reminder before the first lead");
  Check(DueReminder(30 * 60, leads, sent) == 30, "the first lead is sent");
  Check(!DueReminder(25 * 60, leads, sent), "a lead is sent once");
  Check(DueReminder(9 * 60, leads, sent) == 10, "the next lead follows");
  Check(!DueReminder(60, leads, sent) && !DueReminder(0, leads, sent), "nothing after the last lead or the start");
  sent = std::numeric_limits<int32_t>::max();
  Check(DueReminder(5 * 60, leads, sent) == 10 && !DueReminder(4 * 60, leads, sent),
        "a late start sends only the closest lead");

  const auto reminders = Config::Parse(toml::parse("[ui]\ndefense_reminders = \"10, 30, x, 0, 30, 2000\""),
                                       effective, false);
  Check(reminders->ui.defense_reminders == std::vector<int32_t>{30, 10}, "reminder minutes are sorted and checked");
  Check(effective["ui"]["defense_reminders"].value<std::string>() == "30, 10", "the vars file lists the reminders");
  Check(Config::Parse(toml::table{}, effective, false)->ui.defense_reminders.empty(), "reminders are off by default");
}

void FleetBar()
{
  using namespace fleet_bar;
  Check(Timer(48) == "0:48" && Timer(252) == "4:12" && Timer(5825) == "1:37:05" && Timer(-3) == "0:00",
        "fleet timers read as m:ss or h:mm:ss");
  Check(ShortName("Jäger", 14) == "Jäger", "a short name stays whole");
  Check(ShortName("Schlachtkreuzer", 14) == "Schlachtkreuz.", "a long name is cut to 14 characters with a dot");
  Check(ShortName("Größenwahnsinn!", 14) == "Größenwahnsin.", "umlauts count as one character");
  Check(ShortName("Größenwahnsinn", 14) == "Größenwahnsinn", "a name of exactly 14 characters stays whole");

  const Row repairing{1, "Schlachtkreuzer", Status::kRepairing, 252, std::nullopt};
  const auto line = Line(repairing, false);
  Check(line.find(">1</color>") != std::string::npos
            && line.find("<noparse>Schlachtkreuz.</noparse>") != std::string::npos
            && line.find("Reparatur") != std::string::npos && line.find("4:12") != std::string::npos,
        "a fleet line has key, name, status and time");
  const Row mining{3, "Bergbau", Status::kMining, 5825, 42};
  Check(Line(mining, false).find("Mining 42%") != std::string::npos, "mining shows how full the cargo is");
  const Row idle{6, "Frachter", Status::kIdle, std::nullopt, std::nullopt};
  Check(Line(idle, false).find("<color=#F0C26B><noparse>Frachter") != std::string::npos
            && Line(idle, false).find("<pos=80%>") == std::string::npos,
        "an idle ship's name stands out, and no timer column is drawn without a timer");
  Check(Line(repairing, true) == "<color=#8EA3BD>1</color> <color=#F29C50>4:12</color>"
            && Line(idle, true) == "<color=#8EA3BD>6</color> <color=#F0C26B>Leerlauf</color>",
        "the compact line has the key and the time, or the status");
  const Row rows[] = {repairing, idle};
  Check(Text(rows, true) == Line(repairing, true) + "\n" + Line(idle, true), "one line per ship");

  toml::table effective;
  const auto  defaults = Config::Parse(toml::table{}, effective, false);
  Check(defaults->ui.fleet_bar && defaults->ui.fleet_bar_side == FleetBarSide::kLeft && !defaults->ui.fleet_bar_compact,
        "the fleet bar is on, on the left and full by default");
  Check(Config::Parse(toml::parse("[ui]\nfleet_bar_side = \"Right\""), effective, false)->ui.fleet_bar_side
            == FleetBarSide::kRight,
        "the fleet bar moves to the right");
  Check(defaults->ui.fleet_bar_scale == 1.0f, "the fleet bar is drawn at its own size by default");
  Check(Config::Parse(toml::parse("[ui]\nfleet_bar_scale = 1.5"), effective, false)->ui.fleet_bar_scale == 1.5f,
        "the fleet bar grows by the scale");
  for (const auto* text : {"[ui]\nfleet_bar_scale = 0.2", "[ui]\nfleet_bar_scale = 3.0"})
    Check(Config::Parse(toml::parse(text), effective, false)->ui.fleet_bar_scale == 1.0f,
          "a fleet bar scale outside 0.5 to 2.5 keeps the default");

  const auto ship = [](Status status, std::optional<int32_t> cargo = std::nullopt) {
    return Row{4, "U.S.S. Dauntless", status, std::nullopt, cargo, 77};
  };
  Check(Change(ship(Status::kRepairing), ship(Status::kDocked)) == Event::kRepaired, "a finished repair is reported");
  Check(Change(ship(Status::kWarping), ship(Status::kIdle)) == Event::kArrived
            && Change(ship(Status::kImpulsing), ship(Status::kMining)) == Event::kArrived,
        "an end of warp or impulse is an arrival");
  Check(!Change(ship(Status::kWarpCharging), ship(Status::kWarping)), "charging into warp is no arrival");
  Check(Change(ship(Status::kMining, 95), ship(Status::kMining, 100)) == Event::kCargoFull
            && !Change(ship(Status::kMining, 100), ship(Status::kMining, 100))
            && !Change(ship(Status::kMining, 40), ship(Status::kMining, 41)),
        "a full cargo hold is reported once");
  Check(Change(ship(Status::kMining, 100), ship(Status::kIdle)) == Event::kMiningDone, "the end of mining is reported");
  Check(Change(ship(Status::kMining), ship(Status::kDestroyed)) == Event::kDestroyed
            && Change(ship(Status::kWarping), ship(Status::kDestroyed)) == Event::kDestroyed,
        "a destroyed ship is reported");
  Check(!Change(ship(Status::kMining), ship(Status::kBattling)) && !Change(ship(Status::kIdle), ship(Status::kIdle))
            && !Change(ship(Status::kIdle), ship(Status::kUnknown))
            && !Change(ship(Status::kDocked), ship(Status::kIdle)),
        "fights, unchanged and unknown states and leaving the dock report nothing");
  Check(EventText(Event::kRepaired) == "Reparatur fertig"
            && NotificationTitle(ship(Status::kIdle)) == "4 U.S.S. Dauntless",
        "a notification has the key and whole name, and German text");

  const auto quiet = defaults->ui.fleet_notifications;
  Check(!quiet.Any() && defaults->ui.notify_only_in_background,
        "no fleet notifications by default, and notifications only in the background");
  const auto chosen = Config::Parse(toml::parse("[ui]\nfleet_notifications = \"Repair, cargo_full, bogus\""), effective,
                                    false)
                          ->ui.fleet_notifications;
  Check(chosen.repaired && chosen.cargo_full && !chosen.arrived && !chosen.mining_done && !chosen.destroyed,
        "fleet notifications are chosen by name, and unknown names are left out");
}

void WindowPlacement()
{
  using namespace window_placement;
  const Placement placement{-1600, 80, -100, 1000, true};
  Check(Format(placement) == "-1600 80 -100 1000 1 0", "a placement is one line");
  Check(Parse(Format(placement)) == placement, "a placement reads back, also on a monitor left of the main one");
  Check(Parse(" 100 80 1700 1000 0\r") == Placement{100, 80, 1700, 1000, false}, "spaces and a CR are fine");
  Check(Parse("100 80 1700 1000 0") == Placement{100, 80, 1700, 1000, false, false},
        "a file from before the borderless flag reads as windowed");
  const Placement borderless{100, 80, 1700, 1000, false, true};
  Check(Format(borderless) == "100 80 1700 1000 0 1" && Parse(Format(borderless)) == borderless,
        "the borderless flag reads back");
  for (const auto* bad : {"", "100 80 1700 1000", "100 80 1700 1000 2", "100 80 1700 1000 0 7", "a b c d e",
                          "100 80 300 1000 0", "100 80 1700 200 0", "100 80 1700 1000 0 1 0", "100 80 1700 1000 0 x"})
    Check(!Parse(bad), "a malformed or tiny placement is ignored");

  toml::table effective;
  Check(Config::Parse(toml::table{}, effective, false)->graphics.remember_window, "window memory is on by default");
  Check(Config::Parse(toml::table{}, effective, false)->graphics.borderless_fullscreen, "F11 is on by default");
  Check(Config::Parse(toml::table{}, effective, false)->graphics.transition_time == 0.01f, "transition time default");
  for (const auto* text : {"[graphics]\ntransition_time = -1.0", "[graphics]\ntransition_time = nan"})
    Check(Config::Parse(toml::parse(text), effective, false)->graphics.transition_time == 0.01f,
          "a negative or invalid transition time keeps the default");
  Check(Config::Parse(toml::parse("[graphics]\ntransition_time = 0"), effective, false)->graphics.transition_time
            == 0.0f,
        "0 keeps the game's fade");
}

void UpstreamSettings()
{
  toml::table effective;
  const auto  defaults = Config::Parse(toml::table{}, effective, false);
  Check(defaults->graphics.haven_zoom == 4000.0f && defaults->ui.reverse_haven_history,
        "Haven defaults match upstream");
  Check(!defaults->ui.only_show_first_popup && defaults->audio.alert_victory == NotificationSound::kNone,
        "popup suppression and audio cues stay opt-in");
  for (const auto value : {"0.5", "inf", "nan"}) {
    const auto invalid_zoom =
        Config::Parse(toml::parse(std::string("[graphics]\ninterior_zoom_scale = ") + value), effective, false);
    Check(invalid_zoom->graphics.interior_zoom_scale == 2.0f,
          "invalid interior zoom scales fall back to the upstream default");
    Check(effective["graphics"]["interior_zoom_scale"].value<float>() == 2.0f,
          "effective settings report the interior zoom fallback");
  }
  const auto configured = Config::Parse(toml::parse(R"(
    [audio]
    disabled_events = " engine, aLl, music "
    alert_victory = "Victory"
    alert_defeat = "alarm"
    alert_armada_created = "ping"
    alert_armada_battle_won = "success"
    alert_armada_battle_lost = "not-a-sound"
    [graphics]
    haven_zoom = -1.0
    [ui]
    reverse_haven_history = false
    only_show_first_popup = true
    hud_q_trials = "always"
    hud_field_training = "never"
    hud_outposts = "ALWAYS"
    hud_missions = " auto "
  )"),
                                        effective, false);
  Check(configured->audio.disable_all_events
            && configured->audio.disabled_events == std::vector<std::string>{"engine", "music"},
        "All is a case-insensitive wildcard, other event names retain their spelling");
  Check(configured->audio.alert_victory == NotificationSound::kSuccess
            && configured->audio.alert_defeat == NotificationSound::kAlarm
            && configured->audio.alert_armada_created == NotificationSound::kPing
            && configured->audio.alert_armada_battle_won == NotificationSound::kSuccess
            && configured->audio.alert_armada_battle_lost == NotificationSound::kNone,
        "notification sound names, aliases and invalid fallback");
  Check(configured->ui.hud_q_trials == MissionHudVisibility::kAlways
            && configured->ui.hud_field_training == MissionHudVisibility::kNever
            && configured->ui.hud_outposts == MissionHudVisibility::kAlways
            && configured->ui.hud_missions == MissionHudVisibility::kAuto,
        "each HUD setting binds its own visibility");
  Check(Value(effective, "audio", "alert_victory") == "success"
            && Value(effective, "audio", "alert_armada_battle_lost") == "none",
        "effective sound settings use canonical names");
  Check(configured->graphics.haven_zoom == 4000.0f && !configured->ui.reverse_haven_history
            && configured->ui.only_show_first_popup,
        "Haven validation and new UI settings");
}

// Settings that were renamed still read their old names, and the new name wins when both are set.
void RenamedSettings()
{
  toml::table effective;
  const auto  old_names = Config::Parse(toml::parse(R"(
    [graphics]
    ui_scale_adjust = 0.1
    [ui]
    cargo_format = false
    hide_artifact_exchange_all = true
    [sync]
    officer = true
  )"),
                                        effective, false);
  Check(old_names->graphics.ui_scale_step == 0.1f, "ui_scale_adjust still sets the UI scale step");
  Check(!old_names->ui.format_cargo_values, "cargo_format still sets format_cargo_values");
  Check(old_names->ui.disable_exchange_all, "hide_artifact_exchange_all still sets disable_exchange_all");
  Check(effective["ui"]["disable_exchange_all"].value_or(false), "the vars file records the new name");
  Check(!effective["ui"]["hide_artifact_exchange_all"], "the vars file drops the old name");

  const auto both = Config::Parse(toml::parse(R"(
    [graphics]
    ui_scale_step = 0.2
    ui_scale_adjust = 0.1
  )"),
                                  effective, false);
  Check(both->graphics.ui_scale_step == 0.2f, "the new name wins over the old one");
}

void ParseDoesNotActivateSettings()
{
  Load("[shortcuts]\nquit = \"F1\"");
  toml::table effective;
  const auto  candidate = Config::Parse(toml::parse("[shortcuts]\nquit = \"F2\""), effective, false);
  Check(Shortcut::Describe(Action::kQuit) == "F1", "parsing settings leaves the live bindings alone");
  Check(Value(effective, "shortcuts", "quit") == "F2", "the candidate records its own bindings");
}

void Matching()
{
  Load(""); // defaults: move_down = "S|DOWN", toggle_shortcut_hints = "HOME"

  Hold({unity::KeyCode::DownArrow});
  Check(Shortcut::IsPressed(Action::kMoveDown), "default movement key");
  for (auto modifier : {unity::KeyCode::LeftControl, unity::KeyCode::RightShift, unity::KeyCode::LeftAlt,
                        unity::KeyCode::LeftCommand}) {
    Hold({unity::KeyCode::DownArrow, modifier});
    Check(!Shortcut::IsPressed(Action::kMoveDown), "a modified chord must not trigger the plain binding");
  }
  Hold({});
  Check(!Shortcut::IsPressed(Action::kMoveDown), "released key");

  // Releasing the modifier first must not turn a held CTRL-PGUP into PGUP.
  Load(""); // ui_scaleup = "PGUP", ui_scaleshipup = "CTRL-PGUP"
  Hold({unity::KeyCode::LeftControl, unity::KeyCode::PageUp});
  Check(Shortcut::IsPressed(Action::kUiShipScaleUp), "modified chord");
  Check(!Shortcut::IsPressed(Action::kUiScaleUp), "plain chord blocked while modified");
  Hold({unity::KeyCode::PageUp});
  Check(!Shortcut::IsPressed(Action::kUiScaleUp), "plain chord waits for the key's release");
  Hold({});
  Check(!Shortcut::IsPressed(Action::kUiScaleUp), "released key");
  Hold({unity::KeyCode::PageUp});
  Check(Shortcut::IsPressed(Action::kUiScaleUp), "plain chord after a release");

  // Keyboard panning reads only the move bindings. A modified move binding pans while held, even after the screen
  // shortcuts matched it earlier in the frame.
  Load("[shortcuts]\nmove_left = \"CTRL-LEFT\"");
  Hold({unity::KeyCode::LeftControl, unity::KeyCode::LeftArrow});
  Check(Shortcut::IsDown(Action::kMoveLeft), "modified movement chord");
  Check(Shortcut::IsPressed(Action::kMoveLeft), "a modified movement chord keeps panning once matched");

  // An arrow held for another CTRL-arrow chord does not pan, even after CTRL is released; W/A/S/D still do.
  Load("[shortcuts]\nui_scaleup = \"CTRL-LEFT\""); // move_left = "A|LEFT"
  Hold({unity::KeyCode::LeftControl, unity::KeyCode::LeftArrow});
  Check(Shortcut::IsPressed(Action::kUiScaleUp) && !Shortcut::IsPressed(Action::kMoveLeft),
        "a CTRL-arrow chord does not pan");
  Hold({unity::KeyCode::LeftArrow});
  Check(!Shortcut::IsPressed(Action::kMoveLeft), "its arrow pans only after a release");
  Hold({unity::KeyCode::LeftArrow, unity::KeyCode::A});
  Check(Shortcut::IsPressed(Action::kMoveLeft), "other movement keys still pan");
  held.fill(false);
}
} // namespace

// The fake Unity input that the shortcut code reads.
namespace unity::Key
{
bool Pressed(unity::KeyCode key)
{ return held[std::to_underlying(key)]; }
bool Down(unity::KeyCode key)
{ return Pressed(key); }
bool IsModified()
{
  return std::ranges::any_of(std::array{unity::KeyCode::LeftControl, unity::KeyCode::RightControl,
                                        unity::KeyCode::LeftShift, unity::KeyCode::RightShift, unity::KeyCode::LeftAlt,
                                        unity::KeyCode::RightAlt, unity::KeyCode::LeftCommand,
                                        unity::KeyCode::RightCommand},
                             Pressed);
}
} // namespace unity::Key

void ChatStartupGate()
{
  ChatStartup::Gate gate;
  // A slow initial load never opens, whatever the elapsed wall time.
  Check(!gate.Tick(false, false, 0) && !gate.Tick(false, false, 100), "chat gate waits while not ready");
  Check(!gate.Tick(true, false, 101) && !gate.Tick(true, false, 103.99), "chat gate waits three seconds");
  // A transition or lost readiness restarts the full settling period.
  Check(!gate.Tick(false, false, 104) && !gate.Tick(true, false, 110) && !gate.Tick(true, false, 112.99),
        "lost readiness restarts the chat gate");
  Check(gate.Tick(true, false, 113), "chat gate opens after three ready seconds");
  Check(!gate.Tick(true, false, 200), "chat gate opens once; a closed chat is not reopened");
  gate = {}; // Login or reload permits one new open.
  Check(!gate.Tick(true, false, 201) && gate.Tick(true, false, 204), "a reset chat gate opens again");
  gate = {};
  Check(!gate.Tick(false, true, 205) && !gate.Tick(true, false, 300) && !gate.Tick(true, false, 303),
        "chat already open consumes the gate, even before readiness");
}

void ReloadGate()
{
  reload::Gate gate;
  Check(!gate.suspended && !gate.Tick(false, 0), "normal startup is not suspended");
  gate.Begin();
  Check(gate.suspended && !gate.Tick(false, 10), "reload stays suspended without managers");
  Check(!gate.Tick(true, 20) && !gate.Tick(true, 22.9), "readiness must settle before mod work resumes");
  Check(!gate.Tick(false, 23) && !gate.Tick(true, 24), "a transition restarts the settling period");
  Check(!gate.Tick(true, 26.9) && gate.Tick(true, 27) && !gate.suspended, "stable readiness resumes work");
  Check(!gate.Tick(true, 30), "completion is emitted only once");
  gate.Begin();
  Check(!gate.Tick(true, 40), "a later reload suspends again");
  gate.Begin();
  Check(!gate.Tick(true, 43) && !gate.Tick(true, 45.9) && gate.Tick(true, 46),
        "nested teardown notifications restart the settling period");
}
void MouseNavigationGestures()
{
  {
    using namespace MouseNavigation;
    Check(Button(1, 0) == 3 && Button(-1, 0) == 4, "macOS Back and Forward map to Mouse3 and Mouse4");
    Check(Button(0, 1) == -1 && Button(0.1, 1) == -1 && Button(0, 0) == -1,
          "vertical gestures are not mouse navigation");
    Swipe swipe;
    Check(swipe.Observe(3, Phase::kDiscrete) == 3, "discrete Back gesture");
    Check(swipe.Observe(4, Phase::kBegan) == -1 && swipe.Observe(4, Phase::kChanged) == -1,
          "phased gesture waits for completion");
    Check(swipe.Observe(-1, Phase::kEnded) == 4 && swipe.Observe(-1, Phase::kEnded) == -1,
          "completed gesture emits once even when its final delta is zero");
    swipe.Observe(3, Phase::kBegan);
    Check(swipe.Observe(-1, Phase::kCancelled) == -1 && swipe.Observe(-1, Phase::kEnded) == -1,
          "cancelled gesture does not become a command");
  }
}
int main()
{
  ShipHints();
  StringHelpers();
  Timestamps();
  ToastText();
  ShipNames();
  Chords();
  KeyRanges();
  DefaultBindings();
  Loading();
  ExampleSettings();
  NotificationAudio();
  DailyFactionSettings();
  DefenseSchedule();
  FleetBar();
  WindowPlacement();
  UpstreamSettings();
  RenamedSettings();
  ParseDoesNotActivateSettings();
  Matching();
  ChatStartupGate();
  ReloadGate();
  MouseNavigationGestures();
  if (failures)
    return EXIT_FAILURE;
  std::cout << "mod logic tests passed\n";
  return EXIT_SUCCESS;
}
