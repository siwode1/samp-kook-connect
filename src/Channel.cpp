#include "Channel.hpp"
#include "Message.hpp"
#include "Network.hpp"
#include "PawnDispatcher.hpp"
#include "Logger.hpp"
#include "Guild.hpp"
#include "utils.hpp"
#include "Embed.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "fmt/format.h"
#include <random>
#include <sstream>
#include <iomanip>


#undef SendMessage // Windows at its finest


// Simple UTF-8 validator/sanitizer: copies valid UTF-8 sequences, replaces
// invalid bytes with '?' to avoid JSON UTF-8 exceptions.
static bool IsValidUtf8Continuation(unsigned char byte)
{
    return (byte & 0xC0) == 0x80;
}

ChannelId_t ChannelManager::AddDMByChatCode(std::string const &chat_code, Snowflake_t const &target_user_id)
{
    // Reuse existing channel if one already references this target_user_id with same chat_code
    for (auto const &kv : m_Channels)
    {
        const Channel_t &ch = kv.second;
        if (ch && ch->m_Type == Channel::Type::DM && ch->m_Id == target_user_id && ch->m_ChatCode == chat_code)
        {
            return ch->GetPawnId();
        }
    }

    ChannelId_t id = 1;
    while (m_Channels.find(id) != m_Channels.end())
        ++id;

    Channel_t chan(new Channel(id, target_user_id, Channel::Type::DM));
    chan->m_ChatCode = chat_code;

    if (!m_Channels.emplace(id, std::move(chan)).second)
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't create DM channel: duplicate key '{}'", id);
        return INVALID_CHANNEL_ID;
    }

    Logger::Get()->Log(samplog_LogLevel::INFO, "successfully added DM (chat_code) channel with id '{}'", id);
    return id;
}

#ifdef _WIN32
static std::string ToUtf8FromACP(std::string const &in)
{
    if (in.empty()) return std::string();
    int wlen = MultiByteToWideChar(CP_ACP, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0);
    if (wlen <= 0) return in; // fallback
    std::wstring w;
    w.resize(wlen);
    if (MultiByteToWideChar(CP_ACP, 0, in.c_str(), static_cast<int>(in.size()), &w[0], wlen) <= 0)
        return in; // fallback
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return in; // fallback
    std::string out;
    out.resize(ulen);
    if (WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], ulen, nullptr, nullptr) <= 0)
        return in; // fallback
    return out;
}
#else
static std::string ToUtf8FromACP(std::string const &in) { return in; }
#endif

// Normalize to valid UTF-8: try decoding as UTF-8 first; if invalid, decode from ACP.
#ifdef _WIN32
static std::string NormalizeToUtf8(std::string const &in)
{
    if (in.empty()) return std::string();
    // Try strict UTF-8 decode first
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.c_str(), (int)in.size(), nullptr, 0);
    if (wlen > 0)
    {
        std::wstring w;
        w.resize(wlen);
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.c_str(), (int)in.size(), &w[0], wlen) > 0)
        {
            int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            if (ulen > 0)
            {
                std::string out;
                out.resize(ulen);
                if (WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], ulen, nullptr, nullptr) > 0)
                    return out;
            }
        }
    }
    // Fallback: source likely in ACP
    return ToUtf8FromACP(in);
}
#else
static std::string NormalizeToUtf8(std::string const &in) { return in; }
#endif

static std::string GenerateNonce()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(gen), b = dist(gen);
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << a
        << std::setw(16) << std::setfill('0') << b;
    return oss.str();
}

