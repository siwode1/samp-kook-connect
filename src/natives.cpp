#include "natives.hpp"
#include "Network.hpp"
#include "Channel.hpp"
#include "Message.hpp"
#include "User.hpp"
#include "Role.hpp"
#include "Guild.hpp"
#include "Bot.hpp"
#include "misc.hpp"
#include "types.hpp"
#include "Logger.hpp"
#include "Callback.hpp"
#include "Embed.hpp"
#include "Emoji.hpp"
#include "utils.hpp"
#include <fmt/printf.h>
#include <thread>
#include <chrono>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static std::string Utf8ToACP(std::string const &in)
{
    if (in.empty()) return std::string();
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.c_str(), (int)in.size(), nullptr, 0);
    if (wlen <= 0) return in;
    std::wstring w(wlen, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.c_str(), (int)in.size(), &w[0], wlen) <= 0) return in;
    int alen = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (alen <= 0) return in;
    std::string out(alen, '\0');
    if (WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &out[0], alen, nullptr, nullptr) <= 0) return in;
    return out;
}
#else
static std::string Utf8ToACP(std::string const &in) { return in; }
#endif

#ifdef ERROR
#undef ERROR
#endif
/*
// native native_name(...);
AMX_DECLARE_NATIVE(Native::native_name)
{
	ScopedDebugInfo dbg_info(amx, "native_name", params, "sd");



	cell ret_val = ;
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}
*/

