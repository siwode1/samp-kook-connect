#include "Bot.hpp"

#include "Network.hpp"
#include "Http.hpp"
#include "WebSocket.hpp"
#include "PawnDispatcher.hpp"
#include "User.hpp"
#include "Channel.hpp"
#include "Guild.hpp"
#include "Logger.hpp"
#include "utils.hpp"

#include <json.hpp>
#include <fmt/format.h>

#include <map>


void ThisBot::TriggerTypingIndicator(Channel_t const &channel)
{
	Network::Get()->Http().Post(fmt::format("/channels/{:s}/typing", channel->GetId()), "");
}

void ThisBot::SetNickname(Guild_t const &guild, std::string const &nickname)
{
	json data = {
		{ "nick", nickname },
	};

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
		return;
	}

	Network::Get()->Http().Patch(
		fmt::format("/guilds/{:s}/members/@me/nick", guild->GetId()), json_str);
}

bool ThisBot::CreatePrivateChannel(User_t const &user, pawn_cb::Callback_t &&callback)
{
    // KOOK user-chat API: create DM by target user id
    json data = {
        { "target_id", user->GetId() },
    };

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
		return false;
	}
    // KOOK: POST /user-chat/create returns { code:0, data:{ code:"chat_code", target_info:{ id: "..." }, ... } }
    // Capture the requested user id for fallback use inside the lambda
    std::string requested_user_id = user->GetId();
    Network::Get()->Http().Post("/user-chat/create", json_str,
        [this, callback, requested_user_id](Http::Response r)
    {
        Logger::Get()->Log(samplog_LogLevel::DEBUG,
            "KOOK user-chat/create response: status {}; body: {}; add: {}",
            r.status, r.body, r.additional_data);
        if (r.status / 100 == 2)
        {
            json j = json::parse(r.body, nullptr, false);
            if (j.is_discarded())
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR, "user-chat/create invalid JSON: {}", r.body);
                return;
            }

            const json *payload = nullptr;
            auto it_d = j.find("data");
            if (it_d != j.end() && it_d->is_object()) payload = &(*it_d); else payload = &j;

            std::string chat_code;
            std::string target_uid;
            if (payload)
            {
                auto it_c = payload->find("code");
                if (it_c != payload->end() && it_c->is_string())
                    chat_code = it_c->get<std::string>();

                auto it_t = payload->find("target_info");
                if (it_t != payload->end() && it_t->is_object())
                {
                    auto it_id = it_t->find("id");
                    if (it_id != it_t->end() && it_id->is_string())
                        target_uid = it_id->get<std::string>();
                }
            }

            if (chat_code.empty())
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR, "user-chat/create returned no chat_code");
                return;
            }
            if (target_uid.empty())
            {
                // Fallback to the requested user id
                target_uid = requested_user_id;
            }

            ChannelId_t channel_id = ChannelManager::Get()->AddDMByChatCode(chat_code, target_uid);
            if (channel_id == INVALID_CHANNEL_ID)
                return;

            if (callback)
            {
                PawnDispatcher::Get()->Dispatch([=]()
                {
                    m_CreatedChannelId = channel_id;
                    callback->Execute();
                    m_CreatedChannelId = INVALID_CHANNEL_ID;
                });
            }
        }
    });

	return true;
}

std::string const &GetPresenceStatusString(ThisBot::PresenceStatus status)
{
	static const std::map<ThisBot::PresenceStatus, std::string> mapping{
		{ ThisBot::PresenceStatus::ONLINE, "online" },
		{ ThisBot::PresenceStatus::DO_NOT_DISTURB, "dnd" },
		{ ThisBot::PresenceStatus::IDLE, "idle" },
		{ ThisBot::PresenceStatus::INVISIBLE, "invisible" },
		{ ThisBot::PresenceStatus::OFFLINE, "offline" },
	};
	static std::string invalid;

	auto it = mapping.find(status);
	if (it == mapping.end())
		return invalid;

	return it->second;
}

bool ThisBot::SetPresenceStatus(PresenceStatus status)
{
	auto const &status_str = GetPresenceStatusString(status);
	if (status_str.empty())
		return false; // invalid status passed

	m_PresenceStatus = status;
	Network::Get()->WebSocket().UpdateStatus(status_str, m_ActivityName);
	return true;
}

void ThisBot::SetActivity(std::string const &name)
{
	m_ActivityName = name;

	Network::Get()->WebSocket().UpdateStatus(
		GetPresenceStatusString(m_PresenceStatus), m_ActivityName);
}
