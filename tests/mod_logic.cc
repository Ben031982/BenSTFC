// Settings parsing and shortcut matching, run against production code with Unity input replaced by a fake.
#include "config.h"
#include "features/chat_startup.h"
#include "features/defense_schedule.h"
#include "features/mouse_navigation.h"
#include "features/notification_audio.h"
#include "features/ship_shortcut_hint.h"
#include "features/toast_text.h"
#include "features/announcements.h"
#include "features/battle_summary.h"
#include "features/chat_highlight.h"
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
  Check(ParseDefenseTime("Fr 13:00", parsed) && parsed.weekday == 5 && parsed.month == 0 && parsed.hour == 13,
        "a weekly defense");
  Check(ParseDefenseTime("sonntag 9:30", parsed) && parsed.weekday == 0 && ParseDefenseTime("Sat. 18:00", parsed)
            && parsed.weekday == 6,
        "weekdays in German or English, long or short");
  Check(ParseDefenseTime("27.11. 13:00", parsed) && parsed.weekday == -1, "a date is no weekly defense");
  for (const auto* bad : {"", "27.11.", "Fx 13:00", "Fr 25:00", "Fr", "27.11. 24:00",
                          "32.11. 13:00", "27.13. 13:00", "27.11. 13",
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

  // 27.11.2026 is a Friday.
  LoaderDefense weekly;
  ParseDefenseTime("Fr 13:00", weekly);
  Check(DefenseStart(weekly, at("25.11.2026 20:00")) == at("27.11.2026 13:00"), "a weekly defense later this week");
  Check(DefenseStart(weekly, at("27.11.2026 13:30")) == at("27.11.2026 13:00"), "a weekly defense still running");
  Check(DefenseStart(weekly, at("27.11.2026 14:00")) == at("04.12.2026 13:00"), "then the one a week later");
  ParseDefenseTime("Do 23:30", weekly);
  Check(DefenseStart(weekly, at("27.11.2026 00:10")) == at("26.11.2026 23:30"),
        "a weekly defense running past midnight");
  std::vector<LoaderDefense> week(2);
  ParseDefenseTime("Sa 14:00", week[0]);
  ParseDefenseTime("Fr 18:00", week[1]);
  week[0].system = "Duportas";
  week[1].system = "Tazolka";
  Check(DefensePanelText(week, at("28.11.2026 15:30")) == "Verteidigung: Tazolka\nFr 18:00 Uhr, in 6 T 2 Std",
        "after the last one of the week, the first one of the next");

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
  { system = "Kaputt", level = 1, time = "Fx 13:00" },
  { level = 2, time = "27.11. 13:00" },
])"),
                                    effective, false);
  Check(config->graphics.loader_tips == std::vector<std::string>{"Erste Regel", "Zweite Regel"},
        "blank and non-string tips are skipped");
  Check(config->graphics.loader_tips_game_share == 0.5f, "a game share outside 0 to 1 keeps the default");
  Check(config->graphics.loader_defenses.size() == 2 && config->graphics.loader_defenses[0].system == "Aylus"
            && config->graphics.loader_defenses[1].level == 0,
        "valid defenses are kept and the level is optional");
  const auto* recorded = effective["ngo"]["loader_defenses"].as_array();
  Check(recorded && recorded->size() == 2, "the vars file lists the defenses used");
  Check(config->graphics.defense_timer, "the defense panel is on by default");
  {
    toml::table effective_scale;
    Check(Config::Parse(toml::table{}, effective_scale, false)->graphics.defense_timer_scale == 1.5f,
          "the defense panel is drawn larger by default");
    Check(Config::Parse(toml::parse("[graphics]\ndefense_timer_scale = 2.0"), effective_scale, false)
                  ->graphics.defense_timer_scale
              == 2.0f,
          "the defense panel takes its size");
    Check(Config::Parse(toml::parse("[graphics]\ndefense_timer_scale = 9"), effective_scale, false)
                  ->graphics.defense_timer_scale
              == 1.5f,
          "a defense panel size out of range keeps the default");
  }

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
  Check(effective["ngo"]["defense_reminders"].value<std::string>() == "30, 10", "the vars file lists the reminders");
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
            && Line(idle, false).find("<pos=77%>") == std::string::npos,
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

void ChatHighlight()
{
  using namespace chat_highlight;
  const auto words = Words(" Ben, NGO ,, Armada ");
  Check(words == std::vector<std::string>{"Ben", "NGO", "Armada"}, "highlight words are trimmed, blanks left out");

  Check(!Mark("Hallo zusammen", words, "#FFD54A", false), "a message without a word stays as it is");
  Check(!Mark("Benjamin und Armadas", words, "#FFD54A", false), "only whole words count");
  Check(Mark("armada um 20 Uhr, ben?", words, "#FFD54A", false)
            == "<mark=#FFD54A33><b><color=#FFD54A>armada</color></b> um 20 Uhr, <b><color=#FFD54A>ben</color></b>?"
               "</mark>",
        "words are found in any case and marked with the message");
  Check(Mark("<color=#ff0000>NGO</color> <link=\"Ben\">x</link>", words, "#FFD54A", false)
            == "<mark=#FFD54A33><color=#ff0000><b><color=#FFD54A>NGO</color></b></color> <link=\"Ben\">x</link></mark>",
        "tags are kept and not searched");
  Check(Mark("<b>NGO</b>", words, "#FFD54A", true)
            == "<mark=#FFD54A33><noparse><b></noparse><b><color=#FFD54A><noparse>NGO</noparse></color></b>"
               "<noparse><​/b></noparse></mark>",
        "a plain-text label shows the message as typed");
  Check(!Mark("<mark=#FFD54A33>NGO</mark>", words, "#FFD54A", false), "a marked message is not marked again");
  const std::vector<std::string> umlauts{"Größe"};
  Check(Mark("Für Größe: Ben!", umlauts, "#FFD54A", false).has_value(),
        "words with umlauts are found");

  Check(ValidColor("#FFD54A") && ValidColor("#00ff7f") && !ValidColor("FFD54A") && !ValidColor("#FFD54")
            && !ValidColor("#GGGGGG"),
        "colors are #RRGGBB");
  toml::table effective;
  Check(Config::Parse(toml::table{}, effective, false)->ui.chat_highlight_words.empty()
            && Config::Parse(toml::table{}, effective, false)->ui.chat_highlight_color == "#FFD54A",
        "chat highlighting is off by default, in amber");
  Check(Config::Parse(toml::parse("[ui]\nchat_highlight_color = \"red\""), effective, false)->ui.chat_highlight_color
            == "#FFD54A",
        "an invalid highlight color keeps the default");
}