static std::string SanitizeUtf8(std::string const &in)
{
    std::string out;
    out.reserve(in.size());
    const unsigned char *s = reinterpret_cast<const unsigned char*>(in.data());
    size_t i = 0, n = in.size();
    while (i < n)
    {
        unsigned char c = s[i];
        if (c < 0x80)
        {
            // ASCII
            out.push_back(static_cast<char>(c));
            ++i;
        }
        else if ((c >> 5) == 0x6 && i + 1 < n && IsValidUtf8Continuation(s[i+1]))
        {
            // 2-byte sequence
            out.push_back(static_cast<char>(s[i++]));
            out.push_back(static_cast<char>(s[i++]));
        }
        else if ((c >> 4) == 0xE && i + 2 < n && IsValidUtf8Continuation(s[i+1]) && IsValidUtf8Continuation(s[i+2]))
        {
            // 3-byte sequence
            out.push_back(static_cast<char>(s[i++]));
            out.push_back(static_cast<char>(s[i++]));
            out.push_back(static_cast<char>(s[i++]));
        }
        else if ((c >> 3) == 0x1E && i + 3 < n && IsValidUtf8Continuation(s[i+1]) && IsValidUtf8Continuation(s[i+2]) && IsValidUtf8Continuation(s[i+3]))
        {
            // 4-byte sequence
            out.push_back(static_cast<char>(s[i++]));
            out.push_back(static_cast<char>(s[i++]));
            out.push_back(static_cast<char>(s[i++]));
            out.push_back(static_cast<char>(s[i++]));
        }
        else
        {
            // invalid byte, replace with '?'
            out.push_back('?');
            ++i;
        }
    }
    return out;
}

Channel::Channel(ChannelId_t pawn_id, json const &data, GuildId_t guild_id) :
	m_PawnId(pawn_id)
{
	std::underlying_type<Type>::type type;
	if (!utils::TryGetJsonValue(data, type, "type")
		|| !utils::TryGetJsonValue(data, m_Id, "id"))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"invalid JSON: expected \"type\" and \"id\" in \"{}\"", data.dump());
		return;
	}

	m_Type = static_cast<Type>(type);
	if (m_Type == Type::GUILD_TEXT || m_Type == Type::GUILD_VOICE || m_Type == Type::GUILD_CATEGORY)
	{
		if (guild_id != 0)
		{
			m_GuildId = guild_id;
		}
		else
		{
			std::string guild_id_str;
			if (utils::TryGetJsonValue(data, guild_id_str, "guild_id"))
			{
				Guild_t const &guild = GuildManager::Get()->FindGuildById(guild_id_str);
				m_GuildId = guild->GetPawnId();
				guild->AddChannel(pawn_id);
			}
			else
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"guild_id\" in \"{}\"", data.dump());
			}
		}
		Update(data);
	}
}

Channel::Channel(ChannelId_t pawn_id, Snowflake_t channel_id, Type type) :
	m_PawnId(pawn_id),
	m_Id(channel_id),
	m_Type(type)
{
}

void Channel::Update(json const &data)
{
	utils::TryGetJsonValue(data, m_Name, "name"),
	utils::TryGetJsonValue(data, m_Topic, "topic"),
	utils::TryGetJsonValue(data, m_Position, "position"),
	utils::TryGetJsonValue(data, m_IsNsfw, "nsfw");
}

void Channel::UpdateParentChannel(Snowflake_t const &parent_id)
{
	if (parent_id.length() < 1)
	{
		m_ParentId = 0;
		return;
	}

	Channel_t const &channel = ChannelManager::Get()->FindChannelById(parent_id);
	if (!channel)
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"can't update parent channel \"parent_id\" not cached \"{}\"", parent_id);
		return;
	}
	m_ParentId = channel->GetPawnId();
}

