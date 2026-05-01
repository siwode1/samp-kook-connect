#pragma once

// Event constants for backward compatibility with existing code
// These map KOOK events to the expected Discord event names

namespace WebSocket {
    namespace Event {
        // Core events
        static const char* READY = "READY";
        static const char* MESSAGE_CREATE = "MESSAGE_CREATE";
        static const char* MESSAGE_UPDATE = "MESSAGE_UPDATE";
        static const char* MESSAGE_DELETE = "MESSAGE_DELETE";
        
        // Guild events
        static const char* GUILD_CREATE = "GUILD_ADD";
        static const char* GUILD_UPDATE = "GUILD_UPDATE";
        static const char* GUILD_DELETE = "GUILD_REMOVE";
        
        // Channel events
        static const char* CHANNEL_CREATE = "CHANNEL_CREATE";
        static const char* CHANNEL_UPDATE = "CHANNEL_UPDATE";
        static const char* CHANNEL_DELETE = "CHANNEL_DELETE";
        
        // Member events
        static const char* GUILD_MEMBER_ADD = "MEMBER_JOIN";
        static const char* GUILD_MEMBER_UPDATE = "MEMBER_UPDATE";
        static const char* GUILD_MEMBER_REMOVE = "MEMBER_REMOVE";
        
        // Voice events
        static const char* VOICE_STATE_UPDATE = "CHANNEL_VOICE_JOIN";
        
        // Reaction events
        static const char* MESSAGE_REACTION_ADD = "MESSAGE_REACTION_ADD";
        static const char* MESSAGE_REACTION_REMOVE = "MESSAGE_REACTION_REMOVE";
        static const char* MESSAGE_REACTION_REMOVE_ALL = "MESSAGE_REACTION_REMOVE_ALL";
        static const char* MESSAGE_REACTION_REMOVE_EMOJI = "MESSAGE_REACTION_REMOVE_EMOJI";
        
        // Presence events
        static const char* PRESENCE_UPDATE = "PRESENCE_UPDATE";
        
        // Guild member chunk
        static const char* GUILD_MEMBERS_CHUNK = "GUILD_MEMBERS_CHUNK";
    }
}