void NgoSection()
{
  toml::table effective;
  const auto  ngo = Config::Parse(toml::parse("[ngo]\nfleet_bar_scale = 1.5\ngalaxy_zoom = 3.0"), effective, false);
  Check(ngo->ui.fleet_bar_scale == 1.5f && ngo->graphics.galaxy_zoom == 3.0f, "[ngo] sets the NGO settings");
  Check(effective["ngo"]["fleet_bar_scale"].value<float>() == 1.5f && !effective["ui"]["fleet_bar_scale"],
        "the vars file records them under [ngo]");
  const auto old = Config::Parse(toml::parse("[ui]\nfleet_bar_scale = 1.2\n[graphics]\ngalaxy_zoom = 2.0"), effective,
                                 false);
  Check(old->ui.fleet_bar_scale == 1.2f && old->graphics.galaxy_zoom == 2.0f, "their first sections still work");
  const auto both = Config::Parse(toml::parse("[ui]\nfleet_bar_scale = 1.2\n[ngo]\nfleet_bar_scale = 2.0"), effective,
                                  false);
  Check(both->ui.fleet_bar_scale == 2.0f, "[ngo] wins over the first section");
  Check(Config::Parse(toml::parse("[ngo]\nui_scale = 0.5"), effective, false)->graphics.ui_scale != 0.5f,
        "other settings are not read from [ngo]");
}

void Announcements()
{
  using namespace announcements;
  const auto set = Parse("!NGO Armada um 20 Uhr");
  Check(set.command == Command::kSet && set.text == "Armada um 20 Uhr", "!NGO sets an announcement");
  Check(Parse("  !ngo:  Treffpunkt Vulcan ").text == "Treffpunkt Vulcan", "case, a colon and spaces do not matter");
  Check(Parse("!NGO-ENDE").command == Command::kEnd && Parse("!ngo-ende danke").command == Command::kEnd,
        "!NGO-ENDE ends it");
  Check(Parse("!NGO").command == Command::kNone && Parse("!NGOX hallo").command == Command::kNone
            && Parse("Hallo !NGO test").command == Command::kNone && Parse("!NGO-ENDEN").command == Command::kNone,
        "anything else is no command");

  State state;
  Check(Apply(state, Parse("!NGO Erste"), "Ben", 1000) && state.current->text == "Erste", "a command sets it");
  Check(!Apply(state, Parse("!NGO Ältere"), "Motarr", 900) && state.current->text == "Erste",
        "an older announcement does not replace a newer one");
  Check(Apply(state, Parse("!NGO Zweite"), "Motarr", 1100) && state.current->sender == "Motarr",
        "a newer one replaces it");
  Check(Apply(state, Parse("!NGO-ENDE"), "Ben", 1200) && !state.current && state.ended == 1200, "the end clears it");
  Check(!Apply(state, Parse("!NGO Zweite"), "Motarr", 1100) && !state.current,
        "history read again does not bring an ended announcement back");
  Check(!Apply(state, Parse("Hallo"), "Ben", 1300), "plain chat changes nothing");

  Apply(state, Parse("!NGO Dritte\tmit Tab"), "Ben", 2000);
  const auto reloaded = announcements::Load(Save(state));
  Check(reloaded.ended == 1200 && reloaded.current && reloaded.current->posted == 2000
            && reloaded.current->sender == "Ben" && reloaded.current->text == "Dritte mit Tab",
        "the announcement survives a restart");
  Check(!announcements::Load("ended\t3000\n2000\tBen\tAlt\n").current,
        "an announcement older than the end is not loaded");
  Check(!announcements::Load("kaputt\nnoch\tkaputter\n").current, "a broken file loads as nothing");

  Check(Shown(state, 2000 + 7 * 24 * 3600, 7).has_value() && !Shown(state, 2001 + 7 * 24 * 3600, 7),
        "an announcement disappears after the configured days");
  Check(UnixSeconds(1763830000.5) == 1763830000 && UnixSeconds(1763830000500.0) == 1763830000 && UnixSeconds(0) == 0,
        "chat timestamps in seconds or milliseconds");
  Check(BannerText({1, "Ben", "<b>Hi</b>"}).find("<noparse><b>Hi<​/b></noparse>") != std::string::npos,
        "the banner shows the text as typed");
  Check(TipText({1, "Ben", "<b>Hi</b>"}, true).find("von <noparse>Ben</noparse>: <noparse><b>Hi<​/b></noparse>")
                != std::string::npos
            && TipText({1, "Ben", "Armada\num 20"}, false) == "NGO-Ankündigung von Ben: Armada um 20",
        "the loading tip shows the announcement as typed");
  Check(LeaderRank(3, 4) && LeaderRank(1, 0) && !LeaderRank(2, 4) && !LeaderRank(0, 4),
        "the rank next to the admiral's may announce");
  Check(!LeaderRank(4, 4) && !LeaderRank(0, 0), "the admiral's own rank does not, the admiral is known by id");

  toml::table effective;
  const auto  defaults = Config::Parse(toml::table{}, effective, false);
  Check(defaults->ui.announcements && defaults->ui.announcement_max_days == 7, "announcements are on, for 7 days");
  Check(Config::Parse(toml::parse("[ui]\nannouncement_max_days = 0"), effective, false)->ui.announcement_max_days == 7,
        "an invalid number of days keeps the default");
}