void Channel::SendMessage(std::string &&msg, pawn_cb::Callback_t &&cb)
{
    // Prefer UTF-8; if invalid, convert from ACP; then sanitize
    std::string norm = NormalizeToUtf8(msg);
    std::string safe_content = SanitizeUtf8(norm);

    json data = {
        { "content", std::move(safe_content) },
        { "type", 9 }, // KOOK: 9 = kmarkdown text
        { "nonce", GenerateNonce() }
    };
    // If this is a DM created via user-chat, prefer chat_code; otherwise use target_id (channel id)
    if (!m_ChatCode.empty())
    {
        data["chat_code"] = m_ChatCode;
    }
    else
    {
        data["target_id"] = GetId();
    }

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return; // do not perform HTTP request if serialization failed
    }

	Http::ResponseCb_t response_cb;
	if (cb)
	{
		response_cb = [cb](Http::Response response)
		{
			Logger::Get()->Log(samplog_LogLevel::DEBUG,
				"KOOK message/create response: status {}; body: {}; add: {}",
				response.status, response.body, response.additional_data);
			if (response.status / 100 == 2)
			{
				// Try to extract msg_id and build a Message handle for KCC_GetCreatedMessage()
				std::string msg_id;
				try
				{
					json j = json::parse(response.body, nullptr, false);
					if (j.is_object())
					{
						auto it = j.find("data");
						if (it != j.end() && it->is_object())
						{
							auto it2 = it->find("msg_id");
							if (it2 != it->end() && it2->is_string())
								msg_id = it2->get<std::string>();
						}
					}
				}
				catch (...) {}

				if (!msg_id.empty())
				{
					Network::Get()->Http().Get(std::string("/message/view?msg_id=") + msg_id,
						[cb](Http::Response r2)
						{
							Logger::Get()->Log(samplog_LogLevel::DEBUG,
								"KOOK message/view response: status {}; body: {}",
								r2.status, r2.body);
							if (r2.status / 100 == 2)
							{
								json j2 = json::parse(r2.body, nullptr, false);
								json payload = j2;
								if (j2.is_object())
								{
									auto d = j2.find("data");
									if (d != j2.end() && d->is_object()) payload = *d;
								}
								PawnDispatcher::Get()->Dispatch([cb, payload]() mutable
								{
									auto msg = MessageManager::Get()->Create(payload);
									if (msg != INVALID_MESSAGE_ID)
									{
										MessageManager::Get()->SetCreatedMessageId(msg);
										cb->Execute();
										if (!MessageManager::Get()->Find(msg)->Persistent())
										{
											MessageManager::Get()->Delete(msg);
										}
										MessageManager::Get()->SetCreatedMessageId(INVALID_MESSAGE_ID);
										return;
									}
									cb->Execute();
								});
								return;
							}
							PawnDispatcher::Get()->Dispatch([cb]() mutable { cb->Execute(); });
						});
					return; // defer until message/view completes
				}

				// No msg_id: still invoke callback
				PawnDispatcher::Get()->Dispatch([cb]() mutable { cb->Execute(); });
			}
		};
	}

    // Use DM endpoint when addressing by chat_code, otherwise channel message endpoint
    const char* endpoint = m_ChatCode.empty() ? "/message/create" : "/direct-message/create";
    Network::Get()->Http().Post(endpoint, json_str, std::move(response_cb));
}

void Channel::SetChannelName(std::string const &name)
{
    // Normalize and sanitize to valid UTF-8 for KOOK API
    std::string utf8_name = NormalizeToUtf8(name);
    std::string safe_name = SanitizeUtf8(utf8_name);
    json data = {
        { "channel_id", GetId() },
        { "name", safe_name }
    };

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);

    // KOOK: POST /channel/update
    Network::Get()->Http().Post("/channel/update", json_str);
}

void Channel::SetChannelTopic(std::string const &topic)
{
    // Normalize and sanitize to valid UTF-8 for KOOK API
    std::string utf8_topic = NormalizeToUtf8(topic);
    std::string safe_topic = SanitizeUtf8(utf8_topic);
    json data = {
        { "channel_id", GetId() },
        { "topic", safe_topic }
    };

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);

    // KOOK: POST /channel/update
    Network::Get()->Http().Post("/channel/update", json_str);
}

void Channel::SetChannelPosition(int const position)
{
    json data = {
        { "channel_id", GetId() },
        { "level", position }
    };

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);

    // KOOK: POST /channel/update
    Network::Get()->Http().Post("/channel/update", json_str);
}

void Channel::SetChannelNsfw(bool const is_nsfw)
{
	json data = {
		{ "channel_id", GetId() },
		{ "nsfw", is_nsfw }
	};

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);

	Network::Get()->Http().Patch(fmt::format("/channel/{:s}", GetId()), json_str);
}

void Channel::SetChannelParentCategory(Channel_t const &parent)
{
	if (parent->GetType() != Type::GUILD_CATEGORY)
		return;

	json data = {
		{ "parent_id", parent->GetId() }
	};

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);

	Network::Get()->Http().Patch(fmt::format("/channel/{:s}", GetId()), json_str);
}

void Channel::DeleteChannel()
{
	json data = {
		{ "channel_id", GetId() }
	};

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
		return;
	}

	// KOOK: POST /channel/delete
	Network::Get()->Http().Post("/channel/delete", json_str);
}