// native DCC_Channel:DCC_FindChannelByName(const channel_name[]);
AMX_DECLARE_NATIVE(Native::DCC_FindChannelByName)
{
	ScopedDebugInfo dbg_info(amx, "DCC_FindChannelByName", params, "s");

	std::string const channel_name = amx_GetCppString(amx, params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannelByName(channel_name);

	cell ret_val = channel ? channel->GetPawnId() : 0;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native DCC_Channel:KCC_FindChannelById(const channel_id[]);
AMX_DECLARE_NATIVE(Native::KCC_FindChannelById)
{
	ScopedDebugInfo dbg_info(amx, "KCC_FindChannelById", params, "s");

	Snowflake_t const channel_id = amx_GetCppString(amx, params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannelById(channel_id);

	cell ret_val = 0;
	if (channel)
	{
		ret_val = channel->GetPawnId();
	}
	else
	{
		// KOOK: If not cached yet, create a placeholder so that ID-based send works immediately
		ChannelId_t created = ChannelManager::Get()->AddChannelById(channel_id, Channel::Type::GUILD_TEXT);
		if (created != INVALID_CHANNEL_ID)
			ret_val = created;
	}

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetChannelId(DCC_Channel:channel, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetChannelId)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetChannelId", params, "drd");

	ChannelId_t channelid = params[1];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	cell ret_val = amx_SetCppString(amx, params[2], channel->GetId(), params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native DCC_GetChannelType(DCC_Channel:channel, &DCC_ChannelType:type);
AMX_DECLARE_NATIVE(Native::DCC_GetChannelType)
{
	ScopedDebugInfo dbg_info(amx, "DCC_GetChannelType", params, "dr");

	ChannelId_t channelid = params[1];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	// Attempt to ensure real type is available: if current type looks like default placeholder (text),
	// trigger async resolve and wait briefly for update.
	Channel::Type initial = channel->GetType();
	if (initial == Channel::Type::GUILD_TEXT)
	{
		// Kick off a resolve (idempotent if already in-flight)
		ChannelManager::Get()->ResolveChannelTypeAsync(channel->GetId());
		// Wait up to ~500ms for type correction
		for (int i = 0; i < 10; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Channel_t const &ch2 = ChannelManager::Get()->FindChannel(channelid);
			if (!ch2) break;
			auto t = ch2->GetType();
			if (t != initial)
			{
				initial = t;
				break;
			}
		}
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(initial);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetChannelGuild(DCC_Channel:channel, &DCC_Guild:guild);
AMX_DECLARE_NATIVE(Native::KCC_GetChannelGuild)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetChannelGuild", params, "dr");

	ChannelId_t channelid = params[1];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

    // Ensure guild association is available: if missing, trigger resolve and wait briefly.
    GuildId_t gid = channel->GetGuildId();
    if (gid == INVALID_GUILD_ID)
    {
        ChannelManager::Get()->ResolveChannelTypeAsync(channel->GetId());
        for (int i = 0; i < 10; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Channel_t const &ch2 = ChannelManager::Get()->FindChannel(channelid);
            if (!ch2) break;
            gid = ch2->GetGuildId();
            if (gid != INVALID_GUILD_ID) break;
        }
    }

    cell *dest = nullptr;
    if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
        return 0;
    }

    *dest = static_cast<cell>(gid);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetChannelName(DCC_Channel:channel, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetChannelName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetChannelName", params, "drd");

	ChannelId_t channelid = params[1];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

    // If name is empty, try resolve via /channel/view and wait briefly
    std::string name = channel->GetName();
    if (name.empty())
    {
        ChannelManager::Get()->ResolveChannelTypeAsync(channel->GetId());
        for (int i = 0; i < 10; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Channel_t const &ch2 = ChannelManager::Get()->FindChannel(channelid);
            if (!ch2) break;
            name = ch2->GetName();
            if (!name.empty()) break;
        }
    }

    if (name.empty())
    {
        Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "channel '{}' has no name (possibly DM or not yet available)", channelid);
        return 0;
    }

    // Convert to local codepage to avoid mojibake in SA:MP console
    std::string out_name = Utf8ToACP(name);
    cell ret_val = amx_SetCppString(amx, params[2], out_name, params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetChannelTopic(DCC_Channel:channel, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetChannelTopic)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetChannelTopic", params, "drd");

	ChannelId_t channelid = params[1];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}


	// If topic is empty, try resolve via /channel/view and wait briefly
	std::string topic = channel->GetTopic();
	if (topic.empty())
	{
		ChannelManager::Get()->ResolveChannelTypeAsync(channel->GetId());
		for (int i = 0; i < 10; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Channel_t const &ch2 = ChannelManager::Get()->FindChannel(channelid);
			if (!ch2) break;
			topic = ch2->GetTopic();
			if (!topic.empty()) break;
		}
	}

	if (topic.empty())
	{
		Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "channel '{}' has no topic (or not yet available)", channelid);
		return 0;
	}

	// Convert to local codepage for SA:MP console
	std::string out_topic = Utf8ToACP(topic);
	cell ret_val = amx_SetCppString(amx, params[2], out_topic, params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetChannelPosition(DCC_Channel:channel, &position);
AMX_DECLARE_NATIVE(Native::KCC_GetChannelPosition)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetChannelPosition", params, "dr");

	ChannelId_t channelid = params[1];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	int pos = channel->GetPosition();
	if (pos < 0)
	{
		// Trigger resolve and wait briefly for 'level' to populate
		ChannelManager::Get()->ResolveChannelTypeAsync(channel->GetId());
		for (int i = 0; i < 10; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Channel_t const &ch2 = ChannelManager::Get()->FindChannel(channelid);
			if (!ch2) break;
			pos = ch2->GetPosition();
			if (pos >= 0) break;
		}
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(pos);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_IsChannelNsfw(DCC_Channel:channel, &bool:is_nsfw);
AMX_DECLARE_NATIVE(Native::DCC_IsChannelNsfw)
{
	ScopedDebugInfo dbg_info(amx, "DCC_IsChannelNsfw", params, "dr");

	ChannelId_t channelid = params[1];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	if (channel->GetType() != Channel::Type::GUILD_TEXT)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"invalid channel type; must be guild text channel");
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(channel->IsNsfw() ? 1 : 0);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_GetChannelParentCategory(DCC_Channel:channel, &DCC_Channel:category);
AMX_DECLARE_NATIVE(Native::DCC_GetChannelParentCategory)
{
	ScopedDebugInfo dbg_info(amx, "DCC_GetChannelParentCategory", params, "dr");

	ChannelId_t channelid = params[1];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	// Try ensure parent is populated: if 0, resolve and short-wait
	ChannelId_t parent = channel->GetParentId();
	if (parent == 0)
	{
		ChannelManager::Get()->ResolveChannelTypeAsync(channel->GetId());
		for (int i = 0; i < 10; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Channel_t const &ch2 = ChannelManager::Get()->FindChannel(channelid);
			if (!ch2) break;
			parent = ch2->GetParentId();
			if (parent != 0) break;
		}
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(parent);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SendChannelMessage(DCC_Channel:channel, const message[], 
//     const callback[] = "", const format[] = "", {Float, _}:...);
AMX_DECLARE_NATIVE(Native::KCC_SendChannelMessage)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SendChannelMessage", params, "ds");

	ChannelId_t channelid = static_cast<ChannelId_t>(params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	auto message = amx_GetCppString(amx, params[2]);
	if (message.length() > 2000)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"message must be shorter than 2000 characters");
		return 0;
	}

	auto
		cb_name = amx_GetCppString(amx, params[3]),
		cb_format = amx_GetCppString(amx, params[4]);

	pawn_cb::Error cb_error;
	auto cb = pawn_cb::Callback::Prepare(
		amx, cb_name.c_str(), cb_format.c_str(), params, 5, cb_error);
	if (cb_error && cb_error.get() != pawn_cb::Error::Type::EMPTY_NAME)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "could not prepare callback");
		return 0;
	}


	channel->SendMessage(std::move(message), std::move(cb));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetChannelName(DCC_Channel:channel, const name[]);
AMX_DECLARE_NATIVE(Native::KCC_SetChannelName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetChannelName", params, "ds");

	ChannelId_t channelid = static_cast<ChannelId_t>(params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	auto name = amx_GetCppString(amx, params[2]);
	if (name.length() < 2 || name.length() > 100)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"name must be between 2 and 100 characters in length");
		return 0;
	}

	channel->SetChannelName(name);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetChannelTopic(DCC_Channel:channel, const topic[]);
AMX_DECLARE_NATIVE(Native::KCC_SetChannelTopic)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetChannelTopic", params, "ds");

	ChannelId_t channelid = static_cast<ChannelId_t>(params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	if (channel->GetType() != Channel::Type::GUILD_TEXT)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"invalid channel type; must be guild text channel");
		return 0;
	}

	auto topic = amx_GetCppString(amx, params[2]);
	if (topic.length() > 1024)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"topic must be between 0 and 1024 characters in length");
		return 0;
	}

	channel->SetChannelTopic(topic);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_SetChannelPosition(DCC_Channel:channel, position);
AMX_DECLARE_NATIVE(Native::DCC_SetChannelPosition)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetChannelPosition", params, "dd");

	ChannelId_t channelid = static_cast<ChannelId_t>(params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	channel->SetChannelPosition(static_cast<int>(params[2]));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_SetChannelNsfw(DCC_Channel:channel, bool:is_nsfw);
AMX_DECLARE_NATIVE(Native::DCC_SetChannelNsfw)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetChannelNsfw", params, "dd");

	ChannelId_t channelid = static_cast<ChannelId_t>(params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	channel->SetChannelNsfw(params[2] != 0);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_SetChannelParentCategory(DCC_Channel:channel, DCC_Channel:parent_category);
AMX_DECLARE_NATIVE(Native::DCC_SetChannelParentCategory)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetChannelParentCategory", params, "dd");

	ChannelId_t channelid = static_cast<ChannelId_t>(params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	ChannelId_t parent_channelid = static_cast<ChannelId_t>(params[2]);
	Channel_t const &parent_channel = ChannelManager::Get()->FindChannel(parent_channelid);
	if (!parent_channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid parent channel id '{}'",
			parent_channelid);
		return 0;
	}

	if (parent_channel->GetType() != Channel::Type::GUILD_CATEGORY)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"invalid channel type; must be guild category channel");
		return 0;
	}

	channel->SetChannelParentCategory(parent_channel);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_DeleteChannel(DCC_Channel:channel);
AMX_DECLARE_NATIVE(Native::KCC_DeleteChannel)
{
	ScopedDebugInfo dbg_info(amx, "KCC_DeleteChannel", params, "d");

	ChannelId_t channelid = static_cast<ChannelId_t>(params[1]);
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	channel->DeleteChannel();

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetMessageId(DCC_Message:message, dest[DCC_ID_SIZE], max_size = DCC_ID_SIZE);
AMX_DECLARE_NATIVE(Native::KCC_GetMessageId)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetMessageId", params, "drd");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	cell ret_val = amx_SetCppString(amx, params[2], msg->GetId(), params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetMessageChannel(DCC_Message:message, &DCC_Channel:channel);
AMX_DECLARE_NATIVE(Native::KCC_GetMessageChannel)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetMessageChannel", params, "dr");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(msg->GetChannel());

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetMessageAuthor(DCC_Message:message, &DCC_User:author);
AMX_DECLARE_NATIVE(Native::KCC_GetMessageAuthor)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetMessageAuthor", params, "dr");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(msg->GetAuthor());

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetMessageContent(DCC_Message:message, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetMessageContent)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetMessageContent", params, "drd");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	cell ret_val = amx_SetCppString(
		amx, params[2], msg->GetContent(), params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native DCC_IsMessageTts(DCC_Message:message, &bool:is_tts);
AMX_DECLARE_NATIVE(Native::DCC_IsMessageTts)
{
	ScopedDebugInfo dbg_info(amx, "DCC_IsMessageTts", params, "dr");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(msg->IsTts() ? 1 : 0);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_IsMessageMentioningEveryone(DCC_Message:message, &bool:mentions_everyone);
AMX_DECLARE_NATIVE(Native::KCC_IsMessageMentioningEveryone)
{
	ScopedDebugInfo dbg_info(amx, "KCC_IsMessageMentioningEveryone", params, "dr");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(msg->MentionsEveryone() ? 1 : 0);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetMessageUserMentionCount(DCC_Message:message, &mentioned_user_count);
AMX_DECLARE_NATIVE(Native::KCC_GetMessageUserMentionCount)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetMessageUserMentionCount", params, "dr");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(msg->GetUserMentions().size());

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetMessageUserMention(DCC_Message:message, offset, &DCC_User:mentioned_user);
AMX_DECLARE_NATIVE(Native::KCC_GetMessageUserMention)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetMessageUserMention", params, "ddr");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	auto const offset = static_cast<unsigned int>(params[2]);
	auto const &mentions = msg->GetUserMentions();
	if (offset >= mentions.size())
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"invalid offset '{}', max size is '{}'",
			offset, mentions.size());
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(mentions.at(offset));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetMessageRoleMentionCount(DCC_Message:message, &mentioned_role_count);
AMX_DECLARE_NATIVE(Native::KCC_GetMessageRoleMentionCount)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetMessageRoleMentionCount", params, "dr");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(msg->GetRoleMentions().size());

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetMessageRoleMention(DCC_Message:message, offset, &DCC_Role:mentioned_role);
AMX_DECLARE_NATIVE(Native::KCC_GetMessageRoleMention)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetMessageRoleMention", params, "ddr");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	auto const offset = static_cast<unsigned int>(params[2]);
	auto const &mentions = msg->GetRoleMentions();
	if (offset >= mentions.size())
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"invalid offset '{}', max size is '{}'",
			offset, mentions.size());
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(mentions.at(offset));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_DeleteMessage(DCC_Message:message);
AMX_DECLARE_NATIVE(Native::KCC_DeleteMessage)
{
	ScopedDebugInfo dbg_info(amx, "KCC_DeleteMessage", params, "d");

	MessageId_t id = params[1];
	Message_t const &msg = MessageManager::Get()->Find(id);
	if (!msg)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", id);
		return 0;
	}

	msg->DeleteMessage();

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_Message:KCC_GetCreatedMessage();
AMX_DECLARE_NATIVE(Native::KCC_GetCreatedMessage)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetCreatedMessage", params);

	auto ret_val = static_cast<cell>(MessageManager::Get()->GetCreatedMessageId());
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{:d}'", ret_val);
	return ret_val;
}

// native DCC_User:KCC_FindUserByName(const user_name[], const user_discriminator[]);
AMX_DECLARE_NATIVE(Native::KCC_FindUserByName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_FindUserByName", params, "ss");

	std::string const
		user_name = amx_GetCppString(amx, params[1]),
		discriminator = amx_GetCppString(amx, params[2]);
	User_t const &user = UserManager::Get()->FindUserByName(user_name, discriminator);

	cell ret_val = user ? user->GetPawnId() : 0;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native DCC_User:KCC_FindUserById(const user_id[]);
AMX_DECLARE_NATIVE(Native::KCC_FindUserById)
{
	ScopedDebugInfo dbg_info(amx, "KCC_FindUserById", params, "s");

	Snowflake_t const user_id = amx_GetCppString(amx, params[1]);
	User_t const &user = UserManager::Get()->FindUserById(user_id);

	cell ret_val = 0;
	if (user)
	{
		ret_val = user->GetPawnId();
	}
	else
	{
		// KOOK: fetch user info and populate cache, then short-wait
		Network::Get()->Http().Get(std::string("/user/view?user_id=") + user_id,
			[user_id](Http::Response r)
			{
				if (r.status / 100 == 2)
				{
					json j = json::parse(r.body, nullptr, false);
					if (j.is_object())
					{
						auto it = j.find("data");
						if (it != j.end() && it->is_object())
						{
							UserManager::Get()->AddUser(*it);
						}
					}
				}
			});
		for (int i = 0; i < 10; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			User_t const &u2 = UserManager::Get()->FindUserById(user_id);
			if (u2)
			{
				ret_val = u2->GetPawnId();
				break;
			}
		}
	}

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetUserId(DCC_User:user, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetUserId)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetUserId", params, "drd");

	UserId_t userid = params[1];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	cell ret_val = amx_SetCppString(amx, params[2], user->GetId(), params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetUserName(DCC_User:user, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetUserName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetUserName", params, "drd");

	UserId_t userid = params[1];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	// If username is empty (rare), try resolve via /user/view and wait briefly
	std::string uname = user->GetUsername();
	if (uname.empty())
	{
		auto uid = user->GetId();
		Network::Get()->Http().Get(std::string("/user/view?user_id=") + uid,
			[uid](Http::Response r)
			{
				if (r.status / 100 == 2)
				{
					json j = json::parse(r.body, nullptr, false);
					if (j.is_object())
					{
						auto it = j.find("data");
						if (it != j.end() && it->is_object())
						{
							UserManager::Get()->AddUser(*it);
						}
					}
				}
			});
		for (int i = 0; i < 10; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			User_t const &u2 = UserManager::Get()->FindUser(userid);
			if (!u2) break;
			uname = u2->GetUsername();
			if (!uname.empty()) break;
		}
	}

	// Convert to local codepage for SA:MP to avoid mojibake
	std::string out_name = Utf8ToACP(uname);
	cell ret_val = amx_SetCppString(amx, params[2], out_name, params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetUserDiscriminator(DCC_User:user, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetUserDiscriminator)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetUserDiscriminator", params, "drd");

	UserId_t userid = params[1];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	cell ret_val = amx_SetCppString(amx, params[2], user->GetDiscriminator(), params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_IsUserBot(DCC_User:user, &bool:is_bot);
AMX_DECLARE_NATIVE(Native::KCC_IsUserBot)
{
	ScopedDebugInfo dbg_info(amx, "KCC_IsUserBot", params, "dr");

	UserId_t userid = params[1];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = user->IsBot() ? 1 : 0;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_IsUserVerified(DCC_User:user, &bool:is_verified);
AMX_DECLARE_NATIVE(Native::KCC_IsUserVerified)
{
	ScopedDebugInfo dbg_info(amx, "KCC_IsUserVerified", params, "dr");

	UserId_t userid = params[1];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = user->IsVerified() ? 1 : 0;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_Role:KCC_FindRoleByName(DCC_Guild:guild, const role_name[]);
AMX_DECLARE_NATIVE(Native::KCC_FindRoleByName)
{
    ScopedDebugInfo dbg_info(amx, "KCC_FindRoleByName", params, "ds");

    GuildId_t guildid = params[1];
    std::string const role_name = amx_GetCppString(amx, params[2]);

    Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
    if (!guild)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
        return INVALID_ROLE_ID;
    }

    cell ret_val = INVALID_ROLE_ID;
    for (auto const &r : guild->GetRoles())
    {
        Role_t const &role = RoleManager::Get()->FindRole(r);
        if (!role)
            continue;

        if (role->GetName() == role_name)
        {
            ret_val = role->GetPawnId();
            break;
        }
    }

    // If not found in cache, try KOOK API to fetch guild roles then retry briefly
    if (ret_val == INVALID_ROLE_ID)
    {
        std::string gid = guild->GetId();
        if (!gid.empty())
        {
            // Retry wrapper for transient failures (e.g., 'end of stream')
            std::string url = std::string("/guild-role/list?guild_id=") + gid;
            std::shared_ptr<std::function<void(int)>> fetch_ptr = std::make_shared<std::function<void(int)>>();
            *fetch_ptr = [url, guildid, fetch_ptr](int retries)
            {
                Network::Get()->Http().Get(url,
                    [guildid, retries, fetch_ptr](Http::Response r)
                    {
                        if (r.status / 100 != 2)
                        {
                            Logger::Get()->Log(samplog_LogLevel::WARNING,
                                "guild-role/list failed: status {} reason {} (retries left: {})", r.status, r.reason, retries);
                            if (retries > 0)
                            {
                                std::thread([fetch_ptr, retries]()
                                {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                    (*fetch_ptr)(retries - 1);
                                }).detach();
                            }
                            return;
                        }
                        json j = json::parse(r.body, nullptr, false);
                        if (j.is_discarded()) return;
                        json items = json::array();
                        if (j.is_array())
                            items = j;
                        else if (j.is_object())
                        {
                            auto it_data = j.find("data");
                            if (it_data != j.end() && it_data->is_object())
                            {
                                auto it_items = it_data->find("items");
                                if (it_items != it_data->end() && it_items->is_array())
                                    items = *it_items;
                            }
                        }
                        if (!items.is_array()) return;
                        auto const &guild = GuildManager::Get()->FindGuild(guildid);
                        for (auto &item : items)
                        {
                            if (!item.is_object()) continue;
                            json role_json;
                            auto it_rid = item.find("role_id");
                            if (it_rid != item.end()) role_json["role_id"] = *it_rid;
                            try { unsigned long long rid = item.value("role_id", 0ULL); if (rid != 0ULL) role_json["id"] = std::to_string(rid); } catch (...) {}
                            role_json["name"] = item.value("name", "");
                            role_json["color"] = item.value("color", 0u);
                            role_json["position"] = item.value("position", 0u);
                            role_json["hoist"] = item.value("hoist", 0u) != 0u;
                            role_json["mentionable"] = item.value("mentionable", 0u) != 0u;
                            try { unsigned int p = item.value("permissions", 0u); role_json["permissions"] = p; } catch (...) { role_json["permissions"] = 0u; }
                            RoleId_t rid_added = RoleManager::Get()->AddRole(role_json);
                            if (guild && rid_added != INVALID_ROLE_ID)
                                guild->AddRole(rid_added);
                        }
                    });
            };
            (*fetch_ptr)(3);

            // Also schedule two proactive retries even if the initial callback never fires
            for (int delay_ms : {200, 600})
            {
                std::thread([url, guildid, delay_ms]()
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    Network::Get()->Http().Get(url,
                        [guildid](Http::Response r)
                        {
                            if (r.status / 100 != 2) return;
                            json j = json::parse(r.body, nullptr, false);
                            if (j.is_discarded()) return;
                            json items = json::array();
                            if (j.is_array()) items = j;
                            else if (j.is_object())
                            {
                                auto it_data = j.find("data");
                                if (it_data != j.end() && it_data->is_object())
                                {
                                    auto it_items = it_data->find("items");
                                    if (it_items != it_data->end() && it_items->is_array())
                                        items = *it_items;
                                }
                            }
                            if (!items.is_array()) return;
                            auto const &guild = GuildManager::Get()->FindGuild(guildid);
                            for (auto &item : items)
                            {
                                if (!item.is_object()) continue;
                                json role_json;
                                auto it_rid = item.find("role_id");
                                if (it_rid != item.end()) role_json["role_id"] = *it_rid;
                                try { unsigned long long rid = item.value("role_id", 0ULL); if (rid != 0ULL) role_json["id"] = std::to_string(rid); } catch (...) {}
                                role_json["name"] = item.value("name", "");
                                role_json["color"] = item.value("color", 0u);
                                role_json["position"] = item.value("position", 0u);
                                role_json["hoist"] = item.value("hoist", 0u) != 0u;
                                role_json["mentionable"] = item.value("mentionable", 0u) != 0u;
                                try { unsigned int p = item.value("permissions", 0u); role_json["permissions"] = p; } catch (...) { role_json["permissions"] = 0u; }
                                RoleId_t rid_added = RoleManager::Get()->AddRole(role_json);
                                if (guild && rid_added != INVALID_ROLE_ID)
                                    guild->AddRole(rid_added);
                            }
                        });
                }).detach();
            }

            // Extended wait loop (~2s) to allow cache to populate, then retry lookup
            for (int i = 0; i < 40 && ret_val == INVALID_ROLE_ID; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                Guild_t const &g2 = GuildManager::Get()->FindGuild(guildid);
                if (!g2) break;
                for (auto const &r2 : g2->GetRoles())
                {
                    Role_t const &role2 = RoleManager::Get()->FindRole(r2);
                    if (!role2) continue;
                    if (role2->GetName() == role_name)
                    {
                        ret_val = role2->GetPawnId();
                        break;
                    }
                }
            }
        }
    }

    Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
    return ret_val;
}

// native DCC_Role:KCC_FindRoleById(const role_id[]);
AMX_DECLARE_NATIVE(Native::KCC_FindRoleById)
{
    ScopedDebugInfo dbg_info(amx, "KCC_FindRoleById", params, "s");

    Snowflake_t const role_id = amx_GetCppString(amx, params[1]);
    Role_t const &role = RoleManager::Get()->FindRoleById(role_id);

    cell ret_val = role ? role->GetPawnId() : 0;

    // If not in cache, try fetching roles for known guilds (KOOK API requires guild_id)
    if (ret_val == 0)
    {
        auto guild_ids = GuildManager::Get()->GetAllGuildIds();
        for (auto gid_pawn : guild_ids)
        {
            Guild_t const &g = GuildManager::Get()->FindGuild(gid_pawn);
            if (!g) continue;
            std::string gid = g->GetId();
            if (gid.empty()) continue;

            bool scheduled = false;
            // Retry wrapper for this fetch as well
            std::string url2 = std::string("/guild-role/list?guild_id=") + gid;
            std::shared_ptr<std::function<void(int)>> fetch_ptr2 = std::make_shared<std::function<void(int)>>();
            *fetch_ptr2 = [url2, gid_pawn, fetch_ptr2](int retries)
            {
                Network::Get()->Http().Get(url2,
                    [gid_pawn, retries, fetch_ptr2](Http::Response r)
                    {
                        if (r.status / 100 != 2)
                        {
                            Logger::Get()->Log(samplog_LogLevel::WARNING,
                                "guild-role/list failed: status {} reason {} (retries left: {})", r.status, r.reason, retries);
                            if (retries > 0)
                            {
                                std::thread([fetch_ptr2, retries]()
                                {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                    (*fetch_ptr2)(retries - 1);
                                }).detach();
                            }
                            return;
                        }
                        json j = json::parse(r.body, nullptr, false);
                        if (j.is_discarded()) return;
                        json items = json::array();
                        if (j.is_array()) items = j;
                        else if (j.is_object())
                        {
                            auto it_data = j.find("data");
                            if (it_data != j.end() && it_data->is_object())
                            {
                                auto it_items = it_data->find("items");
                                if (it_items != it_data->end() && it_items->is_array())
                                    items = *it_items;
                            }
                        }
                        if (!items.is_array()) return;
                        auto const &guild = GuildManager::Get()->FindGuild(gid_pawn);
                        for (auto &item : items)
                        {
                            if (!item.is_object()) continue;
                            json role_json;
                            auto it_rid = item.find("role_id");
                            if (it_rid != item.end()) role_json["role_id"] = *it_rid;
                            try { unsigned long long rid = item.value("role_id", 0ULL); if (rid != 0ULL) role_json["id"] = std::to_string(rid); } catch (...) {}
                            role_json["name"] = item.value("name", "");
                            role_json["color"] = item.value("color", 0u);
                            role_json["position"] = item.value("position", 0u);
                            role_json["hoist"] = item.value("hoist", 0u) != 0u;
                            role_json["mentionable"] = item.value("mentionable", 0u) != 0u;
                            try { unsigned int p = item.value("permissions", 0u); role_json["permissions"] = p; } catch (...) { role_json["permissions"] = 0u; }
                            RoleId_t rid_added = RoleManager::Get()->AddRole(role_json);
                            if (guild && rid_added != INVALID_ROLE_ID)
                                guild->AddRole(rid_added);
                        }
                    });
            };
            (*fetch_ptr2)(3);
            scheduled = true; // mark to avoid unused warning

            // Short wait then check again
            for (int i = 0; i < 10 && ret_val == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                Role_t const &r2 = RoleManager::Get()->FindRoleById(role_id);
                if (r2)
                {
                    ret_val = r2->GetPawnId();
                    break;
                }
            }

            if (ret_val != 0) break;
        }
    }

    Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
    return ret_val;
}

// native KCC_GetRoleId(DCC_Role:role, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetRoleId)
{
    ScopedDebugInfo dbg_info(amx, "KCC_GetRoleId", params, "drd");

	RoleId_t roleid = params[1];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	cell ret_val = amx_SetCppString(amx, params[2], role->GetId(), params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetRoleName(DCC_Role:role, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetRoleName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetRoleName", params, "drd");

	ChannelId_t roleid = params[1];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	// Convert to local codepage for SA:MP to avoid mojibake
	std::string out_name = Utf8ToACP(role->GetName());
	cell ret_val = amx_SetCppString(amx, params[2], out_name, params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetRoleColor(DCC_Role:role, &color);
AMX_DECLARE_NATIVE(Native::KCC_GetRoleColor)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetRoleColor", params, "dr");

	RoleId_t roleid = params[1];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(role->GetColor());

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetRolePermissions(DCC_Role:role, &perm_high, &perm_low);
AMX_DECLARE_NATIVE(Native::KCC_GetRolePermissions)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetRolePermissions", params, "drr");

	RoleId_t roleid = params[1];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference for 'perm_high'");
		return 0;
	}

	*dest = static_cast<cell>((role->GetPermissions() >> 32) & 0xFFFFFFFF);

	dest = nullptr;
	if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference for 'perm_low'");
		return 0;
	}

	*dest = static_cast<cell>(role->GetPermissions() & 0xFFFFFFFF);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_IsRoleHoist(DCC_Role:role, &bool:is_hoist);
AMX_DECLARE_NATIVE(Native::KCC_IsRoleHoist)
{
	ScopedDebugInfo dbg_info(amx, "KCC_IsRoleHoist", params, "dr");

	RoleId_t roleid = params[1];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = role->IsHoist() ? 1 : 0;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetRolePosition(DCC_Role:role, &position);
AMX_DECLARE_NATIVE(Native::KCC_GetRolePosition)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetRolePosition", params, "dr");

	RoleId_t roleid = params[1];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(role->GetPosition());

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_IsRoleMentionable(DCC_Role:role, &bool:is_mentionable);
AMX_DECLARE_NATIVE(Native::KCC_IsRoleMentionable)
{
	ScopedDebugInfo dbg_info(amx, "KCC_IsRoleMentionable", params, "dr");

	RoleId_t roleid = params[1];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = role->IsMentionable() ? 1 : 0;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_Guild:KCC_FindGuildByName(const guild_name[]);
AMX_DECLARE_NATIVE(Native::KCC_FindGuildByName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_FindGuildByName", params, "s");

	std::string const guild_name = amx_GetCppString(amx, params[1]);
	Guild_t const &guild = GuildManager::Get()->FindGuildByName(guild_name);

	cell ret_val = guild ? guild->GetPawnId() : 0;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native DCC_Guild:KCC_FindGuildById(const guild_id[]);
AMX_DECLARE_NATIVE(Native::KCC_FindGuildById)
{
    ScopedDebugInfo dbg_info(amx, "KCC_FindGuildById", params, "s");

    Snowflake_t const guild_id = amx_GetCppString(amx, params[1]);
    Guild_t const &guild = GuildManager::Get()->FindGuildById(guild_id);

    cell ret_val = 0;
    if (guild)
    {
        ret_val = guild->GetPawnId();
    }
    else
    {
        // KOOK: If not cached yet, create a placeholder so that ID-based calls work immediately
        GuildId_t created = GuildManager::Get()->AddGuildById(guild_id);
        if (created != INVALID_GUILD_ID)
        {
            ret_val = created;
            // Proactively fetch full guild info to populate fields like owner_id (user_id)
            Network::Get()->Http().Get(std::string("/guild/view?guild_id=") + guild_id,
                [created](Http::Response r)
                {
                    if (r.status / 100 == 2)
                    {
                        json j = json::parse(r.body, nullptr, false);
                        if (j.is_object())
                        {
                            const json *payload = nullptr;
                            auto it = j.find("data");
                            if (it != j.end() && it->is_object()) payload = &(*it);
                            else payload = &j;

                            Guild_t const &g = GuildManager::Get()->FindGuild(created);
                            if (g && payload)
                            {
                                g->Update(*payload);
                            }
                        }
                    }
                    else
                    {
                        Logger::Get()->Log(samplog_LogLevel::ERROR,
                            "KOOK guild/view in KCC_FindGuildById failed: {} {}", r.status, r.reason);
                    }
                });
        }
    }

    Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
    return ret_val;
}

// native KCC_GetGuildId(DCC_Guild:guild, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildId)
{
    ScopedDebugInfo dbg_info(amx, "KCC_GetGuildId", params, "drd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	cell ret_val = amx_SetCppString(amx, params[2], guild->GetId(), params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetGuildName(DCC_Guild:guild, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetGuildName", params, "drd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	cell ret_val = amx_SetCppString(amx, params[2], guild->GetName(), params[3]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetGuildOwnerId(DCC_Guild:guild, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildOwnerId)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetGuildOwnerId", params, "drd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

    // KOOK: owner is in 'user_id'. If not yet populated, fetch guild/view and wait briefly.
    std::string owner = guild->GetOwnerId();
    if (owner.empty())
    {
        auto gid_str = guild->GetId();
        Network::Get()->Http().Get(std::string("/guild/view?guild_id=") + gid_str,
            [guildid](Http::Response r)
            {
                if (r.status / 100 == 2)
                {
                    json j = json::parse(r.body, nullptr, false);
                    if (j.is_object())
                    {
                        const json *payload = nullptr;
                        auto it = j.find("data");
                        if (it != j.end() && it->is_object()) payload = &(*it);
                        else payload = &j;

                        Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
                        if (g && payload)
                            g->Update(*payload);
                    }
                }
                else
                {
                    Logger::Get()->Log(samplog_LogLevel::ERROR,
                        "KOOK guild/view failed: {} {}", r.status, r.reason);
                }
            });
        // Short wait up to ~2000ms for owner to populate
        for (int i = 0; i < 40; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Guild_t const &g2 = GuildManager::Get()->FindGuild(guildid);
            if (!g2) break;
            owner = g2->GetOwnerId();
            if (!owner.empty()) break;
        }
    }

	cell ret_val = 0;
	if (!owner.empty())
	{
		ret_val = amx_SetCppString(amx, params[2], owner, params[3]) == AMX_ERR_NONE;
	}
	else
	{
		Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "guild '{}' has no owner id available yet", guildid);
	}

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetGuildRole(DCC_Guild:guild, offset, &DCC_Role:role);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildRole)
{
    ScopedDebugInfo dbg_info(amx, "KCC_GetGuildRole", params, "ddr");

    GuildId_t guildid = params[1];
    Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
    if (!guild)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
        return 0;
    }

    auto offset = static_cast<unsigned int>(params[2]);
    auto roles = guild->GetRoles();
    if (offset >= roles.size())
    {
        // KOOK: Try to populate roles via guild-role/list if insufficient
        auto gid_str = guild->GetId();
        Network::Get()->Http().Get(std::string("/guild-role/list?guild_id=") + gid_str,
            [guildid](Http::Response r)
            {
                if (r.status / 100 == 2)
                {
                    json j = json::parse(r.body, nullptr, false);
                    if (j.is_object())
                    {
                        auto it_d = j.find("data");
                        if (it_d != j.end() && it_d->is_object())
                        {
                            auto it_items = it_d->find("items");
                            if (it_items != it_d->end() && it_items->is_array())
                            {
                                Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
                                if (g)
                                {
                                    for (auto &rj : *it_items)
                                    {
                                        RoleId_t rid = RoleManager::Get()->AddRole(rj);
                                        if (rid != INVALID_ROLE_ID)
                                            g->AddRole(rid);
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    Logger::Get()->Log(samplog_LogLevel::ERROR,
                        "KOOK guild-role/list failed: {} {}", r.status, r.reason);
                }
            });

        for (int i = 0; i < 40; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Guild_t const &g2 = GuildManager::Get()->FindGuild(guildid);
            if (!g2) break;
            auto const &r2 = g2->GetRoles();
            if (offset < r2.size())
            {
                roles = r2;
                break;
            }
        }

        if (offset >= roles.size())
        {
            Logger::Get()->LogNative(samplog_LogLevel::ERROR,
                "invalid offset '{}', max size is '{}'",
                offset, roles.size());
            return 0;
        }
    }

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(roles.at(offset));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetGuildRoleCount(DCC_Guild:guild, &count);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildRoleCount)
{
    ScopedDebugInfo dbg_info(amx, "KCC_GetGuildRoleCount", params, "dr");

    GuildId_t guildid = params[1];
    Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
    if (!guild)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
        return 0;
    }

    // KOOK: If roles are not yet populated, fetch guild-role/list and wait briefly.
    size_t role_count = guild->GetRoles().size();
    if (role_count == 0)
    {
        auto gid_str = guild->GetId();
        Network::Get()->Http().Get(std::string("/guild-role/list?guild_id=") + gid_str,
            [guildid](Http::Response r)
            {
                if (r.status / 100 == 2)
                {
                    json j = json::parse(r.body, nullptr, false);
                    if (j.is_object())
                    {
                        auto it_d = j.find("data");
                        if (it_d != j.end() && it_d->is_object())
                        {
                            auto it_items = it_d->find("items");
                            if (it_items != it_d->end() && it_items->is_array())
                            {
                                Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
                                if (g)
                                {
                                    for (auto &rj : *it_items)
                                    {
                                        RoleId_t rid = RoleManager::Get()->AddRole(rj);
                                        if (rid != INVALID_ROLE_ID)
                                            g->AddRole(rid);
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    Logger::Get()->Log(samplog_LogLevel::ERROR,
                        "KOOK guild-role/list failed: {} {}", r.status, r.reason);
                }
            });
        for (int i = 0; i < 40; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Guild_t const &g2 = GuildManager::Get()->FindGuild(guildid);
            if (!g2) break;
            role_count = g2->GetRoles().size();
            if (role_count > 0) break;
        }
    }

    cell *dest = nullptr;
    if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
        return 0;
    }

    *dest = static_cast<cell>(role_count);

    Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
    return 1;
}

// native KCC_GetGuildMember(DCC_Guild:guild, offset, &DCC_User:user);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildMember)
{
    ScopedDebugInfo dbg_info(amx, "KCC_GetGuildMember", params, "ddr");

    GuildId_t guildid = params[1];
    Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
    if (!guild)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
        return 0;
    }

    auto offset = static_cast<unsigned int>(params[2]);
    auto members = guild->GetMembers();
    if (offset >= members.size())
    {
        // KOOK: Populate members via /guild/user-list
        auto gid_str = guild->GetId();
        Network::Get()->Http().Get(std::string("/guild/user-list?guild_id=") + gid_str,
            [guildid](Http::Response r)
            {
                if (r.status / 100 == 2)
                {
                    json j = json::parse(r.body, nullptr, false);
                    if (j.is_object())
                    {
                        auto it_d = j.find("data");
                        if (it_d != j.end() && it_d->is_object())
                        {
                            auto it_items = it_d->find("items");
                            if (it_items != it_d->end() && it_items->is_array())
                            {
                                Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
                                if (g)
                                {
                                    for (auto &it : *it_items)
                                    {
                                        // Build a member JSON compatible with Guild::Member::Update
                                        json m;
                                        // user subobject
                                        json u;
                                        auto it_id = it.find("id");
                                        if (it_id != it.end() && it_id->is_string()) u["id"] = *it_id;
                                        auto it_name = it.find("username");
                                        if (it_name != it.end() && it_name->is_string()) u["username"] = *it_name;
                                        auto it_disc = it.find("identify_num");
                                        if (it_disc != it.end() && it_disc->is_string()) u["identify_num"] = *it_disc;
                                        if (!u.empty()) m["user"] = std::move(u);
                                        // roles as string array
                                        auto it_roles = it.find("roles");
                                        if (it_roles != it.end() && it_roles->is_array())
                                        {
                                            m["roles"] = json::array();
                                            for (auto &rv : *it_roles)
                                            {
                                                if (rv.is_number_unsigned())
                                                {
                                                    m["roles"].push_back(std::to_string(rv.get<unsigned long long>()));
                                                }
                                                else if (rv.is_string())
                                                {
                                                    m["roles"].push_back(rv.get<std::string>());
                                                }
                                            }
                                        }
                                        // nickname
                                        auto it_nick = it.find("nickname");
                                        if (it_nick != it.end() && it_nick->is_string()) m["nick"] = *it_nick;

                                        // Create/ensure user and add member
                                        Snowflake_t uid;
                                        if (utils::TryGetJsonValue(m["user"], uid, "id"))
                                        {
                                            UserId_t uid_pawn = UserManager::Get()->AddUser(m["user"]);
                                            if (uid_pawn != INVALID_USER_ID)
                                            {
                                                Guild::Member mem;
                                                mem.UserId = uid_pawn;
                                                mem.Update(m);
                                                g->AddMember(std::move(mem));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    Logger::Get()->Log(samplog_LogLevel::ERROR,
                        "KOOK guild/user-list failed: {} {}", r.status, r.reason);
                }
            });

        for (int i = 0; i < 40; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Guild_t const &g2 = GuildManager::Get()->FindGuild(guildid);
            if (!g2) break;
            auto const &m2 = g2->GetMembers();
            if (offset < m2.size())
            {
                members = m2;
                break;
            }
        }

        if (offset >= members.size())
        {
            Logger::Get()->LogNative(samplog_LogLevel::ERROR,
                "invalid offset '{}', max size is '{}'",
                offset, members.size());
            return 0;
        }
    }

    cell *dest = nullptr;
    if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
        return 0;
    }

    *dest = static_cast<cell>(members.at(offset).UserId);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetGuildMemberCount(DCC_Guild:guild, &count);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildMemberCount)
{
    ScopedDebugInfo dbg_info(amx, "KCC_GetGuildMemberCount", params, "dr");

    GuildId_t guildid = params[1];
    Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
    if (!guild)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
        return 0;
    }

    size_t count = guild->GetMembers().size();
    if (count == 0)
    {
        // KOOK: Populate members via /guild/user-list
        auto gid_str = guild->GetId();
        Network::Get()->Http().Get(std::string("/guild/user-list?guild_id=") + gid_str,
            [guildid](Http::Response r)
            {
                if (r.status / 100 == 2)
                {
                    json j = json::parse(r.body, nullptr, false);
                    if (j.is_object())
                    {
                        auto it_d = j.find("data");
                        if (it_d != j.end() && it_d->is_object())
                        {
                            auto it_items = it_d->find("items");
                            if (it_items != it_d->end() && it_items->is_array())
                            {
                                Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
                                if (g)
                                {
                                    for (auto &it : *it_items)
                                    {
                                        json m;
                                        json u;
                                        auto it_id = it.find("id");
                                        if (it_id != it.end() && it_id->is_string()) u["id"] = *it_id;
                                        auto it_name = it.find("username");
                                        if (it_name != it.end() && it_name->is_string()) u["username"] = *it_name;
                                        auto it_disc = it.find("identify_num");
                                        if (it_disc != it.end() && it_disc->is_string()) u["identify_num"] = *it_disc;
                                        if (!u.empty()) m["user"] = std::move(u);
                                        auto it_roles = it.find("roles");
                                        if (it_roles != it.end() && it_roles->is_array())
                                        {
                                            m["roles"] = json::array();
                                            for (auto &rv : *it_roles)
                                            {
                                                if (rv.is_number_unsigned())
                                                    m["roles"].push_back(std::to_string(rv.get<unsigned long long>()));
                                                else if (rv.is_string())
                                                    m["roles"].push_back(rv.get<std::string>());
                                            }
                                        }
                                        auto it_nick = it.find("nickname");
                                        if (it_nick != it.end() && it_nick->is_string()) m["nick"] = *it_nick;

                                        Snowflake_t uid;
                                        if (utils::TryGetJsonValue(m["user"], uid, "id"))
                                        {
                                            UserId_t uid_pawn = UserManager::Get()->AddUser(m["user"]);
                                            if (uid_pawn != INVALID_USER_ID)
                                            {
                                                Guild::Member mem;
                                                mem.UserId = uid_pawn;
                                                mem.Update(m);
                                                g->AddMember(std::move(mem));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    Logger::Get()->Log(samplog_LogLevel::ERROR,
                        "KOOK guild/user-list failed: {} {}", r.status, r.reason);
                }
            });

        for (int i = 0; i < 40; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Guild_t const &g2 = GuildManager::Get()->FindGuild(guildid);
            if (!g2) break;
            count = g2->GetMembers().size();
            if (count > 0) break;
        }
    }

    cell *dest = nullptr;
    if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
    {
        Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
        return 0;
    }

    *dest = static_cast<cell>(count);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetGuildMemberVoiceChannel(DCC_Guild:guild, DCC_User:user, &DCC_Channel:channel);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildMemberVoiceChannel)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetGuildMemberVoiceChannel", params, "ddr");

	GuildId_t guild_id = params[1];
	auto const &guild = GuildManager::Get()->FindGuild(guild_id);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guild_id);
		return 0;
	}

	UserId_t user_id = params[2];

	// Try to locate member in cache first
	ChannelId_t vc = INVALID_CHANNEL_ID;
	bool member_found = false;
	for (auto &m : guild->GetMembers())
	{
		if (m.UserId != user_id)
			continue;
		member_found = true;
		vc = m.GetVoiceChannel();
		break;
	}

	// If member not found or voice channel unknown, resolve via KOOK REST
	if (!member_found || vc == INVALID_CHANNEL_ID)
	{
		std::string gid_str = guild->GetId();
		std::string uid_str;
		if (auto const &u = UserManager::Get()->FindUser(user_id))
			uid_str = u->GetId();

		if (!gid_str.empty() && !uid_str.empty())
		{
			Network::Get()->Http().Get(std::string("/channel-user/get-joined-channel?guild_id=") + gid_str + "&user_id=" + uid_str,
				[guild_id, user_id](Http::Response r)
				{
					if (r.status / 100 == 2)
					{
						json j = json::parse(r.body, nullptr, false);
						if (j.is_object())
						{
							auto it_d = j.find("data");
							if (it_d != j.end() && it_d->is_object())
							{
								auto it_items = it_d->find("items");
								if (it_items != it_d->end() && it_items->is_array())
								{
									// If at least one channel returned, take the first
									for (auto &itc : *it_items)
									{
										std::string chid = itc.value("id", std::string());
										if (chid.empty()) continue;
										// Ensure channel exists in cache as VOICE
										ChannelId_t chpid = ChannelManager::Get()->AddChannelById(chid, Channel::Type::GUILD_VOICE);
										// Update member voice channel in guild cache if member exists
										if (auto const &g = GuildManager::Get()->FindGuild(guild_id))
											g->UpdateMemberVoiceChannel(user_id, chpid);
										break;
									}
								}
							}
						}
					}
					else
					{
						Logger::Get()->Log(samplog_LogLevel::ERROR,
							"KOOK channel-user/get-joined-channel failed: {} {}", r.status, r.reason);
					}
				});

			// Short wait up to ~1000ms for cache to populate
			for (int i = 0; i < 20; ++i)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				auto const &g2 = GuildManager::Get()->FindGuild(guild_id);
				if (!g2) break;
				for (auto &m : g2->GetMembers())
				{
					if (m.UserId != user_id) continue;
					vc = m.GetVoiceChannel();
					break;
				}
				if (vc != INVALID_CHANNEL_ID) break;
			}
		}
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(vc == INVALID_CHANNEL_ID ? 0 : vc);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetGuildMemberNickname(DCC_Guild:guild, DCC_User:user, dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildMemberNickname)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetGuildMemberNickname", params, "ddrd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	std::string nick;
	bool member_found = false;
	for (auto &m : guild->GetMembers())
	{
		if (m.UserId != userid)
			continue;

		nick = m.Nickname;
		member_found = true;
		break;
	}

	if (!member_found)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user specified");
		return 0;
	}

	// Convert to local codepage (ACP) for SA:MP console to avoid mojibake
	std::string out_nick = Utf8ToACP(nick);
	cell ret_val = amx_SetCppString(amx, params[3], out_nick, params[4]) == AMX_ERR_NONE;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native KCC_GetGuildMemberRole(DCC_Guild:guild, DCC_User:user, offset, &DCC_Role:role);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildMemberRole)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetGuildMemberRole", params, "dddr");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	std::vector<RoleId_t> const *roles = nullptr;
	for (auto &m : guild->GetMembers())
	{
		if (m.UserId != userid)
			continue;

		roles = &m.Roles;
		break;
	}

	if (roles == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user specified");
		return 0;
	}

	auto offset = static_cast<unsigned int>(params[3]);
	if (offset >= roles->size())
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"invalid offset '{}', max size is '{}'",
			offset, roles->size());
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[4], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(roles->at(offset));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetGuildMemberRoleCount(DCC_Guild:guild, DCC_User:user, &count);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildMemberRoleCount)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetGuildMemberRoleCount", params, "ddr");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	std::vector<RoleId_t> const *roles = nullptr;
	for (auto &m : guild->GetMembers())
	{
		if (m.UserId != userid)
			continue;

		roles = &m.Roles;
		break;
	}

	if (roles == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user specified");
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(roles->size());

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_HasGuildMemberRole(DCC_Guild:guild, DCC_User:user, DCC_Role:role, &bool:has_role);
AMX_DECLARE_NATIVE(Native::KCC_HasGuildMemberRole)
{
	ScopedDebugInfo dbg_info(amx, "KCC_HasGuildMemberRole", params, "dddr");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	std::vector<RoleId_t> const *roles = nullptr;
	for (auto &m : guild->GetMembers())
	{
		if (m.UserId != userid)
			continue;

		roles = &m.Roles;
		break;
	}

	if (roles == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user specified");
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[4], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	RoleId_t roleid = params[3];
	bool res = false;
	for (auto &r : *roles)
	{
		if (r == roleid)
		{
			res = true;
			break;
		}
	}

	*dest = res ? 1 : 0;

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_GetGuildMemberStatus(DCC_Guild:guild, DCC_User:user, &DCC_UserPresenceStatus:status);
AMX_DECLARE_NATIVE(Native::DCC_GetGuildMemberStatus)
{
	ScopedDebugInfo dbg_info(amx, "DCC_GetGuildMemberStatus", params, "ddr");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	auto status = Guild::Member::PresenceStatus::INVALID;
	for (auto &m : guild->GetMembers())
	{
		if (m.UserId != userid)
			continue;

		status = m.Status;
		break;
	}

	if (status == Guild::Member::PresenceStatus::INVALID)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user specified");
		return 0;
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(status);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetGuildChannel(DCC_Guild:guild, offset, &DCC_Channel:channel);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildChannel)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetGuildChannel", params, "ddr");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	auto offset = static_cast<unsigned int>(params[2]);
	auto channels = guild->GetChannels();
	if (offset >= channels.size())
	{
		// KOOK: populate channels via /guild/view if not ready
		std::string gid_str = guild->GetId();
		Network::Get()->Http().Get(std::string("/guild/view?guild_id=") + gid_str,
			[guildid](Http::Response r)
			{
				if (r.status / 100 == 2)
				{
					json j = json::parse(r.body, nullptr, false);
					if (j.is_object())
					{
						const json *payload = nullptr;
						auto it = j.find("data");
						if (it != j.end() && it->is_object()) payload = &(*it); else payload = &j;
						Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
						if (g && payload) { g->Update(*payload); }
					}
				}
				else
				{
					Logger::Get()->Log(samplog_LogLevel::ERROR,
						"KOOK guild/view in KCC_GetGuildChannel failed: {} {}", r.status, r.reason);
				}
			});

		for (int i = 0; i < 40; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Guild_t const &g2 = GuildManager::Get()->FindGuild(guildid);
			if (!g2) break;
			auto const &chs2 = g2->GetChannels();
			if (offset < chs2.size()) { channels = chs2; break; }
		}

		// Fallback: if still insufficient, try /channel/list
		if (offset >= channels.size())
		{
			std::string gid_str2;
			if (auto const &g3 = GuildManager::Get()->FindGuild(guildid)) gid_str2 = g3->GetId();
			if (!gid_str2.empty())
			{
				Network::Get()->Http().Get(std::string("/channel/list?guild_id=") + gid_str2,
					[guildid](Http::Response r)
					{
						if (r.status / 100 == 2)
						{
							json j = json::parse(r.body, nullptr, false);
							if (j.is_object())
							{
								auto it_d = j.find("data");
								if (it_d != j.end() && it_d->is_object())
								{
									auto it_items = it_d->find("items");
									if (it_items != it_d->end() && it_items->is_array())
									{
										Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
										if (g)
										{
											for (auto &cj : *it_items)
											{
												ChannelId_t cid = ChannelManager::Get()->AddChannel(cj, g->GetPawnId());
												if (cid != INVALID_CHANNEL_ID) g->AddChannel(cid);
											}
										}
									}
								}
							}
						}
						else
						{
							Logger::Get()->Log(samplog_LogLevel::ERROR,
								"KOOK channel/list failed: {} {}", r.status, r.reason);
						}
					});

				for (int i = 0; i < 40; ++i)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					Guild_t const &g4 = GuildManager::Get()->FindGuild(guildid);
					if (!g4) break;
					auto const &chs4 = g4->GetChannels();
					if (offset < chs4.size()) { channels = chs4; break; }
				}
			}
		}

		if (offset >= channels.size())
		{
			Logger::Get()->LogNative(samplog_LogLevel::ERROR,
				"invalid offset '{}', max size is '{}'",
				offset, channels.size());
			return 0;
		}
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[3], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(channels.at(offset));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetGuildChannelCount(DCC_Guild:guild, &count);
AMX_DECLARE_NATIVE(Native::KCC_GetGuildChannelCount)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetGuildChannelCount", params, "dr");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	size_t ch_count = guild->GetChannels().size();
	if (ch_count == 0)
	{
		std::string gid_str = guild->GetId();
		Network::Get()->Http().Get(std::string("/guild/view?guild_id=") + gid_str,
			[guildid](Http::Response r)
			{
				if (r.status / 100 == 2)
				{
					json j = json::parse(r.body, nullptr, false);
					if (j.is_object())
					{
						const json *payload = nullptr;
						auto it = j.find("data");
						if (it != j.end() && it->is_object()) payload = &(*it); else payload = &j;
						Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
						if (g && payload) { g->Update(*payload); }
					}
				}
				else
				{
					Logger::Get()->Log(samplog_LogLevel::ERROR,
						"KOOK guild/view in KCC_GetGuildChannelCount failed: {} {}", r.status, r.reason);
				}
			});

		for (int i = 0; i < 40; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Guild_t const &g2 = GuildManager::Get()->FindGuild(guildid);
			if (!g2) break;
			ch_count = g2->GetChannels().size();
			if (ch_count > 0) break;
		}

		// Fallback to /channel/list if still empty
		if (ch_count == 0)
		{
			std::string gid_str2;
			if (auto const &g3 = GuildManager::Get()->FindGuild(guildid)) gid_str2 = g3->GetId();
			if (!gid_str2.empty())
			{
				Network::Get()->Http().Get(std::string("/channel/list?guild_id=") + gid_str2,
					[guildid](Http::Response r)
					{
						if (r.status / 100 == 2)
						{
							json j = json::parse(r.body, nullptr, false);
							if (j.is_object())
							{
								auto it_d = j.find("data");
								if (it_d != j.end() && it_d->is_object())
								{
									auto it_items = it_d->find("items");
									if (it_items != it_d->end() && it_items->is_array())
									{
										Guild_t const &g = GuildManager::Get()->FindGuild(guildid);
										if (g)
										{
											for (auto &cj : *it_items)
											{
												ChannelId_t cid = ChannelManager::Get()->AddChannel(cj, g->GetPawnId());
												if (cid != INVALID_CHANNEL_ID) g->AddChannel(cid);
											}
										}
									}
								}
							}
						}
						else
						{
							Logger::Get()->Log(samplog_LogLevel::ERROR,
								"KOOK channel/list failed: {} {}", r.status, r.reason);
						}
					});

				for (int i = 0; i < 40; ++i)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					Guild_t const &g4 = GuildManager::Get()->FindGuild(guildid);
					if (!g4) break;
					ch_count = g4->GetChannels().size();
					if (ch_count > 0) break;
				}
			}
		}
	}

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	*dest = static_cast<cell>(ch_count);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetAllGuilds(DCC_Guild:dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_GetAllGuilds)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetAllGuilds", params, "rd");

	cell *dest = nullptr;
	if (amx_GetAddr(amx, params[1], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid reference");
		return 0;
	}

	auto guild_ids = GuildManager::Get()->GetAllGuildIds();

	cell const
		max_dest_size = params[2],
		guilds_count = static_cast<cell>(guild_ids.size());

	if (guilds_count > max_dest_size)
	{
		Logger::Get()->LogNative(samplog_LogLevel::WARNING,
			"destination array is too small (should be at least '{}' cells)",
			guilds_count);
	}

	cell const count = std::min(max_dest_size, guilds_count);
	for (cell i = 0; i != count; ++i)
		dest[i] = guild_ids.at(i);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", count);
	return count;
}

// native DCC_SetGuildName(DCC_Guild:guild, const name[]);
AMX_DECLARE_NATIVE(Native::DCC_SetGuildName)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetGuildName", params, "ds");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	auto name = amx_GetCppString(amx, params[2]);
	if (name.length() < 2 || name.length() > 100)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"name must be between 2 and 100 characters in length");
		return 0;
	}

	guild->SetGuildName(name);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_CreateGuildChannel(DCC_Guild:guild, const name[], DCC_ChannelType:type,
//     const callback[], const format[], {Float, _}:...);
AMX_DECLARE_NATIVE(Native::KCC_CreateGuildChannel)
{
	ScopedDebugInfo dbg_info(amx, "KCC_CreateGuildChannel", params, "dsdss");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	auto name = amx_GetCppString(amx, params[2]);
	if (name.length() < 2 || name.length() > 100)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"name must be between 2 and 100 characters in length");
		return 0;
	}

	auto type = static_cast<Channel::Type>(params[3]);
	if (type != Channel::Type::GUILD_CATEGORY
		&& type != Channel::Type::GUILD_TEXT
		&& type != Channel::Type::GUILD_VOICE)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel type");
		return 0;
	}

	auto
		cb_name = amx_GetCppString(amx, params[4]),
		cb_format = amx_GetCppString(amx, params[5]);

	pawn_cb::Error cb_error;
	auto cb = pawn_cb::Callback::Prepare(
		amx, cb_name.c_str(), cb_format.c_str(), params, 6, cb_error);
	if (cb_error && cb_error.get() != pawn_cb::Error::Type::EMPTY_NAME)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "could not prepare callback");
		return 0;
	}

	ChannelManager::Get()->CreateGuildChannel(guild, name, type, std::move(cb));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_Channel:KCC_GetCreatedGuildChannel();
AMX_DECLARE_NATIVE(Native::KCC_GetCreatedGuildChannel)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetCreatedGuildChannel", params);
	return ChannelManager::Get()->GetCreatedGuildChannelId();
}

// native KCC_SetGuildMemberNickname(DCC_Guild:guild, DCC_User:user, const nickname[]);
AMX_DECLARE_NATIVE(Native::KCC_SetGuildMemberNickname)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetGuildMemberNickname", params, "dds");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	guild->SetMemberNickname(user, amx_GetCppString(amx, params[3]));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_SetGuildMemberVoiceChannel(DCC_Guild:guild, DCC_User:user, DCC_Channel:channel);
AMX_DECLARE_NATIVE(Native::DCC_SetGuildMemberVoiceChannel)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetGuildMemberVoiceChannel", params, "ddd");

	GuildId_t guild_id = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guild_id);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guild_id);
		return 0;
	}

	UserId_t user_id = params[2];
	User_t const &user = UserManager::Get()->FindUser(user_id);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", user_id);
		return 0;
	}

	ChannelId_t channel_id = params[3];
	Channel_t const &channel = ChannelManager::Get()->FindChannel(channel_id);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channel_id);
		return 0;
	}
	guild->SetMemberVoiceChannel(user, channel->GetId());

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_AddGuildMemberRole(DCC_Guild:guild, DCC_User:user, DCC_Role:role);
AMX_DECLARE_NATIVE(Native::KCC_AddGuildMemberRole)
{
	ScopedDebugInfo dbg_info(amx, "KCC_AddGuildMemberRole", params, "ddd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	RoleId_t roleid = params[3];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	guild->AddMemberRole(user, role);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_RemoveGuildMemberRole(DCC_Guild:guild, DCC_User:user, DCC_Role:role);
AMX_DECLARE_NATIVE(Native::KCC_RemoveGuildMemberRole)
{
	ScopedDebugInfo dbg_info(amx, "KCC_RemoveGuildMemberRole", params, "ddd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	RoleId_t roleid = params[3];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	guild->RemoveMemberRole(user, role);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_RemoveGuildMember(DCC_Guild:guild, DCC_User:user);
AMX_DECLARE_NATIVE(Native::KCC_RemoveGuildMember)
{
	ScopedDebugInfo dbg_info(amx, "KCC_RemoveGuildMember", params, "dd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	guild->KickMember(user);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_CreateGuildMemberBan(DCC_Guild:guild, DCC_User:user, const reason[] = "");
AMX_DECLARE_NATIVE(Native::KCC_CreateGuildMemberBan)
{
	ScopedDebugInfo dbg_info(amx, "KCC_CreateGuildMemberBan", params, "dds");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	guild->CreateMemberBan(user, amx_GetCppString(amx, params[3]));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_RemoveGuildMemberBan(DCC_Guild:guild, DCC_User:user);
AMX_DECLARE_NATIVE(Native::KCC_RemoveGuildMemberBan)
{
	ScopedDebugInfo dbg_info(amx, "KCC_RemoveGuildMemberBan", params, "dd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	UserId_t userid = params[2];
	User_t const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	guild->RemoveMemberBan(user);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_SetGuildRolePosition(DCC_Guild:guild, DCC_Role:role, position);
AMX_DECLARE_NATIVE(Native::DCC_SetGuildRolePosition)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetGuildRolePosition", params, "ddd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	RoleId_t roleid = params[2];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	guild->SetRolePosition(role, static_cast<int>(params[3]));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetGuildRoleName(DCC_Guild:guild, DCC_Role:role, const name[]);
AMX_DECLARE_NATIVE(Native::KCC_SetGuildRoleName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetGuildRoleName", params, "dds");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	RoleId_t roleid = params[2];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	guild->SetRoleName(role, amx_GetCppString(amx, params[3]));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetGuildRolePermissions(DCC_Guild:guild, DCC_Role:role, perm_high, perm_low);
AMX_DECLARE_NATIVE(Native::KCC_SetGuildRolePermissions)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetGuildRolePermissions", params, "dddd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	RoleId_t roleid = params[2];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	unsigned long long
		perm_high = static_cast<unsigned long long>(params[3]) << 32,
		perm_low = static_cast<unsigned long long>(params[4]),
		permissions = perm_high & perm_low;

	guild->SetRolePermissions(role, permissions);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetGuildRoleColor(DCC_Guild:guild, DCC_Role:role, color);
AMX_DECLARE_NATIVE(Native::KCC_SetGuildRoleColor)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetGuildRoleColor", params, "ddd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	RoleId_t roleid = params[2];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	guild->SetRoleColor(role, static_cast<unsigned int>(params[3]));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetGuildRoleHoist(DCC_Guild:guild, DCC_Role:role, bool:hoist);
AMX_DECLARE_NATIVE(Native::KCC_SetGuildRoleHoist)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetGuildRoleColor", params, "ddd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	RoleId_t roleid = params[2];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	guild->SetRoleHoist(role, params[3] != 0);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetGuildRoleMentionable(DCC_Guild:guild, DCC_Role:role, bool:mentionable);
AMX_DECLARE_NATIVE(Native::KCC_SetGuildRoleMentionable)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetGuildRoleColor", params, "ddd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	RoleId_t roleid = params[2];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	guild->SetRoleMentionable(role, params[3] != 0);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_CreateGuildRole(DCC_Guild:guild, const name[],
//     const callback[], const format[], {Float, _}:...);
AMX_DECLARE_NATIVE(Native::KCC_CreateGuildRole)
{
	ScopedDebugInfo dbg_info(amx, "KCC_CreateGuildRole", params, "dsss");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	auto name = amx_GetCppString(amx, params[2]);
	if (name.length() < 2 || name.length() > 100)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"name must be between 2 and 100 characters in length");
		return 0;
	}

	auto
		cb_name = amx_GetCppString(amx, params[3]),
		cb_format = amx_GetCppString(amx, params[4]);

	pawn_cb::Error cb_error;
	auto cb = pawn_cb::Callback::Prepare(
		amx, cb_name.c_str(), cb_format.c_str(), params, 5, cb_error);
	if (cb_error && cb_error.get() != pawn_cb::Error::Type::EMPTY_NAME)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "could not prepare callback");
		return 0;
	}

	GuildManager::Get()->CreateGuildRole(guild, name, std::move(cb));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_Role:KCC_GetCreatedGuildRole();
AMX_DECLARE_NATIVE(Native::KCC_GetCreatedGuildRole)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetCreatedGuildRole", params);
	return GuildManager::Get()->GetCreatedRoleChannelId();
}

// native KCC_DeleteGuildRole(DCC_Guild:guild, DCC_Role:role);
AMX_DECLARE_NATIVE(Native::KCC_DeleteGuildRole)
{
	ScopedDebugInfo dbg_info(amx, "KCC_DeleteGuildRole", params, "dd");

	GuildId_t guildid = params[1];
	Guild_t const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	RoleId_t roleid = params[2];
	Role_t const &role = RoleManager::Get()->FindRole(roleid);
	if (!role)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid role id '{}'", roleid);
		return 0;
	}

	guild->DeleteRole(role);

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_BotPresenceStatus:DCC_GetBotPresenceStatus();
AMX_DECLARE_NATIVE(Native::DCC_GetBotPresenceStatus)
{
	ScopedDebugInfo dbg_info(amx, "DCC_GetBotPresenceStatus", params);
	return static_cast<cell>(ThisBot::Get()->GetPresenceStatus());
}

// native DCC_TriggerBotTypingIndicator(DCC_Channel:channel);
AMX_DECLARE_NATIVE(Native::DCC_TriggerBotTypingIndicator)
{
	ScopedDebugInfo dbg_info(amx, "DCC_TriggerBotTypingIndicator", params, "d");

	auto channelid = static_cast<ChannelId_t>(params[1]);
	auto const &channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	ThisBot::Get()->TriggerTypingIndicator(channel);
	return 1;
}

// native DCC_SetBotNickname(DCC_Guild:guild, const nickname[]);
AMX_DECLARE_NATIVE(Native::DCC_SetBotNickname)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetBotNickname", params, "ds");

	auto guildid = static_cast<GuildId_t>(params[1]);
	auto const &guild = GuildManager::Get()->FindGuild(guildid);
	if (!guild)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid guild id '{}'", guildid);
		return 0;
	}

	auto nickname = amx_GetCppString(amx, params[2]);
	if (!nickname.empty()) // if nickname is empty it gets resetted/removed
	{
		// see https://discordapp.com/developers/docs/resources/user#usernames-and-nicknames
		if (nickname.length() < 2 || nickname.length() > 32
			|| nickname == "discordtag" || nickname == "everyone" || nickname == "here"
			|| nickname.front() == '@' || nickname.front() == '#' || nickname.front() == ':'
			|| nickname.find("```") == 0)
		{
			Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid nickname '{:s}'", nickname);
			return 0;
		}
	}

	ThisBot::Get()->SetNickname(guild, nickname);
	return 1;
}

// native KCC_CreatePrivateChannel(DCC_User:user, const callback[] = "",
//		const format[] = "", {Float, _}:...);
AMX_DECLARE_NATIVE(Native::KCC_CreatePrivateChannel)
{
	ScopedDebugInfo dbg_info(amx, "KCC_CreatePrivateChannel", params, "dss");

	auto userid = static_cast<UserId_t>(params[1]);
	auto const &user = UserManager::Get()->FindUser(userid);
	if (!user)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid user id '{}'", userid);
		return 0;
	}

	auto
		cb_name = amx_GetCppString(amx, params[2]),
		cb_format = amx_GetCppString(amx, params[3]);

	pawn_cb::Error cb_error;
	auto cb = pawn_cb::Callback::Prepare(
		amx, cb_name.c_str(), cb_format.c_str(), params, 4, cb_error);
	if (cb_error && cb_error.get() != pawn_cb::Error::Type::EMPTY_NAME)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "could not prepare callback");
		return 0;
	}

	ThisBot::Get()->CreatePrivateChannel(user, std::move(cb));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_Channel:KCC_GetCreatedPrivateChannel();
AMX_DECLARE_NATIVE(Native::KCC_GetCreatedPrivateChannel)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetCreatedPrivateChannel", params);
	return ThisBot::Get()->GetCreatedPrivateChannelId();
}

// native DCC_SetBotPresenceStatus(DCC_BotPresenceStatus:status);
AMX_DECLARE_NATIVE(Native::DCC_SetBotPresenceStatus)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetBotPresenceStatus", params, "d");

	bool ret_val = ThisBot::Get()->SetPresenceStatus(
		static_cast<ThisBot::PresenceStatus>(params[1]));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native DCC_SetBotActivity(const name[]);
AMX_DECLARE_NATIVE(Native::DCC_SetBotActivity)
{
	ScopedDebugInfo dbg_info(amx, "DCC_SetBotActivity", params, "s");

	ThisBot::Get()->SetActivity(amx_GetCppString(amx, params[1]));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_EscapeMarkdown(const src[], dest[], max_size = sizeof dest);
AMX_DECLARE_NATIVE(Native::KCC_EscapeMarkdown)
{
	ScopedDebugInfo dbg_info(amx, "KCC_EscapeMarkdown", params, "drs");

	auto const src = amx_GetCppString(amx, params[1]);
	std::string dest;
	dest.reserve(src.length());

	static const std::unordered_set<char> markdown_chars{
		'_', '*', '~', '`', '|'
	};

	bool skip = false;
	for (auto it = src.begin(); it != src.end(); ++it)
	{
		auto ch = *it;

		if (ch == '\\')
		{
			skip = true; // skip next escape
		}
		else if (markdown_chars.find(ch) != markdown_chars.end())
		{
			if (skip)
				skip = false;
			else
				dest.push_back('\\');
		}

		dest.push_back(ch);
	}

	if (amx_SetCppString(amx, params[2], dest, params[3]) != AMX_ERR_NONE)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "couldn't set destination string");
		return -1;
	}

	auto ret_val = static_cast<cell>(dest.length());
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", ret_val);
	return ret_val;
}

// native DCC_CreateEmbedMessage(const title[] = "", const description[] = "", const url[] = "", const timestamp[] = "", int color = 0, const footer_text[] = "", const footer_icon_url[] = "", 
//		const thumbnail_url[] = "", const image_url[] = "");
AMX_DECLARE_NATIVE(Native::KCC_CreateEmbed)
{
	ScopedDebugInfo dbg_info(amx, "KCC_CreateEmbed", params, "ssssdssss");
	auto const title = amx_GetCppString(amx, params[1]);
	auto const description = amx_GetCppString(amx, params[2]);
	auto const url = amx_GetCppString(amx, params[3]);
	auto const timestamp = amx_GetCppString(amx, params[4]);
	auto const color = static_cast<int>(params[5]);
	auto const footer_text = amx_GetCppString(amx, params[6]);
	auto const footer_icon_url = amx_GetCppString(amx, params[7]);
	auto const thumbnail_url = amx_GetCppString(amx, params[8]);
	auto const image_url = amx_GetCppString(amx, params[9]);

	EmbedId_t id = EmbedManager::Get()->AddEmbed(title, description, url, timestamp, color, footer_text, footer_icon_url, thumbnail_url, image_url);
	if (!id)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "failed to create embed");
		return 0;
	}
	return id;
}

// native KCC_DeleteEmbed(DCC_Embed:embed);
AMX_DECLARE_NATIVE(Native::KCC_DeleteEmbed)
{
	ScopedDebugInfo dbg_info(amx, "KCC_DeleteEmbed", params, "d");
	EmbedId_t embedid = static_cast<EmbedId_t>(params[1]);
	Embed_t const& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	EmbedManager::Get()->DeleteEmbed(embedid);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SendChannelEmbedMessage(DCC_Channel:channel, DCC_Embed:embed, const message[] = "", 
//     const callback[] = "", const format[] = "", {Float, _}:...);
AMX_DECLARE_NATIVE(Native::KCC_SendChannelEmbedMessage)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SendChannelEmbedMessage", params, "ddsss");

	ChannelId_t channelid = static_cast<ChannelId_t>(params[1]);
	Channel_t const& channel = ChannelManager::Get()->FindChannel(channelid);
	if (!channel)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel id '{}'", channelid);
		return 0;
	}

	EmbedId_t embedid = static_cast<EmbedId_t>(params[2]);
	Embed_t const& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto message = amx_GetCppString(amx, params[3]);
	if (message.length() > 5000)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"message must be shorter than 5000 characters (KOOK kmarkdown limit)");
		return 0;
	}

	auto
		cb_name = amx_GetCppString(amx, params[4]),
		cb_format = amx_GetCppString(amx, params[5]);

	pawn_cb::Error cb_error;
	auto cb = pawn_cb::Callback::Prepare(
		amx, cb_name.c_str(), cb_format.c_str(), params, 6, cb_error);
	if (cb_error && cb_error.get() != pawn_cb::Error::Type::EMPTY_NAME)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "could not prepare callback");
		return 0;
	}

	channel->SendEmbeddedMessage(embed, std::move(message), std::move(cb));
	EmbedManager::Get()->DeleteEmbed(embedid);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_AddEmbedField(DCC_Embed:embed, const name[], const value[], bool:embed = false);
AMX_DECLARE_NATIVE(Native::KCC_AddEmbedField)
{
	ScopedDebugInfo dbg_info(amx, "DCC_AddEmbeddedField", params, "dssd");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const name = amx_GetCppString(amx, params[2]);
	auto const value = amx_GetCppString(amx, params[3]);
	auto const inline_ = static_cast<bool>(params[4]);
	embed->AddField(name, value, inline_);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetEmbedTitle(DCC_Embed:embed, const title[]);
AMX_DECLARE_NATIVE(Native::KCC_SetEmbedTitle)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetEmbedTitle", params, "ds");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const title = amx_GetCppString(amx, params[2]);
	embed->SetTitle(title);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetEmbedDescription(DCC_Embed:embed, const description[]);
AMX_DECLARE_NATIVE(Native::KCC_SetEmbedDescription)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetEmbedDescription", params, "ds");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const description = amx_GetCppString(amx, params[2]);
	embed->SetDescription(description);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetEmbedUrl(DCC_Embed:embed, const url[]);
AMX_DECLARE_NATIVE(Native::KCC_SetEmbedUrl)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetEmbedUrl", params, "ds");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const url = amx_GetCppString(amx, params[2]);
	embed->SetUrl(url);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetEmbedTimestamp(DCC_Embed:embed, const timestamp[]);
AMX_DECLARE_NATIVE(Native::KCC_SetEmbedTimestamp)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetEmbedTimestamp", params, "ds");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const timestamp = amx_GetCppString(amx, params[2]);
	embed->SetTimestamp(timestamp);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetEmbedColor(DCC_Embed:embed, color);
AMX_DECLARE_NATIVE(Native::KCC_SetEmbedColor)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetEmbedColor", params, "dd");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const color = static_cast<int>(params[2]);
	embed->SetColor(color);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetEmbedFooter(DCC_Embed:embed, const footer_text[], const footer_icon_url[] = "");
AMX_DECLARE_NATIVE(Native::KCC_SetEmbedFooter)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetEmbedFooter", params, "dss");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const text = amx_GetCppString(amx, params[2]);
	auto const icon_url = amx_GetCppString(amx, params[3]);
	embed->SetFooterText(text);
	embed->SetFooterIconUrl(icon_url);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetEmbedThumbnail(DCC_Embed:embed, const thumbnail_url[]);
AMX_DECLARE_NATIVE(Native::KCC_SetEmbedThumbnail)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetEmbedThumbnail", params, "ds");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const url = amx_GetCppString(amx, params[2]);
	embed->SetThumbnailUrl(url);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_SetEmbedImage(DCC_Embed:embed, const image_url[]);
AMX_DECLARE_NATIVE(Native::KCC_SetEmbedImage)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetEmbedImage", params, "ds");
	auto embedid = static_cast<EmbedId_t>(params[1]);
	auto& embed = EmbedManager::Get()->FindEmbed(embedid);
	if (!embed)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid embed id '{}'", embedid);
		return 0;
	}

	auto const url = amx_GetCppString(amx, params[2]);
	embed->SetImageUrl(url);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_DeleteInternalMessage(DCC_Message:message);
AMX_DECLARE_NATIVE(Native::KCC_DeleteInternalMessage)
{
	ScopedDebugInfo dbg_info(amx, "KCC_DeleteInternalMessage", params, "d");
	auto messageid = static_cast<MessageId_t>(params[1]);
	auto& message = MessageManager::Get()->Find(messageid);
	if (!message)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", messageid);
		return 0;
	}
	MessageManager::Get()->Delete(messageid);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native DCC_Emoji:KCC_CreateEmoji(const name[DCC_EMOJI_NAME_SIZE], const snowflake[64] = "");
AMX_DECLARE_NATIVE(Native::KCC_CreateEmoji)
{
	ScopedDebugInfo dbg_info(amx, "KCC_CreateEmoji", params, "ss");
	const auto& name = amx_GetCppString(amx, params[1]);
	const auto& snowflake = amx_GetCppString(amx, params[2]);

	EmojiId_t id = EmojiManager::Get()->AddEmoji(snowflake, name);
	if (!id)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "failed to create emoji");
		return 0;
	}
	return id;
}

// native KCC_DeleteEmoji(DCC_Emoji:emoji);
AMX_DECLARE_NATIVE(Native::KCC_DeleteEmoji)
{
	ScopedDebugInfo dbg_info(amx, "KCC_DeleteEmoji", params, "d");
	const auto& emojid = static_cast<EmojiId_t>(params[1]);
	const auto& emoji = EmojiManager::Get()->FindEmoji(emojid);
	if (!emoji)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid emoji id '{}'", emojid);
		return 0;
	}

	EmojiManager::Get()->DeleteEmoji(emojid);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_GetEmojiName(DCC_Emoji:emoji, dest[DCC_EMOJI_NAME_SIZE], maxlen = DCC_EMOJI_NAME_SIZE);
AMX_DECLARE_NATIVE(Native::KCC_GetEmojiName)
{
	ScopedDebugInfo dbg_info(amx, "KCC_GetEmojiName", params, "dsd");
	std::string dest;

	auto emojid = static_cast<EmojiId_t>(params[1]);
	auto& emoji = EmojiManager::Get()->FindEmoji(emojid);
	if (!emoji)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid emoji id '{}'", emojid);
		return 0;
	}

	dest = emoji->GetName();
	if (amx_SetCppString(amx, params[2], dest, params[3]) != AMX_ERR_NONE)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "couldn't set destination string");
		return -1;
	}

	return dest.length();
}

// native KCC_CreateReaction(DCC_Message:message, DCC_Emoji:reaction_emoji);
AMX_DECLARE_NATIVE(Native::KCC_CreateReaction)
{
	ScopedDebugInfo dbg_info(amx, "KCC_CreateReaction", params, "dd");
	const auto& messageid = static_cast<MessageId_t>(params[1]);
	const auto& message = MessageManager::Get()->Find(messageid);
	if (!message)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", messageid);
		return 0;
	}

	const auto& emojid = static_cast<EmojiId_t>(params[2]);
	const auto& emoji = EmojiManager::Get()->FindEmoji(emojid);
	if (!emoji)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid emoji id '{}'", emojid);
		return 0;
	}

	message->AddReaction(emoji);
	EmojiManager::Get()->DeleteEmoji(emojid);
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '1'");
	return 1;
}

// native KCC_DeleteMessageReaction(DCC_Message:message, DCC_Emoji:emoji = 0);
AMX_DECLARE_NATIVE(Native::KCC_DeleteMessageReaction)
{
	ScopedDebugInfo dbg_info(amx, "DCC_DeleteMessageReactions", params, "dd");
	const auto& messageid = static_cast<MessageId_t>(params[1]);
	const auto& message = MessageManager::Get()->Find(messageid);
	if (!message)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", messageid);
		return 0;
	}

	const auto& emojid = static_cast<EmojiId_t>(params[2]);
	cell retval = static_cast<cell>(message->DeleteReaction(emojid));
	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", retval);
	return retval;
}

// native KCC_EditMessage(DCC_Message:message, const message[], DCC_Embed:embed = 0);
AMX_DECLARE_NATIVE(Native::KCC_EditMessage)
{
	ScopedDebugInfo dbg_info(amx, "KCC_EditMessage", params, "dsd");
	const auto& messageid = static_cast<MessageId_t>(params[1]);
	const auto& message = MessageManager::Get()->Find(messageid);
	if (!message)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", messageid);
		return 0;
	}

	const auto content = amx_GetCppString(amx, params[2]);
	if (content.length() > 5000)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR,
			"message must be shorter than 5000 characters (KOOK kmarkdown limit)");
		return 0;
	}

	const auto& embedid = static_cast<EmbedId_t>(params[3]);
	cell retval = static_cast<cell>(message->EditMessage(content, embedid));

	Logger::Get()->LogNative(samplog_LogLevel::DEBUG, "return value: '{}'", retval);
	return retval;
}

// native DCC_SetMessage(DCC_Message:message, bool persistent);
AMX_DECLARE_NATIVE(Native::KCC_SetMessagePersistent)
{
	ScopedDebugInfo dbg_info(amx, "KCC_SetMessagePersistent", params, "dd");
	const auto& messageid = static_cast<MessageId_t>(params[1]);
	const auto& message = MessageManager::Get()->Find(messageid);
	bool persistent = static_cast<bool>(params[2]);
	if (!message)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid message id '{}'", messageid);
		return 0;
	}

	message->SetPresistent(persistent);
	Logger::Get()->LogNative(samplog_LogLevel::INFO, "return value: '1'");
	return 1;
}

// native KCC_CacheChannelMessage(const channel_id[DCC_ID_SIZE], const message_id[DCC_ID_SIZE], const callback[] = "", const format[] = "", {Float, _}:...);
AMX_DECLARE_NATIVE(Native::KCC_CacheChannelMessage)
{
	ScopedDebugInfo dbg_info(amx, "KCC_CacheChannelMessage", params, "ssss");
	const auto& channel_snowflake = amx_GetCppString(amx, params[1]);
	const auto& message_snowflake = amx_GetCppString(amx, params[2]);

	if (!channel_snowflake.length() || !message_snowflake.length())
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel/message length '{} {}'", channel_snowflake, message_snowflake);
		return 0;
	}

	if (!ChannelManager::Get()->FindChannelById(channel_snowflake))
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "invalid channel snowflake '{}'", channel_snowflake);
		return 0;
	}
	else if(MessageManager::Get()->FindById(message_snowflake))
	{
		Logger::Get()->LogNative(samplog_LogLevel::INFO, "message is already in cache! '{}'", message_snowflake);
		return 0;
	}

	auto
		cb_name = amx_GetCppString(amx, params[3]),
		cb_format = amx_GetCppString(amx, params[4]);

	pawn_cb::Error cb_error;
	auto cb = pawn_cb::Callback::Prepare(
		amx, cb_name.c_str(), cb_format.c_str(), params, 5, cb_error);
	if (cb_error && cb_error.get() != pawn_cb::Error::Type::EMPTY_NAME)
	{
		Logger::Get()->LogNative(samplog_LogLevel::ERROR, "could not prepare callback");
		return 0;
	}

	MessageManager::Get()->CreateFromSnowflake(channel_snowflake, message_snowflake, std::move(cb));
	Logger::Get()->LogNative(samplog_LogLevel::INFO, "return value: '1'");
	return 1;
}