void BattleSummary()
{
  using namespace battle_summary;
  // A report as the game exports it (German client), with the player and alliance renamed.
  constexpr std::string_view kReport = R"csv(Spielername	Spielerlevel	Ergebnis	Schiffsname	Schiffslevel	Schiffsstärke	Schiff-EP	Offizier Eins	Offizier Zwei	Offizier Drei	Hüllen-TP	Verbleibende Hüllen-TP	Schild-TP	Verbleibende Schild-TP	Standort	Zeitstempel
Spieler	75	SIEG	U.S.S. DAUNTLESS	75	2118563072	0	Rachel Garrett	Phellun	SNW Black Ops Chapel	1444904344	1441984018	1625185219	1611417972	Mirror Romulus	22.11.2026 10:36:20
Abbauer der Romulanischen Republik	70	NIEDERLAGE	Abbauer der Romulanischen Republik	70	85754028032	0	Commander der Romulanischen Republik	--	--	97264487838	0	64842991892	0	Mirror Romulus	22.11.2026 10:36:20

Belohnungsname	Anzahl
Riesentruhe	1

Flottentyp	Angreifen	Verteidigung	Gesundheit	Schiffsfähigkeit	MANÖVER DES CAPTAINS	Offizier-Eins-Fähigkeit	Offizier-Zwei-Fähigkeit	Offizier-Drei-Fähigkeit	Offiziersangriffsbonus	Schaden pro Runde	Panzerungsdurchbruch	Schilddurchbruch	Treffsicherheit	Kritische Trefferchance	Kritischer Schaden	Offiziersverteidigungsbonus	Panzerung	Schildablenkung	Ausweichen	Offiziersgesundheitsbonus	Schild-TP	Hüllen-TP	Impulsgeschwin­digkeit	Warpreichweite	Warpgeschwindigkeit	Frachtkapazität	Geschützte Fracht	Abbaubonus	Debuff angewendet	Buff angewendet
Spielerflotte 1	228673838	58299405	1831589888	Aktive Aufklärung---Heckausbau---Gruppierungs-Plünderung---Aktive Apex-Barriere	Befehlskette	Glorreiche Zukunft	Ruf einen Fachmann	Wir schaffen das	5	221620897	8444880	683775	669546	0,22	5,33	5	534192	499942	10625747	5	1625185219	2037994488	297	2128	2280,47	62207400	0	1	NEIN	NEIN
Feindliche Flotte 1	1988350080	2711939760	81053739008	Demontage---Zwickmühle---Fotophobie	--	--	--	--	0	980311559	336012832	336012832	336012832	0,1	1,5	0	180795984	180795984	180795984	0	64842993664	97264484352	50	2	2	0	0	1	NEIN	NEIN

Runde	Kampf-Ereignis	Typ	Angreifername	Angreiferallianz	Angreiferschiff	Angreifer - Ist Armada?	Zielname	Zielallianz	Zielschiff	Ziel - Ist Armada?	Hyperthermischer Zerfall %	Hyperthermischer Stabilisator %	Kritischer Treffer?	Hüllenschaden	Schildschaden	Abgemilderter Schaden	Abgemilderter Isolytischer Schaden	Abgemilderte Apex-Barriere	Gesamtschaden	Isolytischer Totalschaden	Abgemilderter kritischer Schaden	Polaronschaden	Gemilderter Polaronschaden	Infusionsschaden	Gemilderter Infusionsschaden	Fähigkeitstyp	Fähigkeitswert	Fähigkeitsbezeichnung	Name der Figur mit der Fähigkeit	Ziel besiegt	Ziel zerstört	Waffen laden %
1	1	Offizierfähigkeit	Spieler	Allianz	U.S.S. DAUNTLESS	NEIN	--	--	U.S.S. DAUNTLESS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	8	Eine Kampfchance	ZdK-Leonard-McCoy	--	--	--
1	2	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	U.S.S. DAUNTLESS	NEIN	--	--	U.S.S. DAUNTLESS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	30	--	Agoniestand	--	--	--
1	3	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	U.S.S. DAUNTLESS	NEIN	--	--	U.S.S. DAUNTLESS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	2000	--	Agoniestand	--	--	--
1	4	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	U.S.S. DAUNTLESS	NEIN	--	--	U.S.S. DAUNTLESS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	100	--	Gedankenmacher	--	--	--
1	5	Angreifen	Abbauer der Romulanischen Republik	--	Abbauer der Romulanischen Republik	NEIN	Spieler	Allianz	U.S.S. DAUNTLESS	NEIN	--	--	NEIN	1457796	6872465	130685112	17696926	51264396	189069723	18906972	0	0	0	0	0	--	--	--	--	--	--	--
1	6	Angreifen	Abbauer der Romulanischen Republik	--	Abbauer der Romulanischen Republik	NEIN	Spieler	Allianz	U.S.S. DAUNTLESS	NEIN	--	--	NEIN	1462530	6894782	131109488	17754394	51430872	189683696	18968370	0	0	0	0	0	--	--	--	--	--	--	--
1	7	Angreifen	Spieler	Allianz	U.S.S. DAUNTLESS	NEIN	Abbauer der Romulanischen Republik	--	Abbauer der Romulanischen Republik	NEIN	--	--	NEIN	6115108352	21063151616	1639228416	0	0	2495019520	26322468864	0	0	0	0	0	--	--	--	--	--	--	--
1	8	Angreifen	Spieler	Allianz	U.S.S. DAUNTLESS	NEIN	Abbauer der Romulanischen Republik	--	Abbauer der Romulanischen Republik	NEIN	--	--	JA	118263963648	407353655296	31702077440	0	0	48252792832	509066903552	0	0	0	0	0	--	--	--	--	--	--	--
1	9	Schild erschöpft	Abbauer der Romulanischen Republik	--	Abbauer der Romulanischen Republik	NEIN	Spieler	Allianz	Abbauer der Romulanischen Republik	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	JA	--	--
1	10	Kämpfer vernichtet	Abbauer der Romulanischen Republik	--	Abbauer der Romulanischen Republik	NEIN	Spieler	Allianz	Abbauer der Romulanischen Republik	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	JA	--
)csv";
  const auto summary = Parse(kReport);
  Check(summary && summary->sides.size() == 2, "both sides of the report");
  const auto& player = summary->sides[0];
  const auto& enemy  = summary->sides[1];
  Check(player.name == "Spieler" && player.result == "SIEG" && player.ship == "U.S.S. DAUNTLESS"
            && enemy.name == "Abbauer der Romulanischen Republik" && enemy.level == "70",
        "the player's side first, then the enemy");
  Check(player.officers == std::vector<std::string>{"Rachel Garrett", "Phellun", "SNW Black Ops Chapel"}
            && enemy.officers.size() == 1,
        "the bridge officers, without empty seats");
  Check(player.hull == 1444904344 && player.hull_left == 1441984018 && enemy.shield_left == 0,
        "hull and shield before and after");
  Check(summary->location == "Mirror Romulus" && summary->time == "22.11.2026 10:36:20", "where and when");
  Check(summary->rewards.size() == 1 && summary->rewards[0].first == "Riesentruhe" && summary->rewards[0].second == "1",
        "the rewards");
  Check(summary->rounds == 1 && summary->hits_out == 2 && summary->hits_in == 2 && summary->crits_out == 1
            && summary->crits_in == 0,
        "rounds, hits and crits from the battle log");
  Check(summary->unread.empty(), "every section of this report is read");
  Check(Describe(*summary)
            == "SIEG: U.S.S. DAUNTLESS gegen Abbauer der Romulanischen Republik (70) in Mirror Romulus, 22.11.2026 "
               "10:36:20; Runden 1; Treffer 2 raus / 2 rein; Crits 1 raus / 0 rein; Schaden raus 97 Mrd Hülle + 65 "
               "Mrd Schild; Schaden rein 2,9 Mio Hülle + 14 Mio Schild; Hülle danach 99 %; Brücke: Rachel Garrett, "
               "Phellun, SNW Black Ops Chapel; Beute: Riesentruhe 1",
        "the summary line");

  Check(summary->hull_in == 2920326 && summary->shield_in == 13767247 && Repaired(*summary) == 0,
        "the damage taken, hit by hit, matches what the hull and shield lost");

  // Three rounds, a repairing officer below deck, numbers in exponent notation and the anomalies' buffs.
  constexpr std::string_view kLongReport = R"csv(Spielername	Spielerlevel	Ergebnis	Schiffsname	Schiffslevel	Schiffsstärke	Schiff-EP	Offizier Eins	Offizier Zwei	Offizier Drei	Hüllen-TP	Verbleibende Hüllen-TP	Schild-TP	Verbleibende Schild-TP	Standort	Zeitstempel