void Channel::SendEmbeddedMessage(const Embed_t & embed, std::string&& msg, pawn_cb::Callback_t&& cb)
{
    // Build KOOK CardMessage content from Embed and optional message
    // Normalize optional message as kmarkdown section
    std::string utf8_content = ToUtf8FromACP(msg);
    std::string safe_content = SanitizeUtf8(utf8_content);

    json card = json::array();
    json card_obj;
    card_obj["type"] = "card";

    // color
    {
        unsigned int color = embed->GetColor();
        char buf[10];
        std::snprintf(buf, sizeof(buf), "#%06X", color & 0xFFFFFF);
        card_obj["color"] = std::string(buf);
    }

    json modules = json::array();

    if (!safe_content.empty())
    {
        json section;
        section["type"] = "section";
        json text;
        text["type"] = "kmarkdown";
        text["content"] = safe_content;
        section["text"] = text;
        modules.push_back(section);
    }

    // Normalize embed title to UTF-8
    std::string title_safe;
    {
        std::string t_utf8 = ToUtf8FromACP(embed->GetTitle());
        title_safe = SanitizeUtf8(t_utf8);
    }
    if (!title_safe.empty())
    {
        json header;
        header["type"] = "header";
        json text;
        text["type"] = "plain-text";
        text["content"] = title_safe;
        header["text"] = text;
        modules.push_back(header);
    }

    // Normalize embed description to UTF-8
    std::string desc_safe;
    {
        std::string d_utf8 = ToUtf8FromACP(embed->GetDescription());
        desc_safe = SanitizeUtf8(d_utf8);
    }
    if (!desc_safe.empty())
    {
        json section;
        section["type"] = "section";
        json text;
        text["type"] = "kmarkdown";
        text["content"] = desc_safe;
        section["text"] = text;
        modules.push_back(section);
    }

    // Optional URL: add a section with a link button on the right
    if (!embed->GetUrl().empty())
    {
        json section;
        section["type"] = "section";
        section["mode"] = "right"; // accessory at right side
        // text with kmarkdown link for better a11y
        json text;
        text["type"] = "kmarkdown";
        text["content"] = fmt::format("[打开链接]({})", embed->GetUrl());
        section["text"] = text;
        // button accessory
        json btn;
        btn["type"] = "button";
        btn["theme"] = "primary";
        btn["click"] = "link";
        btn["value"] = embed->GetUrl();
        json btn_text;
        btn_text["type"] = "plain-text";
        btn_text["content"] = "打开链接";
        btn["text"] = btn_text;
        section["accessory"] = btn;
        modules.push_back(section);
    }

    if (embed->GetFields().size())
    {
        json section;
        section["type"] = "section";
        json para;
        para["type"] = "paragraph";
        para["cols"] = 3;
        json fields = json::array();
        for (const auto& f : embed->GetFields())
        {
            json fld;
            fld["type"] = "kmarkdown";
            // Normalize field name/value to UTF-8
            std::string name_safe = SanitizeUtf8(ToUtf8FromACP(f._name));
            std::string value_safe = SanitizeUtf8(ToUtf8FromACP(f._value));
            fld["content"] = fmt::format("**{}**\n{}", name_safe, value_safe);
            fields.push_back(fld);
        }
        para["fields"] = fields;
        json text;
        text["type"] = "paragraph";
        text["fields"] = fields;
        section["text"] = text;
        modules.push_back(section);
    }

    if (!embed->GetThumbnailUrl().empty())
    {
        json img_group;
        img_group["type"] = "image-group";
        json elements = json::array();
        json img;
        img["type"] = "image";
        img["src"] = embed->GetThumbnailUrl();
        elements.push_back(img);
        img_group["elements"] = elements;
        modules.push_back(img_group);
    }

    if (!embed->GetImageUrl().empty())
    {
        json img_group;
        img_group["type"] = "image-group";
        json elements = json::array();
        json img;
        img["type"] = "image";
        img["src"] = embed->GetImageUrl();
        elements.push_back(img);
        img_group["elements"] = elements;
        modules.push_back(img_group);
    }

    // Normalize footer text to UTF-8
    std::string footer_safe;
    {
        std::string f_utf8 = ToUtf8FromACP(embed->GetFooterText());
        footer_safe = SanitizeUtf8(f_utf8);
    }
    // Prepare timestamp (raw string for now)
    const std::string ts_raw = embed->GetTimestamp();
    const bool has_footer = !footer_safe.empty();
    const bool has_footer_icon = !embed->GetFooterIconUrl().empty();
    const bool has_timestamp = !ts_raw.empty();
    if (has_footer || has_footer_icon || has_timestamp)
    {
        json context;
        context["type"] = "context";
        json elements = json::array();
        // footer icon
        if (has_footer_icon)
        {
            json img;
            img["type"] = "image";
            img["src"] = embed->GetFooterIconUrl();
            elements.push_back(img);
        }
        // footer text
        if (has_footer)
        {
            json txt;
            txt["type"] = "plain-text";
            txt["content"] = footer_safe;
            elements.push_back(txt);
        }
        // timestamp
        if (has_timestamp)
        {
            json ts;
            ts["type"] = "plain-text";
            ts["content"] = fmt::format(" | 时间: {}", ts_raw);
            elements.push_back(ts);
        }
        context["elements"] = elements;
        modules.push_back(context);
    }

    card_obj["modules"] = modules;
    card.push_back(card_obj);

    std::string card_str;
    utils::TryDumpJson(card, card_str);

    // Build KOOK message/create payload
    json data = {
        { "content", card_str },
        { "type", 10 }, // CardMessage
        { "nonce", GenerateNonce() }
    };
    // If this is a DM created via user-chat, prefer chat_code; otherwise use target_id (channel id)
    if (!m_ChatCode.empty())
    {
        data["chat_code"] = m_ChatCode;
    }
    else
    {
        data["target_id"] = GetId();
    }

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    Http::ResponseCb_t response_cb;
    if (cb)
    {
        response_cb = [cb](Http::Response response)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK message/create response: status {}; body: {}; add: {}",
                response.status, response.body, response.additional_data);
            if (response.status / 100 == 2)
            {
                // Expect KOOK format: { code:0, data:{ msg_id: "..." }, ... }
                std::string msg_id;
                try
                {
                    json j = json::parse(response.body, nullptr, false);
                    if (j.is_object())
                    {
                        auto it = j.find("data");
                        if (it != j.end() && it->is_object())
                        {
                            auto it2 = it->find("msg_id");
                            if (it2 != it->end() && it2->is_string())
                                msg_id = it2->get<std::string>();
                        }
                    }
                }
                catch (...) {}

                if (!msg_id.empty())
                {
                    // Fetch full message view to build a Message handle
                    Network::Get()->Http().Get(std::string("/message/view?msg_id=") + msg_id,
                        [cb](Http::Response r2)
                        {
                            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                                "KOOK message/view response: status {}; body: {}",
                                r2.status, r2.body);
                            if (r2.status / 100 == 2)
                            {
                                json j2 = json::parse(r2.body, nullptr, false);
                                // Attempt to extract 'data' as the message payload
                                json payload = j2;
                                if (j2.is_object())
                                {
                                    auto d = j2.find("data");
                                    if (d != j2.end() && d->is_object()) payload = *d;
                                }
                                PawnDispatcher::Get()->Dispatch([cb, payload]() mutable
                                {
                                    auto msg = MessageManager::Get()->Create(payload);
                                    if (msg != INVALID_MESSAGE_ID)
                                    {
                                        MessageManager::Get()->SetCreatedMessageId(msg);
                                        cb->Execute();
                                        if (!MessageManager::Get()->Find(msg)->Persistent())
                                        {
                                            MessageManager::Get()->Delete(msg);
                                        }
                                        MessageManager::Get()->SetCreatedMessageId(INVALID_MESSAGE_ID);
                                        return;
                                    }
                                    // Fallback: invoke callback without handle
                                    cb->Execute();
                                });
                                return;
                            }
                            // If fetch failed, still execute callback
                            PawnDispatcher::Get()->Dispatch([cb]() mutable { cb->Execute(); });
                        });
                    return; // defer cb until view returns
                }

                // No msg_id found; execute callback directly
                PawnDispatcher::Get()->Dispatch([cb]() mutable { cb->Execute(); });
            }
            else
            {
                // Non-2xx: still execute callback to unblock user code
                PawnDispatcher::Get()->Dispatch([cb]() mutable { cb->Execute(); });
            }
        };
    }

    // Use DM endpoint when addressing by chat_code, otherwise channel message endpoint
    const char* endpoint = m_ChatCode.empty() ? "/message/create" : "/direct-message/create";
    Network::Get()->Http().Post(endpoint, json_str, std::move(response_cb));
}

