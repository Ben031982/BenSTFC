#pragma once

// The chat manager, the full-screen and side chat views, and the chat message rows.

#include "ui.h"

#include <il2cpp/il2cpp.h>

#include <cstdint>
#include <string>

enum class ChatChannelCategory : int32_t {
  None            = -1,
  Newbie          = 0,
  Global          = 1,
  Alliance        = 2,
  Private         = 3,
  Private_Message = 4,
  Block           = 5,
  Regional        = 6,
};

enum class ChatViewMode : int32_t {
  Fullscreen = 0,
  Side       = 1,
};

struct ChatManager : MonoSingleton<ChatManager> {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.Chat", "ChatManager">;

  bool isContextInitialised()
  { return Field<"_isContextInitialised", bool>(); }
  bool IsSideChatAllowed()
  { return Get<"IsSideChatAllowed", bool>(); }
  bool IsSideChatOpen()
  { return Get<"IsSideChatOpen", bool>(); }
  void SetViewMode(ChatViewMode value)
  { Set<"ViewMode">(value); }

  bool CanSeeNewbieChat()
  { return Call<"CanSeeNewbieChat", bool>(); }
  bool CanSeeRegionalChat()
  { return Call<"CanSeeRegionalChat", bool>(); }
  void OpenChannel(ChatChannelCategory category = ChatChannelCategory::Alliance)
  { Call<"OpenChannel">(category, il2cpp::NewString("")); }

  void OpenChannel(ChatChannelCategory category, ChatViewMode viewMode)
  {
    SetViewMode(viewMode);
    OpenChannel(category);
  }
};

struct SwipeScroller : il2cpp::Object {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Client.UI", "SwipeScroller">;

  int32_t currentContentIndex()
  { return Field<"_currentContentIndex", int32_t>(); }
  void FocusOnInstantly(int32_t index)
  { Call<"FocusOnInstantly">(index); }
};

struct ChatPreviewController : il2cpp::Object {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.Chat", "ChatPreviewController">;

  SwipeScroller* swipeScroller()
  { return Field<"_swipeScroller", SwipeScroller*>(); }
  void SetFocusedPanel(ChatChannelCategory value)
  { SetField<"_focusedPanel">(value); }
  // Set by the game after its visibility refresh for a section change.
  bool isSectionChangedCompleted()
  { return Field<"_isSectionChangedCompleted", bool>(); }
};

struct ChatMessageListLocalViewController : il2cpp::Object {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.Chat", "ChatMessageListLocalViewController">;

  unity::TMP_InputField* inputField()
  { return Field<"_inputField", unity::TMP_InputField*>(); }
};

struct FullScreenChatViewController : il2cpp::Object {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.Chat", "FullScreenChatViewController">;

  ChatMessageListLocalViewController* messageList()
  { return Field<"_messageList", ChatMessageListLocalViewController*>(); }
  TabBarViewController* categoriesTabBarViewController()
  { return Field<"_categoriesTabBarViewController", TabBarViewController*>(); }
};

struct PlayerTitleWidget : unity::Component {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.PlayerAvatars", "PlayerTitleWidget">;

  unity::GameObject* container()
  { return Field<"_container", unity::GameObject*>(); }
};

// A player's name, avatar and title, on chat rows and other screens.
struct UserProfileWidget : unity::Component {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.PlayerProfile", "UserProfileWidget">;

  PlayerTitleWidget* playerTitleWidget()
  { return Field<"_playerTitleWidget", PlayerTitleWidget*>(); }
};

// One chat message, Digit.PrimeServer.Models.Message.
struct ChatMessage : il2cpp::Object {
  using Class = il2cpp::Class<"Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Models", "Message">;

  bool IsAlliance()
  { return Get<"IsAlliance", bool>(); }
  Il2CppString* SenderId()
  { return Get<"SenderId", Il2CppString*>(); }
  std::string SenderName()
  { return il2cpp::ToUtf8(Get<"SenderName", Il2CppString*>()); }
  std::string RawText()
  { return il2cpp::ToUtf8(Get<"RawText", Il2CppString*>()); }
  // Seconds or milliseconds since 1970.
  double TimeStampDouble()
  { return Field<"TimeStampDouble", double>(); }
};

// ChatMessageWidget.ChatBubble, the texts of one message. A nested class, so its members resolve at run time.
struct ChatBubble : il2cpp::Object {
  unity::TMP_Text* Message()
  { return Field<"Message", unity::TMP_Text*>(); }
};

struct ChatMessageWidget : unity::Component {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.Chat", "ChatMessageWidget">;

  ChatBubble* chatBubble()
  { return Field<"_chatBubble", ChatBubble*>(); }
  // The message it shows.
  ChatMessage* Context()
  { return Get<"Context", ChatMessage*>(); }
};

// One line of the chat preview above the HUD.
struct ChatPreviewMessageWidget : unity::Component {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.Chat", "ChatPreviewMessageWidget">;

  // _text is the label's TextLocalizer; its Target is the label.
  unity::TMP_Text* text()
  {
    auto* localizer = Field<"_text", TextLocalizer*>();
    auto* target    = localizer ? localizer->Target() : nullptr;
    return target ? target->As<unity::TMP_Text>() : nullptr;
  }
  ChatMessage* Context()
  { return Get<"Context", ChatMessage*>(); }
};