Spieler	75	SIEG	PRAEFORTIS	35	1052866904064	0	Planetfall Kirk	Planetfall Scotty	Planetfall Uhura	871670906939	857523109286	1166328469779	1165548863795	Xerionis ∃	22.11.2026 17:14:43
Föderations-Polaron-Prototyp-∃	72	NIEDERLAGE	Föderations-Polaron-Prototyp-∃	72	2157803339776	0	Captain der Sternenflotte	--	--	3637222589290	0	1,50000000000001E+16	1,49968274105638E+16	Xerionis ∃	22.11.2026 17:14:43

Belohnungsname	Anzahl
Romulanisches Lob 7⇵	308
Polaronionen 7⇵	154
Riesentruhe	1

[PH] Galactic Anomalies Name	[PH] Buff Value
Verlorener Sohn	0,15
Unendlich Weiter	0,15
Kirks Kühnheit	10000
Rücksichtsloses Vorgehen	0,15

Flottentyp	Angreifen	Verteidigung	Gesundheit	Schiffsfähigkeit	MANÖVER DES CAPTAINS	Offizier-Eins-Fähigkeit	Offizier-Zwei-Fähigkeit	Offizier-Drei-Fähigkeit	Offiziersangriffsbonus	Schaden pro Runde	Panzerungsdurchbruch	Schilddurchbruch	Treffsicherheit	Kritische Trefferchance	Kritischer Schaden	Offiziersverteidigungsbonus	Panzerung	Schildablenkung	Ausweichen	Offiziersgesundheitsbonus	Schild-TP	Hüllen-TP	Impulsgeschwin­digkeit	Warpreichweite	Warpgeschwindigkeit	Frachtkapazität	Geschützte Fracht	Abbaubonus	Debuff angewendet	Buff angewendet
Spielerflotte 1	163797528080	109239350400	779830034432	Durch den Schleier---Isolytischer Schild	Defensiver Fokus	Zerstörungskraft	Infusionskondensatorverstärkung	Zahlreiche Kondensatoren	56	160859121293	461170695	455286540	1307385805	0,42	6,83	56	1924118880	18167330168	1756420327	56	789637589944	770022538806	205	3803	2712,56	1668520	0	1	NEIN	NEIN
Feindliche Flotte 1	25360216512	7777445360	2124665618432	Polaronwaffen­technologie ∃---Experimentelles Verteidigungssys.	--	--	--	--	0	21061931868	402801792	446980064	1299360640	0,1	1,5	0	173987488	1201305728	180195856	0	612108664832	3637222572032	80	2	2	0	0	1	NEIN	NEIN