void ChannelManager::Initialize()
{
    assert(m_Initialized != m_InitValue);

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::CHANNEL_CREATE, [](json const &data)
	{
		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			auto const channel_id = ChannelManager::Get()->AddChannel(data);
			if (channel_id == INVALID_CHANNEL_ID)
				return;

			// forward KCC_OnChannelCreate(DCC_Channel:channel);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnChannelCreate", channel_id);
		});
	});
	
	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::CHANNEL_UPDATE, [](json const &data)
	{
		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			Snowflake_t sfid;
			if (!utils::TryGetJsonValue(data, sfid, "id"))
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"id\" in \"{}\"", data.dump());
				return;
			}

			Channel_t const &channel = ChannelManager::Get()->FindChannelById(sfid);
			if (!channel)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update channel: channel id \"{}\" not cached", sfid);
				return;
			}

			channel->Update(data);
			if (channel->GetType() != Channel::Type::GUILD_CATEGORY)
			{
				Snowflake_t parent_id;
				utils::TryGetJsonValue(data, parent_id, "parent_id");
				channel->UpdateParentChannel(parent_id);
			}

			// forward KCC_OnChannelUpdate(DCC_Channel:channel);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnChannelUpdate", channel->GetPawnId());
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::CHANNEL_DELETE, [](json const &data)
	{
		ChannelManager::Get()->DeleteChannel(data);
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::READY, [this](json const &data)
	{
		static const char *PRIVATE_CHANNEL_KEY = "private_channels";
		if (utils::IsValidJson(data, PRIVATE_CHANNEL_KEY, json::value_t::array))
		{
			for (auto const &c : data.at(PRIVATE_CHANNEL_KEY))
				AddChannel(c);
		}
		else
		{
			// KOOK READY may not contain private_channels; skip DM cache warm-up.
			Logger::Get()->Log(samplog_LogLevel::DEBUG,
				"READY has no '{}' field; skipping DM channels warm-up.", PRIVATE_CHANNEL_KEY);
		}
		m_Initialized++;
	});
}

