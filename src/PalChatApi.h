#pragma once
#include <cstdint>
#include <String/StringType.hpp>

// ==== Palworld chat API surface. RE-VERIFY AFTER EVERY PALWORLD UPDATE ====
// Verified against PalworldModdingKit headers and live server, 2026-07-15.
// If chat capture or injection stops working after a game patch, these
// paths and the FPalChatMessage layout are the first things to re-check.

static constexpr auto CAPTURE_HOOK_PATH =
    STR("/Script/Pal.PalPlayerController:EnterChat_Receive");
// Signature: void EnterChat_Receive(const FString& Message, uint8 Category)
// Reliable Server RPC. Executes on the server when a player sends chat.
// IMPORTANT: there is no sender parameter. The sender is the hooked object:
// the APalPlayerController instance. Derive the sender name and player UId
// by walking Controller -> PlayerState.

static constexpr auto BROADCAST_FUNC_PATH =
    STR("/Script/Pal.PalGameStateInGame:BroadcastChatMessage");
// Signature: void BroadcastChatMessage(const FPalChatMessage& ChatMessage)
// NetMulticast Reliable. Calling this on the server's APalGameStateInGame
// instance replicates the chat message to all connected clients.

// FPalChatMessage fields, exact declaration order:
//   1. EPalChatCategory Category      (uint8 underlying)
//   2. FString          Sender
//   3. FGuid            SenderPlayerUId
//   4. FString          Message
//   5. TArray<FGuid>    ReceiverPlayerUIds   (leave empty = broadcast to all)
//   6. FName            MessageId            (leave NAME_None)
//   7. TArray<FString>  MessageArgKeys       (leave empty)
//   8. TArray<FString>  MessageArgValues     (leave empty)

// EPalChatCategory: None=0, Global=1, Guild=2, Say=3, Discord=4
static constexpr uint8_t CHAT_CATEGORY_GLOBAL  = 1;
static constexpr uint8_t CHAT_CATEGORY_DISCORD = 4;

// Categories this mod relays from local players into the shared DB.
// Easy to extend if Say/Guild should ever be included.
static constexpr uint8_t RELAYED_CATEGORIES[] = {CHAT_CATEGORY_GLOBAL};
static constexpr size_t RELAYED_CATEGORIES_COUNT =
    sizeof(RELAYED_CATEGORIES) / sizeof(RELAYED_CATEGORIES[0]);

inline bool IsRelayedCategory(uint8_t category)
{
    for (size_t i = 0; i < RELAYED_CATEGORIES_COUNT; ++i)
    {
        if (RELAYED_CATEGORIES[i] == category)
        {
            return true;
        }
    }
    return false;
}