Runde	Kampf-Ereignis	Typ	Angreifername	Angreiferallianz	Angreiferschiff	Angreifer - Ist Armada?	Zielname	Zielallianz	Zielschiff	Ziel - Ist Armada?	Hyperthermischer Zerfall %	Hyperthermischer Stabilisator %	Kritischer Treffer?	Hüllenschaden	Schildschaden	Abgemilderter Schaden	Abgemilderter Isolytischer Schaden	Abgemilderte Apex-Barriere	Gesamtschaden	Isolytischer Totalschaden	Abgemilderter kritischer Schaden	Polaronschaden	Gemilderter Polaronschaden	Infusionsschaden	Gemilderter Infusionsschaden	Fähigkeitstyp	Fähigkeitswert	Fähigkeitsbezeichnung	Name der Figur mit der Fähigkeit	Ziel besiegt	Ziel zerstört	Waffen laden %
1	1	Hyperthermischer Stabilisator	--	--	VERTEIDIGUNGSPLATTFORM A	NEIN	--	--	VERTEIDIGUNGSPLATTFORM A	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--
1	2	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	20000	Zerstörungskraft	Planetfall Kirk	--	--	--
1	3	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	0,1	Infusionskondensatorverstärkung	Planetfall Scotty	--	--	--
1	4	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1250	Zahlreiche Kondensatoren	Planetfall Uhura	--	--	--
1	5	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Defensiver Fokus	Planetfall Kirk	--	--	--
1	6	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Umgehauen	B'Elanna Torres	--	--	--
1	7	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Leitender Moraloffizier	Neelix	--	--	--
1	8	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	50	--	Tan Ru	--	--	--
1	9	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	5	--	Tan Ru	--	--	--
1	10	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	0,25	--	Quanten-Windschattenantrieb	--	--	--
1	11	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3552677888	0	0	0	0	0	0	0	3552677888	0	0	0	--	--	--	--	--	--	--
1	12	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	JA	5245763072	0	0	0	0	0	0	0	52457631744	0	0	0	--	--	--	--	--	--	--
1	13	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	357120606208	309978628096	1030651117568	15188952612864	0	-3,57106811339515E+16	15498931240960	0	0	0	3,57120603607532E+16	-8544922112	--	--	--	--	--	--	--
1	14	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	386973368320	335890644992	1116806184960	16458641833984	0	-3,86958251849047E+16	16794532478976	0	0	0	3,86973375093801E+16	8544922112	--	--	--	--	--	--	--
1	15	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3319058688	0	0	0	0	0	0	0	3319058688	0	0	0	--	--	--	--	--	--	--
1	16	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3315980544	0	0	0	0	0	0	0	3315980544	0	0	0	--	--	--	--	--	--	--
1	17	Waffen laden	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	100
1	18	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	27582484	130031704	1156442368	0	311131072	1625187628	0	0	0	0	0	0	--	--	--	--	--	--	--
1	19	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	27237918	128407328	1141995904	0	307244384	1604885534	0	0	0	0	0	0	--	--	--	--	--	--	--
1	20	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	27501666	129650712	1153053952	0	310219456	1620425786	0	0	0	0	0	0	--	--	--	--	--	--	--
1	21	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	357612060672	310405201920	1032069447680	15209855975424	0	-3,57469324384666E+15	15520261177344	0	0	0	3,57612076715213E+15	37841797120	--	--	--	--	--	--	--
1	22	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	371883245568	322792521728	1073256136704	15816832581632	0	-3,71871194174587E+16	16139625103360	0	0	0	3,71883265197343E+16	-238037106688	--	--	--	--	--	--	--
1	23	Waffen laden	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	100
1	24	Waffen laden	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	100
2	26	Hyperthermischer Stabilisator	--	--	VERTEIDIGUNGSPLATTFORM A	NEIN	--	--	VERTEIDIGUNGSPLATTFORM A	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--
2	27	battle_action_type_{0}	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	11636850000	--	--	--	--
2	28	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Umgehauen	B'Elanna Torres	--	--	--
2	29	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Leitender Moraloffizier	Neelix	--	--	--
2	30	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	0,35	Leben zurückerobern	STP Hugh	--	--	--
2	31	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	0,25	--	Quanten-Windschattenantrieb	--	--	--
2	32	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3291800576	0	0	0	0	0	0	0	3291800576	0	0	0	--	--	--	--	--	--	--
2	33	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3343749888	0	0	0	0	0	0	0	3343749888	0	0	0	--	--	--	--	--	--	--
2	34	Waffen laden	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	100
2	35	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3333193472	0	0	0	0	0	0	0	3333193472	0	0	0	--	--	--	--	--	--	--
2	36	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3410575360	0	0	0	0	0	0	0	3410575360	0	0	0	--	--	--	--	--	--	--
2	37	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	331161042944	287445876736	955731738624	14084847173632	0	-3,31148181689985E+16	14372293050368	0	0	0	3,31161048176394E+16	-244140624	--	--	--	--	--	--	--
2	38	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	399646556160	346890895360	1153381040128	16997653938176	0	-3,99499177642394E+15	17344544833536	0	0	0	3,99646545831526E+15	-79345704960	--	--	--	--	--	--	--
2	39	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	27908292	131567664	1170102528	0	314806208	1644384692	0	0	0	0	0	0	--	--	--	--	--	--	--
2	40	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	28316942	133494160	1187235840	0	319415808	1668462750	0	0	0	0	0	0	--	--	--	--	--	--	--
2	41	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	26823664	126454424	1124627712	0	302571584	1580477384	0	0	0	0	0	0	--	--	--	--	--	--	--
2	42	Waffen laden	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	100
2	43	Waffen laden	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	100
2	44	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	359782318080	312289001472	1038332854272	15302160023552	0	-3,59642536586709E+15	15614449025024	0	0	0	3,59782323689882E+15	-244140624	--	--	--	--	--	--	--
2	45	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	378040614912	328137048064	1091026288640	16078715486208	0	-3,77897238823322E+15	16406852534272	0	0	0	3,78040605474816E+15	-35400388608	--	--	--	--	--	--	--
3	47	Hyperthermischer Stabilisator	--	--	VERTEIDIGUNGSPLATTFORM A	NEIN	--	--	VERTEIDIGUNGSPLATTFORM A	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--
3	48	battle_action_type_{0}	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	10096780000	--	--	--	--
3	49	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Umgehauen	B'Elanna Torres	--	--	--
3	50	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Leitender Moraloffizier	Neelix	--	--	--
3	51	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	0,35	Leben zurückerobern	STP Hugh	--	--	--
3	52	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	0,25	--	Quanten-Windschattenantrieb	--	--	--
3	53	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3347119104	0	0	0	0	0	0	0	3347119104	0	0	0	--	--	--	--	--	--	--
3	54	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	3556135936	0	0	0	0	0	0	0	3556135936	0	0	0	--	--	--	--	--	--	--
3	55	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	323282862080	280607653888	932995268608	13749774712832	0	-322026591093497	14030382366720	0	0	0	323282859458560	-9765625	--	--	--	--	--	--	--
3	56	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	389578588160	338151964672	1124324999168	16569445908480	0	-388024412119040	16907597873152	0	0	0	389578598907904	40283201536	--	--	--	--	--	--	--
3	57	Kämpfer vernichtet	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	Föderations-Polaron-Prototyp-∃	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	JA	--)csv";
  const auto long_summary = Parse(kLongReport);
  Check(long_summary && long_summary->sides.size() == 2, "both sides of the long report");
  Check(long_summary->sides[1].shield == 15000000000000100 && long_summary->sides[1].shield_left == 14996827410563800,
        "a shield in exponent notation");
  Check(long_summary->rewards.size() == 3 && long_summary->rewards[2].first == "Riesentruhe"
            && long_summary->buffs.size() == 4 && long_summary->buffs[2].first == "Kirks Kühnheit",
        "the buffs are no loot");
  Check(long_summary->rounds == 3 && long_summary->hits_out == 10 && long_summary->hits_in == 16
            && long_summary->crits_out == 10 && long_summary->crits_in == 1,
        "three rounds of hits and crits");
  Check(long_summary->hull_in == 35881425494 && long_summary->shield_in == 779605992
            && Repaired(*long_summary) == 21733627841,
        "the damage taken and the hull repaired in between");
  Check(long_summary->unread.empty(), "every section of the long report is read");
  Check(Describe(*long_summary)
            == "SIEG: PRAEFORTIS gegen Föderations-Polaron-Prototyp-∃ (72) in Xerionis ∃, 22.11.2026 17:14:43; "
               "Runden 3; Treffer 10 raus / 16 rein; Crits 10 raus / 1 rein; Schaden raus 3,6 Bio Hülle + 3,2 Bio "
               "Schild; Schaden rein 36 Mrd Hülle (22 Mrd repariert) + 780 Mio Schild; Hülle danach 98 %; Brücke: "
               "Planetfall Kirk, Planetfall Scotty, Planetfall Uhura; Beute: Romulanisches Lob 7⇵ 308, Polaronionen "
               "7⇵ 154, Riesentruhe 1; Buffs: Verlorener Sohn 0,15, Unendlich Weiter 0,15, Kirks Kühnheit 10000, "
               "Rücksichtsloses Vorgehen 0,15",
        "the long summary line");

  std::string missed(kLongReport);
  missed += "\n3\t66\tAngreifen\tGegner\t--\tGegner\tNEIN\tSpieler\tAllianz\tPRAEFORTIS\tNEIN\t--\t--\tJA";
  for (int cell = 14; cell <= 25; ++cell)
    missed += "\t0";
  for (int cell = 26; cell <= 32; ++cell)
    missed += "\t--";
  const auto with_miss = Parse(missed);
  Check(with_miss && with_miss->hits_in == 16 && with_miss->crits_in == 1,
        "a shot without any damage is no hit, also when it is marked critical");

  const auto panel = PanelText(*long_summary);
  Check(panel.starts_with("<size=130%><b><color=#5EE07A><noparse>SIEG</noparse></color></b></size>\n"),
        "the panel starts with the result, in green for a win");
  Check(panel.contains("<color=#8FA3B8>Repariert</color><pos=22%>22 Mrd Hülle")
            && panel.contains("<noparse>Planetfall Uhura</noparse>")
            && panel.ends_with("<noparse>Riesentruhe</noparse><pos=40%><noparse>1</noparse>"),
        "the panel shows the repair, the bridge and the loot");
  Check(PanelLines(panel) == 19, "the panel's lines: three over the whole width, a gap, then the longer column");

  Check(long_summary->dealt.isolytic == 158629469683712 && long_summary->dealt.isolytic_blocked == 155456880246784
            && long_summary->dealt.mitigated == 10548575076352 && long_summary->dealt.polaron == 0
            && long_summary->taken.polaron == 82927923200 && long_summary->taken.mitigated == 6933458304
            && long_summary->taken.apex == 1865388512 && long_summary->taken.isolytic == 0
            && long_summary->dealt.infusion == 3655081263104 && long_summary->taken.infusion == 0,
        "the damage by type, dealt and taken");
  Check(panel.contains("<pos=50%><color=#8FA3B8>Polaron</color><pos=72%><color=#5EE07A>0</color><pos=87%>"
                        "<color=#FF6B6B>83 Mrd</color>")
            && panel.contains("<pos=50%><color=#8FA3B8>Infusion</color><pos=72%><color=#5EE07A>3,7 Bio</color>")
            && !panel.contains("Krit abgewehrt"),
        "the damage table: dealt in green, taken in red, without the types that are 0 both ways");
  Check(!PanelText(*summary).contains("Repariert"), "no repair line without a repair");

  // A loss: the ship destroyed by its last hit, the shield untouched, and the loot section only a header.
  constexpr std::string_view kLossReport = R"csv(Spielername	Spielerlevel	Ergebnis	Schiffsname	Schiffslevel	Schiffsstärke	Schiff-EP	Offizier Eins	Offizier Zwei	Offizier Drei	Hüllen-TP	Verbleibende Hüllen-TP	Schild-TP	Verbleibende Schild-TP	Standort	Zeitstempel