bool ChannelManager::IsInitialized()
{
	return m_Initialized == m_InitValue;
}

bool ChannelManager::CreateGuildChannel(Guild_t const &guild,
    std::string const &name, Channel::Type type, pawn_cb::Callback_t &&cb)
{
    // Build KOOK-compliant payload
    json data;
    data["guild_id"] = guild->GetId();
    // Normalize and sanitize channel name to valid UTF-8 to avoid JSON exceptions
    {
        std::string utf8_name = NormalizeToUtf8(name);
        std::string safe_name = SanitizeUtf8(utf8_name);
        data["name"] = safe_name;
    }
    if (type == Channel::Type::GUILD_CATEGORY)
    {
        // When is_category = 1, KOOK only accepts guild_id/name/is_category
        data["is_category"] = 1;
    }
    else
    {
        // KOOK: 1 text, 2 voice
        int kook_type = (type == Channel::Type::GUILD_VOICE) ? 2 : 1;
        data["type"] = kook_type;
    }

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return false;
    }

    Network::Get()->Http().Post(std::string("/channel/create"), json_str,
        [this, cb, guild_id = guild->GetPawnId()](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "channel create response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 == 2)
            {
                json j = json::parse(r.body, nullptr, false);
                if (!j.is_discarded() && j.is_object())
                {
                    const json *payload = nullptr;
                    auto it_d = j.find("data");
                    if (it_d != j.end() && it_d->is_object()) payload = &(*it_d); else payload = &j;

                    if (payload)
                    {
                        ChannelId_t channel_id = ChannelManager::Get()->AddChannel(*payload, guild_id);
                        if (channel_id != INVALID_CHANNEL_ID)
                        {
                            if (auto const &g = GuildManager::Get()->FindGuild(guild_id))
                                g->AddChannel(channel_id);

                            if (cb)
                            {
                                PawnDispatcher::Get()->Dispatch([=]()
                                {
                                    m_CreatedChannelId = channel_id;
                                    cb->Execute();
                                    m_CreatedChannelId = INVALID_CHANNEL_ID;
                                });
                            }
                        }
                    }
                }
            }
            else
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK channel/create failed: {} {}", r.status, r.reason);
            }
        });

    return true;
}

