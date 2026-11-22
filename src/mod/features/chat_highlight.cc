// [ui] chat_highlight_words: chat messages with one of these words stand out in full-screen and side chat and in the
// chat preview, in [ui] chat_highlight_color. Each message is marked when the game fills its widget, so a recycled
// widget never keeps the marks of an earlier message.

#include "config.h"
#include "features/chat_highlight.h"
#include "features/features.h"

#include "game/chat.h"

#include <il2cpp/hook.h>
#include <il2cpp/il2cpp.h>
#include <unity/core.h>
#include <unity/ui.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace
{
// The labels this feature switched to rich text, switched back when they show a message without a match.
auto& chat_highlight_rich = *new std::unordered_set<unity::TMP_Text*>;

const std::vector<std::string>& ChatHighlightWords()
{
  static auto& source = *new std::string;
  static auto& words  = *new std::vector<std::string>;
  if (const auto& list = Config::Get().ui.chat_highlight_words; list != source) {
    source = list;
    words  = chat_highlight::Words(list);
  }
  return words;
}

void HighlightChatLabel(unity::TMP_Text* label)
{
  if (!unity::Alive(label))
    return;
  const auto& words = ChatHighlightWords();
  const auto& color = Config::Get().ui.chat_highlight_color;
  const auto  text  = label->text();
  // Marked already: the game left the text as it was.
  if (text.starts_with("<mark=" + color + "33>"))
    return;
  const bool turned = chat_highlight_rich.contains(label);
  const bool rich   = label->richText() && !turned;
  const auto marked = words.empty() ? std::nullopt : chat_highlight::Mark(text, words, color, !rich);
  if (!marked) {
    if (turned) {
      label->SetRichText(false);
      chat_highlight_rich.erase(label);
    }
    return;
  }
  if (!rich && !turned) {
    label->SetRichText(true);
    chat_highlight_rich.insert(label);
  }
  label->SetText(*marked);
}

void ChatMessageWidget_SetWidgetData_Hook(auto original, ChatMessageWidget* _this)
{
  original(_this);
  auto* bubble = _this->chatBubble();
  HighlightChatLabel(bubble ? bubble->Message() : nullptr);
}

void ChatPreviewMessageWidget_SetWidgetData_Hook(auto original, ChatPreviewMessageWidget* _this)
{
  original(_this);
  HighlightChatLabel(_this->text());
}

bool InstallChatHighlight()
{
  il2cpp::HookSet hooks("ChatHighlight");
  IL2CPP_STAGE(hooks, ChatMessageWidget::Class::Method<void()>("SetWidgetData"), ChatMessageWidget_SetWidgetData_Hook);
  IL2CPP_STAGE(hooks, ChatPreviewMessageWidget::Class::Method<void()>("SetWidgetData"),
               ChatPreviewMessageWidget_SetWidgetData_Hook);
  return hooks.Commit();
}
} // namespace

const Feature kChatHighlight{"ChatHighlight", &Config::Patches::chatpatches, InstallChatHighlight};