Spieler	75	NIEDERLAGE	PRAEFORTIS	35	1042625200128	0	Planetfall Kirk	Planetfall Scotty	Planetfall Uhura	1335244765	0	964361509610	964361509610	Xerionis ∃	22.11.2026 17:54:38
Föderations-Polaron-Prototyp-∃	70	SIEG	Föderations-Polaron-Prototyp-∃	70	402140463104	0	Captain der Sternenflotte	--	--	481857608283	123791898300	1,5E+16	1,49995446433703E+16	Xerionis ∃	22.11.2026 17:54:38

Belohnungsname	Anzahl

Flottentyp	Angreifen	Verteidigung	Gesundheit	Schiffsfähigkeit	MANÖVER DES CAPTAINS	Offizier-Eins-Fähigkeit	Offizier-Zwei-Fähigkeit	Offizier-Drei-Fähigkeit	Offiziersangriffsbonus	Schaden pro Runde	Panzerungsdurchbruch	Schilddurchbruch	Treffsicherheit	Kritische Trefferchance	Kritischer Schaden	Offiziersverteidigungsbonus	Panzerung	Schildablenkung	Ausweichen	Offiziersgesundheitsbonus	Schild-TP	Hüllen-TP	Impulsgeschwin­digkeit	Warpreichweite	Warpgeschwindigkeit	Frachtkapazität	Geschützte Fracht	Abbaubonus	Debuff angewendet	Buff angewendet
Spielerflotte 1	163797528080	109239350400	769588330496	Durch den Schleier---Isolytischer Schild	Defensiver Fokus	Zerstörungskraft	Infusionskondensatorverstärkung	Zahlreiche Kondensatoren	56	160859121293	461170695	455286540	1307385805	0,42	6,83	56	1924118880	18167330168	1756420327	56	769154124576	770022538806	235	3803	2712,56	1668520	0	1	NEIN	NEIN
Feindliche Flotte 1	6230625376	849675650	395060166656	Polaronwaffen­technologie ∃---Experimentelles Verteidigungssys.	--	--	--	--	0	3511663264	113290080	113290080	1132900864	0,1	1,5	0	14161261	141612608	14161261	0	308262731776	481857601536	80	2	2	0	0	1	NEIN	NEIN