ChannelId_t ChannelManager::AddChannel(json const &data, GuildId_t guild_id/* = 0*/)
{
	Snowflake_t sfid;
	if (!utils::TryGetJsonValue(data, sfid, "id"))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"invalid JSON: expected \"id\" in \"{}\"", data.dump());
		return INVALID_CHANNEL_ID;
	}

	Channel_t const& channel = FindChannelById(sfid);
	if (channel)
		return channel->GetPawnId(); // channel already exists

	ChannelId_t id = 1;
	while (m_Channels.find(id) != m_Channels.end())
		++id;

	if (!m_Channels.emplace(id, Channel_t(new Channel(id, data, guild_id))).second)
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"can't create channel: duplicate key '{}'", id);
		return INVALID_CHANNEL_ID;
	}

	Logger::Get()->Log(samplog_LogLevel::INFO, "successfully added channel with id '{}'", id);
	return id;
}

ChannelId_t ChannelManager::AddChannelById(Snowflake_t const &sfid, Channel::Type type)
{
    Channel_t const& channel = FindChannelById(sfid);
    if (channel)
        return channel->GetPawnId(); // channel already exists

    ChannelId_t id = 1;
    while (m_Channels.find(id) != m_Channels.end())
        ++id;

    if (!m_Channels.emplace(id, Channel_t(new Channel(id, sfid, type))).second)
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR,
            "can't create channel: duplicate key '{}'", id);
        return INVALID_CHANNEL_ID;
    }

    Logger::Get()->Log(samplog_LogLevel::INFO, "successfully added placeholder channel with id '{}'", id);
    // Resolve real channel type asynchronously using KOOK API
    ResolveChannelTypeAsync(sfid);
    return id;
}

void ChannelManager::ResolveChannelTypeAsync(Snowflake_t const &channel_id)
{
    // Query KOOK channel view with target_id per KOOK docs
    Network::Get()->Http().Get(std::string("/channel/view?target_id=") + channel_id,
        [this, channel_id](Http::Response r)
        {
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::DEBUG, "ResolveChannelTypeAsync failed: {} {}", r.status, r.reason);
                return;
            }
            json j = json::parse(r.body, nullptr, false);
            if (j.is_discarded())
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR, "ResolveChannelTypeAsync invalid JSON: {}", r.body);
                return;
            }
            auto d = j.find("data");
            if (d == j.end() || !d->is_object())
                return;
            int ktype = -1;
            auto it_t = d->find("type");
            if (it_t != d->end() && it_t->is_number_integer())
                ktype = it_t->get<int>();

            // Map KOOK channel type -> internal enum
            Channel::Type mapped = Channel::Type::GUILD_TEXT;
            if (ktype == 1) mapped = Channel::Type::GUILD_TEXT;
            else if (ktype == 2) mapped = Channel::Type::GUILD_VOICE;
            else if (ktype == 0) mapped = Channel::Type::GUILD_CATEGORY;

            // Update cached channel if present
            Channel_t const &ch = FindChannelById(channel_id);
            if (ch)
            {
                ch->m_Type = mapped;
                Logger::Get()->Log(samplog_LogLevel::DEBUG, "Resolved channel '{}' type to {}", channel_id, static_cast<int>(mapped));

                // Update basic fields: name/topic if present
                auto it_name = d->find("name");
                if (it_name != d->end() && it_name->is_string())
                {
                    ch->m_Name = it_name->get<std::string>();
                }
                auto it_topic = d->find("topic");
                if (it_topic != d->end() && it_topic->is_string())
                {
                    ch->m_Topic = it_topic->get<std::string>();
                }
                // KOOK: 'level' is the channel sorting order; map to m_Position
                auto it_level = d->find("level");
                if (it_level != d->end() && it_level->is_number_integer())
                {
                    ch->m_Position = it_level->get<int>();
                }

                // Try to set guild association if available
                auto it_g = d->find("guild_id");
                if (it_g != d->end() && it_g->is_string())
                {
                    std::string gid = it_g->get<std::string>();
                    Guild_t const &g = GuildManager::Get()->FindGuildById(gid);
                    if (g)
                    {
                        ch->m_GuildId = g->GetPawnId();
                        Logger::Get()->Log(samplog_LogLevel::DEBUG, "Resolved channel '{}' guild to {}", channel_id, gid);
                    }
                    else
                    {
                        // Create a placeholder guild so Pawn code can immediately use KCC_GetChannelGuild
                        GuildId_t new_gid = GuildManager::Get()->AddGuildById(gid);
                        if (new_gid != INVALID_GUILD_ID)
                        {
                            ch->m_GuildId = new_gid;
                            Logger::Get()->Log(samplog_LogLevel::DEBUG, "Created placeholder guild for '{}' and linked channel '{}'", gid, channel_id);
                        }
                    }
                }

                // Set parent category if provided
                auto it_parent = d->find("parent_id");
                if (it_parent != d->end() && it_parent->is_string())
                {
                    std::string parent_id = it_parent->get<std::string>();
                    if (!parent_id.empty())
                    {
                        Channel_t const &parent = FindChannelById(parent_id);
                        if (!parent)
                        {
                            // Create placeholder category for parent, KOOK guarantees parent_id is a category
                            ChannelId_t pid = AddChannelById(parent_id, Channel::Type::GUILD_CATEGORY);
                            if (pid != INVALID_CHANNEL_ID)
                            {
                                ch->UpdateParentChannel(parent_id);
                                Logger::Get()->Log(samplog_LogLevel::DEBUG, "Linked channel '{}' to parent '{}' (placeholder)", channel_id, parent_id);
                            }
                        }
                        else
                        {
                            ch->UpdateParentChannel(parent_id);
                            Logger::Get()->Log(samplog_LogLevel::DEBUG, "Linked channel '{}' to parent '{}'", channel_id, parent_id);
                        }
                    }
                }
            }
        });
}

ChannelId_t ChannelManager::AddDMChannel(json const& data)
{
	Snowflake_t sfid;
	if (!utils::TryGetJsonValue(data, sfid, "channel_id"))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"invalid JSON: expected \"channel_id\" in \"{}\"", data.dump());
		return INVALID_CHANNEL_ID;
	}

	Channel_t const& channel = FindChannelById(sfid);
	if (channel)
		return channel->GetPawnId(); // channel already exists

	ChannelId_t id = 1;
	while (m_Channels.find(id) != m_Channels.end())
		++id;

	if (!m_Channels.emplace(id, Channel_t(new Channel(id, sfid, Channel::Type::DM))).second)
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"can't create channel: duplicate key '{}'", id);
		return INVALID_CHANNEL_ID;
	}

	Logger::Get()->Log(samplog_LogLevel::INFO, "successfully added channel with id '{}'", id);
	return id;
}

void ChannelManager::DeleteChannel(json const &data)
{
	Snowflake_t sfid;
	if (!utils::TryGetJsonValue(data, sfid, "id"))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"invalid JSON: expected \"id\" in \"{}\"", data.dump());
		return;
	}

	PawnDispatcher::Get()->Dispatch([this, sfid]()
	{
		Channel_t const &channel = FindChannelById(sfid);
		if (!channel)
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"can't delete channel: channel id \"{}\" not cached", sfid);
			return;
		}

		// forward KCC_OnChannelDelete(DCC_Channel:channel);
		pawn_cb::Error error;
		pawn_cb::Callback::CallFirst(error, "KCC_OnChannelDelete", channel->GetPawnId());

		Guild_t const &guild = GuildManager::Get()->FindGuild(channel->GetGuildId());
		if (guild)
			guild->RemoveChannel(channel->GetPawnId());

		m_Channels.erase(channel->GetPawnId());
	});

}

Channel_t const &ChannelManager::FindChannel(ChannelId_t id)
{
	static Channel_t invalid_channel;
	auto it = m_Channels.find(id);
	if (it == m_Channels.end())
		return invalid_channel;
	return it->second;
}

Channel_t const &ChannelManager::FindChannelByName(std::string const &name)
{
	static Channel_t invalid_channel;
	for (auto const &c : m_Channels)
	{
		Channel_t const &channel = c.second;
		if (channel->GetName().compare(name) == 0)
			return channel;
	}
	return invalid_channel;
}

Channel_t const &ChannelManager::FindChannelById(Snowflake_t const &sfid)
{
	static Channel_t invalid_channel;
	for (auto const &c : m_Channels)
	{
		Channel_t const &channel = c.second;
		if (channel->GetId().compare(sfid) == 0)
			return channel;
	}
	return invalid_channel;
}