Runde	Kampf-Ereignis	Typ	Angreifername	Angreiferallianz	Angreiferschiff	Angreifer - Ist Armada?	Zielname	Zielallianz	Zielschiff	Ziel - Ist Armada?	Hyperthermischer Zerfall %	Hyperthermischer Stabilisator %	Kritischer Treffer?	Hüllenschaden	Schildschaden	Abgemilderter Schaden	Abgemilderter Isolytischer Schaden	Abgemilderte Apex-Barriere	Gesamtschaden	Isolytischer Totalschaden	Abgemilderter kritischer Schaden	Polaronschaden	Gemilderter Polaronschaden	Infusionsschaden	Gemilderter Infusionsschaden	Fähigkeitstyp	Fähigkeitswert	Fähigkeitsbezeichnung	Name der Figur mit der Fähigkeit	Ziel besiegt	Ziel zerstört	Waffen laden %
1	1	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	20000	Zerstörungskraft	Planetfall Kirk	--	--	--
1	2	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	0,1	Infusionskondensatorverstärkung	Planetfall Scotty	--	--	--
1	3	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1250	Zahlreiche Kondensatoren	Planetfall Uhura	--	--	--
1	4	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Defensiver Fokus	Planetfall Kirk	--	--	--
1	5	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Umgehauen	B'Elanna Torres	--	--	--
1	6	Offizierfähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	Officer	1	Leitender Moraloffizier	Neelix	--	--	--
1	7	Verbotene-Technologie-Fähigkeit	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	ForbiddenTechAbility	0,25	--	Quanten-Windschattenantrieb	--	--	--
1	8	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	581923520	0	0	0	0	0	0	0	581923520	0	0	0	--	--	--	--	--	--	--
1	9	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	NEIN	594580096	0	0	0	0	0	0	0	594580096	0	0	0	--	--	--	--	--	--	--
1	10	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	189777084416	241341890560	802440085504	11825752047616	0	-1,89807747551396E+16	12067093938176	0	0	0	1,89777081343672E+16	-4058837942272	--	--	--	--	--	--	--
1	11	Angreifen	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	--	--	JA	168288616448	214014738432	711579795456	10486722592768	0	-1,68238011314012E+16	10700737331200	0	0	0	1,68288619079926E+16	4180908179456	--	--	--	--	--	--	--
1	12	Angreifen	Föderations-Polaron-Prototyp-∃	--	Föderations-Polaron-Prototyp-∃	NEIN	Spieler	Allianz	PRAEFORTIS	NEIN	--	--	JA	845575872	0	0	0	0	0	0	0	8455758848	0	0	0	--	--	--	--	--	--	--
1	13	Kämpfer vernichtet	Spieler	Allianz	PRAEFORTIS	NEIN	Föderations-Polaron-Prototyp-∃	--	PRAEFORTIS	NEIN	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--	--)csv";
  const auto loss = Parse(kLossReport);
  Check(loss && loss->sides[0].result == "NIEDERLAGE" && loss->sides[1].shield == 15000000000000000,
        "the loss report");
  Check(loss->rewards.empty() && loss->buffs.empty() && loss->unread.empty(), "a loss without loot");
  Check(loss->rounds == 1 && loss->hits_out == 2 && loss->hits_in == 3 && loss->crits_out == 2 && loss->crits_in == 1,
        "the loss's hits and crits");
  Check(loss->hull_in == 1335244765 && loss->shield_in == 0 && Repaired(*loss) == 0,
        "a destroyed ship took the hull it had, not the last hit's overkill");
  Check(Describe(*loss)
            == "NIEDERLAGE: PRAEFORTIS gegen Föderations-Polaron-Prototyp-∃ (70) in Xerionis ∃, 22.11.2026 17:54:38; "
               "Runden 1; Treffer 2 raus / 3 rein; Crits 2 raus / 1 rein; Schaden raus 358 Mrd Hülle + 455 Mrd Schild; "
               "Schaden rein 1,3 Mrd Hülle + 0 Schild; Hülle danach 0 %; Brücke: Planetfall Kirk, Planetfall Scotty, "
               "Planetfall Uhura; Beute: keine",
        "the loss's summary line");
  const auto loss_panel = PanelText(*loss);
  Check(loss_panel.starts_with("<size=130%><b><color=#FF6B6B>") && loss_panel.ends_with("Beute</color>\nkeine"),
        "the panel shows a loss in red, without loot");
  Check(DescribeDamage(*loss)
            == "Schaden nach Art, raus / rein: Isolytisch 23 Bio / 0; Infusion 358 Mrd / 0; Polaron 0 / 9,6 Mrd; "
               "Abgemildert 1,5 Bio / 0; "
               "Isolytisch abgewehrt 22 Bio / 0; Apex abgemildert 0 / 0; Krit abgewehrt 0 / 0",
        "the damage by type for the log");

  toml::table effective;
  Check(Config::Parse(toml::table{}, effective, false)->ui.battle_summary, "the battle summary is on by default");
  Check(!Config::Parse(toml::parse("[ngo]\nbattle_summary = false"), effective, false)->ui.battle_summary,
        "[ngo] battle_summary turns it off");

  Check(!Parse("") && !Parse("kaputt\n\nnoch kaputter\n"), "anything else is no report");
  Check(Short(950) == "950" && Short(12'400) == "12 Tsd" && Short(2'920'326) == "2,9 Mio"
            && Short(1'240'000'000) == "1,2 Mrd" && Short(97'264'487'838) == "97 Mrd"
            && Short(3'637'222'589'290) == "3,6 Bio",
        "short numbers");
  using detail::Number;
  Check(Number("1,49968274105638E+16") == 14996827410563800 && Number("-3,5E+3") == -3500 && Number("2,5") == 3
            && Number("1.5e2") == 150 && Number("42") == 42,
        "numbers in exponent notation");
  Check(!Number("") && !Number("--") && !Number("1,5E") && !Number("E+16") && !Number("1,2,3") && !Number("9E+30"),
        "anything else is no number");
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
  {
    const auto galaxy = Config::Parse(toml::table{}, effective, false);
    Check(galaxy->graphics.galaxy_zoom == 1.0f && galaxy->graphics.galaxy_zoom_speed == 1.0f,
          "the galaxy zoom is the game's by default");
    const auto wider = Config::Parse(toml::parse("[graphics]\ngalaxy_zoom = 2.5\ngalaxy_zoom_speed = 0.5"), effective,
                                     false);
    Check(wider->graphics.galaxy_zoom == 2.5f && wider->graphics.galaxy_zoom_speed == 0.5f,
          "the galaxy zoom and its speed are read");
    const auto bad = Config::Parse(toml::parse("[graphics]\ngalaxy_zoom = 0.5\ngalaxy_zoom_speed = 20.0"), effective,
                                   false);
    Check(bad->graphics.galaxy_zoom == 1.0f && bad->graphics.galaxy_zoom_speed == 1.0f,
          "a galaxy zoom below 1, or a speed outside 0.25 to 10, keeps the game's");
  }
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
  ChatHighlight();
  NgoSection();
  Announcements();
  BattleSummary();
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
