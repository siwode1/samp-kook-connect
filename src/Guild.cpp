#include "Guild.hpp"
#include "Network.hpp"
#include "Channel.hpp"
#include "User.hpp"
#include "Role.hpp"
#include "PawnDispatcher.hpp"
#include "Logger.hpp"
#include "utils.hpp"
#include "Callback.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <string>
 #include <algorithm>

// Helpers to normalize strings to valid UTF-8 for KOOK APIs
#ifdef _WIN32
static std::string ToUtf8FromACP_Guild(std::string const &in)
{
    if (in.empty()) return std::string();
    int wlen = MultiByteToWideChar(CP_ACP, 0, in.c_str(), (int)in.size(), nullptr, 0);
    if (wlen <= 0) return in;
    std::wstring w;
    w.resize(wlen);
    if (MultiByteToWideChar(CP_ACP, 0, in.c_str(), (int)in.size(), &w[0], wlen) <= 0)
        return in;
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return in;
    std::string out;
    out.resize(ulen);
    if (WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], ulen, nullptr, nullptr) <= 0)
        return in;
    return out;
}

// Forward declaration used by sanitizer
static bool IsValidUtf8Cont_Guild(unsigned char byte);

#else
static std::string ToUtf8FromACP_Guild(std::string const &in) { return in; }
#endif

static bool IsValidUtf8Cont_Guild(unsigned char byte) { return (byte & 0xC0) == 0x80; }

static std::string SanitizeUtf8_Guild(std::string const &in)
{
    std::string out; out.reserve(in.size());
    const unsigned char *s = reinterpret_cast<const unsigned char*>(in.data());
    size_t i = 0, n = in.size();
    while (i < n)
    {
        unsigned char c = s[i];
        if (c < 0x80) { out.push_back((char)c); ++i; }
        else if ((c >> 5) == 0x6 && i + 1 < n && IsValidUtf8Cont_Guild(s[i+1])) { out.push_back((char)s[i++]); out.push_back((char)s[i++]); }
        else if ((c >> 4) == 0xE && i + 2 < n && IsValidUtf8Cont_Guild(s[i+1]) && IsValidUtf8Cont_Guild(s[i+2])) { out.push_back((char)s[i++]); out.push_back((char)s[i++]); out.push_back((char)s[i++]); }
        else if ((c >> 3) == 0x1E && i + 3 < n && IsValidUtf8Cont_Guild(s[i+1]) && IsValidUtf8Cont_Guild(s[i+2]) && IsValidUtf8Cont_Guild(s[i+3])) { out.push_back((char)s[i++]); out.push_back((char)s[i++]); out.push_back((char)s[i++]); out.push_back((char)s[i++]); }
        else { out.push_back('?'); ++i; }
    }
    return out;
}

#include <unordered_map>

Guild::Guild(GuildId_t pawn_id, json const &data) :
    m_PawnId(pawn_id)
{
    if (!utils::TryGetJsonValue(data, m_Id, "id"))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR,
            "invalid JSON: expected \"id\" in \"{}\"", data.dump());
        return;
    }
    Update(data);

	if (utils::IsValidJson(data, "channels", json::value_t::array))
	{
		for (auto &c : data["channels"])
		{
			auto const channel_id = ChannelManager::Get()->AddChannel(c, pawn_id);
			if (channel_id == INVALID_CHANNEL_ID)
				continue;

			AddChannel(channel_id);
		}

		//After caching is done, cache Parent channel accordingly
		for (auto &c : data["channels"])
		{
			Snowflake_t id;
			if (!utils::TryGetJsonValue(c, id, "id"))
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR, "invalid JSON: expected \"id\" in \"{}\"", c.dump());
				break;
			}

			auto const &channel = ChannelManager::Get()->FindChannelById(id);
			if (channel)
			{
				if (channel->GetType() == Channel::Type::GUILD_CATEGORY)
					continue;

				Snowflake_t parent_id;
				utils::TryGetJsonValue(c, parent_id, "parent_id");
				channel->UpdateParentChannel(parent_id);
			}
		}
	}

	if (utils::IsValidJson(data, "members", json::value_t::array))
	{
		for (auto &m : data["members"])
		{
			if (!utils::IsValidJson(m, "user", json::value_t::object))
			{
				// we break here because all other array entries are likely
				// to be invalid too, and we don't want to spam an error message
				// for every single element in this array
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"user\" in \"{}\"", m.dump());
				break;
			}

			Member member;
			member.UserId = UserManager::Get()->AddUser(m["user"]);
			member.Update(m);
			AddMember(std::move(member));
		}

		unsigned int member_count;
		if (!utils::TryGetJsonValue(data, member_count, "member_count")
			|| member_count != m_Members.size())
		{
			Network::Get()->WebSocket().RequestGuildMembers(m_Id);
		}
	}

	if (utils::IsValidJson(data, "voice_states", json::value_t::array))
	{
		for (auto &v : data["voice_states"])
		{
			Snowflake_t channel_id;
			if (!utils::TryGetJsonValue(v, channel_id, "channel_id"))
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"channel.id\" in \"{}\"", v.dump());
				break;
			}
			
			Snowflake_t user_id;
			if (!utils::TryGetJsonValue(v, user_id, "user_id"))
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"user.id\" in \"{}\"", v.dump());
				break;
			}

			Channel_t const &channel = ChannelManager::Get()->FindChannelById(channel_id);			
						
			for (auto &m : m_Members)
			{
				User_t const &user = UserManager::Get()->FindUser(m.UserId);
				if (user && user->GetId() == user_id)
				{
					m.UpdateVoiceChannel(channel ? channel->GetPawnId() : INVALID_CHANNEL_ID);
					break;
				}
			}
		}
	}

	if (utils::IsValidJson(data, "presences", json::value_t::array))
	{
		for (auto &p : data["presences"])
		{
			Snowflake_t userid;
			if (!utils::TryGetJsonValue(p, userid, "user", "id"))
			{
				// see above on why we break here
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"user.id\" in \"{}\"", p.dump());
				break;
			}

			std::string status;
			if (!utils::TryGetJsonValue(p, status, "status"))
			{
				// see above on why we break here
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"status\" in \"{}\"", p.dump());
				break;
			}

			for (auto &m : m_Members)
			{
				User_t const &user = UserManager::Get()->FindUser(m.UserId);
				assert(user);
				if (user->GetId() == userid)
				{
					m.UpdatePresence(status);
					break;
				}
			}
		}
	}
}

void Guild::Member::Update(json const &data)
{
	// we don't care about the user object, there's an extra event for users
	if (utils::IsValidJson(data, "roles", json::value_t::array))
	{
		Roles.clear();
		for (auto &mr : data["roles"])
		{
			if (!mr.is_string())
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: not a string: \"{}\"", mr.dump());
				break;
			}

			Snowflake_t sfid = mr.get<std::string>();
			Role_t const &role = RoleManager::Get()->FindRoleById(sfid);
			if (role)
			{
				Roles.push_back(role->GetPawnId());
			}
			else
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update member role: role id \"{}\" not cached", sfid);
			}
		}
	}

	if (data.find("nick") != data.end())
	{
		auto &nick_json = data["nick"];
		if (nick_json.is_string())
			Nickname = data["nick"].get<std::string>();
		else if (nick_json.is_null())
			Nickname.clear();
		else
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: invalid datatype for \"nick\" in \"{}\"", data.dump());
	}
}

void Guild::Member::UpdatePresence(std::string const &status)
{
	// "idle", "dnd", "online", or "offline"
	static const std::unordered_map<std::string, Member::PresenceStatus> status_map{
		{ "idle", Member::PresenceStatus::IDLE },
		{ "dnd", Member::PresenceStatus::DO_NOT_DISTURB },
		{ "online", Member::PresenceStatus::ONLINE },
		{ "offline", Member::PresenceStatus::OFFLINE }
	};

	Status = status_map.at(status);
}

void Guild::Member::UpdateVoiceChannel(ChannelId_t const &Channel)
{
	VoiceChannel = Channel;
}

void Guild::UpdateMember(UserId_t userid, json const &data)
{
	for (auto &m : m_Members)
	{
		if (m.UserId != userid)
			continue;

		m.Update(data);
		break;
	}
}

void Guild::UpdateMemberPresence(UserId_t userid, std::string const &status)
{
	for (auto &m : m_Members)
	{
		if (m.UserId != userid)
			continue;

		m.UpdatePresence(status);
		break;
	}
}

void Guild::UpdateMemberVoiceChannel(UserId_t user_id, ChannelId_t const &channel)
{
	for (auto &m : m_Members)
	{
		if (m.UserId != user_id)
			continue;

		m.UpdateVoiceChannel(channel);
		break;
	}
}

void Guild::Update(json const &data)
{
	utils::TryGetJsonValue(data, m_Name, "name");

	// KOOK: server owner is provided as 'user_id'
	utils::TryGetJsonValue(data, m_OwnerId, "user_id");

	if (utils::IsValidJson(data, "roles", json::value_t::array))
	{
		for (auto &r : data["roles"])
		{
			// Accept KOOK schema: either string 'id' (Discord) or numeric 'role_id' (KOOK)
			std::string sfid;
			if (!utils::TryGetJsonValue(r, sfid, "id"))
			{
				unsigned long long rid_num = 0ULL;
				if (utils::TryGetJsonValue(r, rid_num, "role_id"))
				{
					sfid = std::to_string(rid_num);
				}
			}

			if (!sfid.empty())
			{
				auto const &role = RoleManager::Get()->FindRoleById(sfid);
				if (role)
				{
					role->Update(r);
					continue;
				}
			}

			// If role not found by id or id missing, let RoleManager create or reuse
			m_Roles.push_back(RoleManager::Get()->AddRole(r));
		}
	}

	// Also update channels if provided by KOOK /guild/view
	if (utils::IsValidJson(data, "channels", json::value_t::array))
	{
		// First, add/update channels and record their pawn ids
		for (auto &c : data["channels"])
		{
			auto const channel_id = ChannelManager::Get()->AddChannel(c, m_PawnId);
			if (channel_id == INVALID_CHANNEL_ID)
				continue;

			AddChannel(channel_id);
		}

		// After caching is done, cache Parent channel accordingly
		for (auto &c : data["channels"])
		{
			Snowflake_t id;
			if (!utils::TryGetJsonValue(c, id, "id"))
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR, "invalid JSON: expected \"id\" in \"{}\"", c.dump());
				break;
			}

			auto const &channel = ChannelManager::Get()->FindChannelById(id);
			if (channel)
			{
				if (channel->GetType() == Channel::Type::GUILD_CATEGORY)
					continue;

				Snowflake_t parent_id;
				utils::TryGetJsonValue(c, parent_id, "parent_id");
				channel->UpdateParentChannel(parent_id);
			}
		}
	}
}

void Guild::SetGuildName(std::string const &name)
{
    // Normalize and sanitize to valid UTF-8 (Windows: always convert from ACP)
    std::string safe_name = SanitizeUtf8_Guild(ToUtf8FromACP_Guild(name));
    Logger::Get()->Log(samplog_LogLevel::DEBUG, "Converted guild name from ACP to UTF-8: {}", safe_name);

    json data = {
        { "name", safe_name }
    };

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON (guild name) after forced ACP->UTF8: {}", json_str);
        return;
    }

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "guild name update request payload: {}", json_str);

    Network::Get()->Http().Patch(fmt::format("/guilds/{:s}", GetId()), json_str);
}

void Guild::SetMemberNickname(User_t const &user, std::string const &nickname)
{
    json data;
    data["guild_id"] = GetId();
    data["user_id"] = user->GetId();
    if (!nickname.empty())
    {
        // Normalize and sanitize to valid UTF-8 (Windows: always convert from ACP)
        std::string safe_nick = SanitizeUtf8_Guild(ToUtf8FromACP_Guild(nickname));
        data["nickname"] = safe_nick;
    }

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    bool member_cached = (m_MembersSet.count(user->GetPawnId()) != 0);
    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild/nickname request (cached_member: {}): {}",
        member_cached ? "yes" : "no", json_str);

    Network::Get()->Http().Post(std::string("/guild/nickname"), json_str,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild/nickname response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK set nickname failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::SetMemberVoiceChannel(User_t const &user, Snowflake_t const &channel_id)
{
	json data = {
		{ "channel_id", channel_id }
	};

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
		return;
	}

	if (m_MembersSet.count(user->GetPawnId()) == 0)
	{
		return;
	}
	

	Network::Get()->Http().Patch(fmt::format(
		"/guilds/{:s}/members/{:s}", GetId(), user->GetId()), json_str);
}

void Guild::AddMemberRole(User_t const &user, Role_t const &role)
{
    // Proceed even if member is not cached; warn and still attempt API call
    bool member_cached = (m_MembersSet.count(user->GetPawnId()) != 0);
    // if (!member_cached)
    // {
    //     Logger::Get()->Log(samplog_LogLevel::WARNING,
    //         "member not cached in guild '{}' (user pawn id: {}), still attempting to grant role",
    //         GetId(), user->GetPawnId());
    // }

    // Simple client-side rate limiting to avoid burst calls for the same (guild,user,role)
    // Window: ~500ms between identical requests
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_grant_throttle;
    const std::string throttle_key = GetId() + std::string(":") + user->GetId() + std::string(":") + role->GetId();
    auto now = std::chrono::steady_clock::now();
    auto it = s_grant_throttle.find(throttle_key);
    if (it != s_grant_throttle.end())
    {
        auto elapsed = now - it->second;
        auto min_gap = std::chrono::milliseconds(500);
        if (elapsed < min_gap)
        {
            auto sleep_dur = min_gap - elapsed;
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "throttling guild-role/grant for key '{}' by {} ms", throttle_key,
                std::chrono::duration_cast<std::chrono::milliseconds>(sleep_dur).count());
            std::this_thread::sleep_for(sleep_dur);
        }
    }
    s_grant_throttle[throttle_key] = std::chrono::steady_clock::now();

    // KOOK: POST /api/v3/guild-role/grant
    // Body: { guild_id: string, user_id: string, role_id: unsigned int }
    json data;
    data["guild_id"] = GetId();
    data["user_id"] = user->GetId();
    // role_id in KOOK is numeric; try to parse from stored string id
    try {
        unsigned long long rid = std::stoull(role->GetId());
        data["role_id"] = rid;
    } catch (...) {
        // Fallback: let JSON store as string if parsing fails (server may reject)
        data["role_id"] = role->GetId();
    }

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/grant request: payload: {} (cached_member: {})", json_str,
        member_cached ? "yes" : "no");

    // Note: no Pawn callback emitted; no need to capture pawn ids

    Network::Get()->Http().Post(std::string("/guild-role/grant"), json_str,
        [member_cached](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/grant response: status {}; body: {}; add: {}; cached_member: {}",
                r.status, r.body, r.additional_data, member_cached ? "yes" : "no");
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK grant failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::RemoveMemberRole(User_t const &user, Role_t const &role)
{
    // Proceed even if member is not cached; warn and still attempt API call
    bool member_cached = (m_MembersSet.count(user->GetPawnId()) != 0);
    if (!member_cached)
    {
        Logger::Get()->Log(samplog_LogLevel::DEBUG,
            "member not cached in guild '{}' (user pawn id: {}), still attempting to revoke role",
            GetId(), user->GetPawnId());
    }

    // Simple client-side rate limiting for revoke as well (500ms window per key)
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_revoke_throttle;
    const std::string throttle_key = GetId() + std::string(":") + user->GetId() + std::string(":") + role->GetId() + ":revoke";
    auto now = std::chrono::steady_clock::now();
    auto it = s_revoke_throttle.find(throttle_key);
    if (it != s_revoke_throttle.end())
    {
        auto elapsed = now - it->second;
        auto min_gap = std::chrono::milliseconds(500);
        if (elapsed < min_gap)
        {
            auto sleep_dur = min_gap - elapsed;
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "throttling guild-role/revoke for key '{}' by {} ms", throttle_key,
                std::chrono::duration_cast<std::chrono::milliseconds>(sleep_dur).count());
            std::this_thread::sleep_for(sleep_dur);
        }
    }
    s_revoke_throttle[throttle_key] = std::chrono::steady_clock::now();

    // KOOK: POST /api/v3/guild-role/revoke
    // Body: { guild_id: string, user_id: string, role_id: unsigned int }
    json data;
    data["guild_id"] = GetId();
    data["user_id"] = user->GetId();
    try {
        unsigned long long rid = std::stoull(role->GetId());
        data["role_id"] = rid;
    } catch (...) {
        data["role_id"] = role->GetId();
    }

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/revoke request: payload: {} (cached_member: {})", json_str,
        member_cached ? "yes" : "no");

    // Note: no Pawn callback emitted; no need to capture pawn ids

    Network::Get()->Http().Post(std::string("/guild-role/revoke"), json_str,
        [member_cached](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/revoke response: status {}; body: {}; add: {}; cached_member: {}",
                r.status, r.body, r.additional_data, member_cached ? "yes" : "no");
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK revoke failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::KickMember(User_t const &user)
{
    if (m_MembersSet.count(user->GetPawnId()) == 0)
        return;

    // KOOK: POST /api/v3/guild/kickout
    // Body: { guild_id: string, target_id: string }
    json data;
    data["guild_id"] = GetId();
    data["target_id"] = user->GetId();

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    Network::Get()->Http().Post(std::string("/guild/kickout"), json_str,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild/kickout response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK kickout failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::CreateMemberBan(User_t const &user, std::string const &reason)
{
	if (m_MembersSet.count(user->GetPawnId()) == 0)
		return;

	// KOOK: POST /api/v3/blacklist/create
	// Body: { guild_id: string, target_id: string, remark?: string, del_msg_days?: int }
	json data;
	data["guild_id"] = GetId();
	data["target_id"] = user->GetId();
	if (!reason.empty())
	{
		// Normalize and sanitize to valid UTF-8 (Windows: convert from ACP)
		std::string safe_reason = SanitizeUtf8_Guild(ToUtf8FromACP_Guild(reason));
		data["remark"] = safe_reason;
	}
	data["del_msg_days"] = 0; // default behavior similar to previous implementation

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON (blacklist/create): {}", json_str);
		return;
	}

	Network::Get()->Http().Post(std::string("/blacklist/create"), json_str,
		[](Http::Response r)
		{
			Logger::Get()->Log(samplog_LogLevel::DEBUG,
				"KOOK blacklist/create response: status {}; body: {}; add: {}",
				r.status, r.body, r.additional_data);
			if (r.status / 100 != 2)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"KOOK blacklist create failed: {} {}", r.status, r.reason);
			}
		});
}

void Guild::RemoveMemberBan(User_t const &user)
{
	if (m_MembersSet.count(user->GetPawnId()) == 0)
		return;

	// KOOK: POST /api/v3/blacklist/delete
	// Body: { guild_id: string, target_id: string }
	json data;
	data["guild_id"] = GetId();
	data["target_id"] = user->GetId();

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON (blacklist/delete): {}", json_str);
		return;
	}

	Network::Get()->Http().Post(std::string("/blacklist/delete"), json_str,
		[](Http::Response r)
		{
			Logger::Get()->Log(samplog_LogLevel::DEBUG,
				"KOOK blacklist/delete response: status {}; body: {}; add: {}",
				r.status, r.body, r.additional_data);
			if (r.status / 100 != 2)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"KOOK blacklist delete failed: {} {}", r.status, r.reason);
			}
		});
}

void Guild::SetRolePosition(Role_t const &role, int position)
{
	json data = {
		{
			{ "id", role->GetId() },
			{ "position", position }
		}
	};

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);

	Network::Get()->Http().Patch(fmt::format(
		"/guilds/{:s}/roles", GetId()), json_str);
}

template<typename T>
void GuildModifyRole(Guild *guild, Role_t const &role, const char *name, T value)
{
	json data = {
		{ name, value },
	};

	std::string json_str;
	if (!utils::TryDumpJson(data, json_str))
		Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);

	Network::Get()->Http().Patch(fmt::format(
		"/guilds/{:s}/roles/{:s}", guild->GetId(), role->GetId()), json_str);
}

void Guild::SetRoleName(Role_t const &role, std::string const &name)
{
    // KOOK: POST /api/v3/guild-role/update
    json data;
    data["guild_id"] = GetId();
    // KOOK role_id expects unsigned integer; parse from stored string when possible
    try {
        unsigned long long rid = std::stoull(role->GetId());
        data["role_id"] = rid;
    } catch (...) {
        data["role_id"] = role->GetId();
    }
    // Normalize and sanitize to valid UTF-8 (Windows: always convert from ACP)
    std::string safe_name = SanitizeUtf8_Guild(ToUtf8FromACP_Guild(name));
    data["name"] = safe_name;

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    // Note: no Pawn callback emitted; no need to capture pawn ids

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/update request (name): {}", json_str);

    Network::Get()->Http().Post(std::string("/guild-role/update"), json_str,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/update response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK guild-role/update failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::SetRolePermissions(Role_t const &role, unsigned long long permissions)
{
    // KOOK: POST /api/v3/guild-role/update
    json data;
    data["guild_id"] = GetId();
    // KOOK role_id expects unsigned integer; parse from stored string when possible
    try {
        unsigned long long rid = std::stoull(role->GetId());
        data["role_id"] = rid;
    } catch (...) {
        data["role_id"] = role->GetId();
    }
    // KOOK expects 32-bit unsigned permissions; truncate if a 64-bit value was passed
    unsigned int perms32 = static_cast<unsigned int>(permissions & 0xFFFFFFFFULL);
    data["permissions"] = perms32;

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    // Note: no Pawn callback emitted; no need to capture pawn ids

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/update request (permissions={}): {}", perms32, json_str);

    Network::Get()->Http().Post(std::string("/guild-role/update"), json_str,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/update response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK guild-role/update failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::SetRoleColor(Role_t const &role, unsigned int color)
{
    // KOOK: POST /api/v3/guild-role/update
    json data;
    data["guild_id"] = GetId();
    // KOOK role_id expects unsigned integer; parse from stored string when possible
    try {
        unsigned long long rid = std::stoull(role->GetId());
        data["role_id"] = rid;
    } catch (...) {
        data["role_id"] = role->GetId();
    }
    data["color"] = color;

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/update request (color={}): {}", color, json_str);

    Network::Get()->Http().Post(std::string("/guild-role/update"), json_str,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/update response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK guild-role/update failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::SetRoleHoist(Role_t const &role, bool hoist)
{
    // KOOK: POST /api/v3/guild-role/update
    json data;
    data["guild_id"] = GetId();
    // KOOK role_id expects unsigned integer; parse from stored string when possible
    try {
        unsigned long long rid = std::stoull(role->GetId());
        data["role_id"] = rid;
    } catch (...) {
        data["role_id"] = role->GetId();
    }
    data["hoist"] = hoist ? 1 : 0;

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/update request (hoist={}): {}", (hoist ? 1 : 0), json_str);

    Network::Get()->Http().Post(std::string("/guild-role/update"), json_str,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/update response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK guild-role/update failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::SetRoleMentionable(Role_t const &role, bool mentionable)
{
    // KOOK: POST /api/v3/guild-role/update
    json data;
    data["guild_id"] = GetId();
    // KOOK role_id expects unsigned integer; parse from stored string when possible
    try {
        unsigned long long rid = std::stoull(role->GetId());
        data["role_id"] = rid;
    } catch (...) {
        data["role_id"] = role->GetId();
    }
    data["mentionable"] = mentionable ? 1 : 0;

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/update request (mentionable={}): {}", (mentionable ? 1 : 0), json_str);

    Network::Get()->Http().Post(std::string("/guild-role/update"), json_str,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/update response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK guild-role/update failed: {} {}", r.status, r.reason);
            }
        });
}

void Guild::DeleteRole(Role_t const &role)
{
    // KOOK: POST /api/v3/guild-role/delete
    json data;
    data["guild_id"] = GetId();
    // KOOK role_id expects unsigned integer; parse from stored string when possible
    try {
        unsigned long long rid = std::stoull(role->GetId());
        data["role_id"] = rid;
    } catch (...) {
        data["role_id"] = role->GetId();
    }

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return;
    }

    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/delete request: {}", json_str);

    Network::Get()->Http().Post(std::string("/guild-role/delete"), json_str,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/delete response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK role delete failed: {} {}", r.status, r.reason);
            }
        });
}


void GuildManager::Initialize()
{
	assert(m_Initialized != m_InitValue);

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::READY, [this](json const &data)
	{
		if (!utils::IsValidJson(data, "guilds", json::value_t::array))
		{
			Logger::Get()->Log(samplog_LogLevel::FATAL,
				"invalid JSON: expected \"guilds\" in \"{}\"", data.dump());
			return;
		}

		m_InitValue += data["guilds"].size();
		m_Initialized++;
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_CREATE, [this](json const &data)
	{
		if (!m_IsInitialized)
		{
			AddGuild(data);
			m_Initialized++;
		}
		else
		{
			PawnDispatcher::Get()->Dispatch([data]() mutable
			{
				auto const guild_id = GuildManager::Get()->AddGuild(data);
				if (guild_id == INVALID_GUILD_ID)
					return;

				// forward KCC_OnGuildCreate(DCC_Guild:guild);
				pawn_cb::Error error;
				pawn_cb::Callback::CallFirst(error, "KCC_OnGuildCreate", guild_id);
			});
		}
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_DELETE, [](json const &data)
	{
		Snowflake_t sfid;
		if (!utils::TryGetJsonValue(data, sfid, "id"))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"id\" in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([sfid]() mutable
		{
			Guild_t const &guild = GuildManager::Get()->FindGuildById(sfid);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::WARNING,
					"can't delete guild: guild id \"{}\" not cached", sfid);
				return;
			}

			// forward KCC_OnGuildDelete(DCC_Guild:guild);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildDelete", guild->GetPawnId());

			GuildManager::Get()->DeleteGuild(guild);
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_UPDATE, [](json const &data)
	{
		Snowflake_t sfid;
		if (!utils::TryGetJsonValue(data, sfid, "id"))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"id\" in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([data, sfid]() mutable
		{
			Guild_t const &guild = GuildManager::Get()->FindGuildById(sfid);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild: guild id \"{}\" not cached", sfid);
				return;
			}

			guild->Update(data);

			// forward KCC_OnGuildUpdate(DCC_Guild:guild);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildUpdate", guild->GetPawnId());
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_MEMBER_ADD, [](json const &data)
	{
		if (!utils::IsValidJson(data,
			"guild_id", json::value_t::string,
			"user", json::value_t::object,
			"roles", json::value_t::array))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"guild_id\", \"user\" and \"roles\" " \
				"in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			Snowflake_t sfid = data["guild_id"].get<std::string>();
			auto const &guild = GuildManager::Get()->FindGuildById(sfid);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't add guild member: guild id \"{}\" not cached", sfid);
				return;
			}

			Guild::Member member;
			// returns correct user if he already exists
			auto const userid = UserManager::Get()->AddUser(data["user"]);
			member.UserId = userid;
			member.Update(data);

			guild->AddMember(std::move(member));

			// forward KCC_OnGuildMemberAdd(DCC_Guild:guild, DCC_User:user);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildMemberAdd", guild->GetPawnId(), userid);
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_MEMBER_REMOVE, [](json const &data)
	{
		if (!utils::IsValidJson(data,
			"guild_id", json::value_t::string,
			"user", "id", json::value_t::string))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"guild_id\" and \"user.id\" in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			Snowflake_t guild_id = data["guild_id"].get<std::string>();
			auto const &guild = GuildManager::Get()->FindGuildById(guild_id);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't remove guild member: guild id \"{}\" not cached", guild_id);
				return;
			}

			Snowflake_t user_id = data["user"]["id"].get<std::string>();
			auto const &user = UserManager::Get()->FindUserById(user_id);
			if (!user)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't remove guild member: user id \"{}\" not cached", user_id);
				return;
			}

			// forward KCC_OnGuildMemberRemove(DCC_Guild:guild, DCC_User:user);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildMemberRemove", guild->GetPawnId(), user->GetPawnId());

			guild->RemoveMember(user->GetPawnId());
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_MEMBER_UPDATE, [](json const &data)
	{
		if (!utils::IsValidJson(data,
			"guild_id", json::value_t::string,
			"user", "id", json::value_t::string))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"guild_id\" and \"user.id\" in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			Snowflake_t guild_id = data["guild_id"].get<std::string>();
			auto const &guild = GuildManager::Get()->FindGuildById(guild_id);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild member: guild id \"{}\" not cached", guild_id);
				return;
			}

			Snowflake_t user_id = data["user"]["id"].get<std::string>();
			auto const &user = UserManager::Get()->FindUserById(user_id);
			if (!user)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild member: user id \"{}\" not cached", user_id);
				return;
			}

			guild->UpdateMember(user->GetPawnId(), data);
			user->Update(data["user"]);
			// forward KCC_OnGuildMemberUpdate(DCC_Guild:guild, DCC_User:user);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildMemberUpdate", guild->GetPawnId(), user->GetPawnId());
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_ROLE_CREATE, [](json const &data)
	{
		if (!utils::IsValidJson(data,
			"guild_id", json::value_t::string,
			"role", json::value_t::object))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"guild_id\" and \"role\" in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			Snowflake_t guild_id = data["guild_id"].get<std::string>();
			auto const &guild = GuildManager::Get()->FindGuildById(guild_id);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't add guild role: guild id \"{}\" not cached", guild_id);
				return;
			}

			auto const role = RoleManager::Get()->AddRole(data["role"]);
			guild->AddRole(role);

			// forward KCC_OnGuildRoleCreate(DCC_Guild:guild, DCC_Role:role);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildRoleCreate", guild->GetPawnId(), role);
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_ROLE_DELETE, [](json const &data)
	{
		if (!utils::IsValidJson(data,
			"guild_id", json::value_t::string,
			"role_id", json::value_t::string))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"guild_id\" and \"role_id\" in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			Snowflake_t guild_id = data["guild_id"].get<std::string>();
			auto const &guild = GuildManager::Get()->FindGuildById(guild_id);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't delete guild role: guild id \"{}\" not cached", guild_id);
				return;
			}

			Snowflake_t role_id = data["role_id"].get<std::string>();
			auto const &role = RoleManager::Get()->FindRoleById(role_id);
			if (!role)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't delete guild role: role id \"{}\" not cached", role_id);
				return;
			}

			// forward KCC_OnGuildRoleDelete(DCC_Guild:guild, DCC_Role:role);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildRoleDelete", guild->GetPawnId(), role->GetPawnId());

			guild->RemoveRole(role->GetPawnId());
			RoleManager::Get()->RemoveRole(role);
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_ROLE_UPDATE, [](json const &data)
	{
		if (!utils::IsValidJson(data,
			"guild_id", json::value_t::string,
			"role", "id", json::value_t::string))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"guild_id\" and \"role.id\" in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			Snowflake_t guild_id = data["guild_id"].get<std::string>();
			auto const &guild = GuildManager::Get()->FindGuildById(guild_id);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild role: guild id \"{}\" not cached", guild_id);
				return;
			}

			Snowflake_t role_id = data["role"]["id"].get<std::string>();
			auto const &role = RoleManager::Get()->FindRoleById(role_id);
			if (!role)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild role: role id \"{}\" not cached", role_id);
				return;
			}

			role->Update(data["role"]);

			// forward KCC_OnGuildRoleUpdate(DCC_Guild:guild, DCC_Role:role);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildRoleUpdate", guild->GetPawnId(), role->GetPawnId());
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::PRESENCE_UPDATE, [](json const &data)
	{
		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			std::string guild_id;
			if (!utils::TryGetJsonValue(data, guild_id, "guild_id"))
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"guild_id\" in \"{}\"", data.dump());
				return;
			}

			std::string user_id;
			if (!utils::TryGetJsonValue(data, user_id, "user", "id"))
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"user.id\" in \"{}\"", data.dump());
				return;
			}

			std::string status;
			if (!utils::TryGetJsonValue(data, status, "status"))
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"invalid JSON: expected \"status\" in \"{}\"", data.dump());
				return;
			}

			auto const &guild = GuildManager::Get()->FindGuildById(guild_id);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild member presence: guild id \"{}\" not cached", guild_id);
				return;
			}

			auto const &user = UserManager::Get()->FindUserById(user_id);
			if (!user)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild member presence: user id \"{}\" not cached", user_id);
				return;
			}

			guild->UpdateMemberPresence(user->GetPawnId(), status);

			// forward KCC_OnGuildMemberUpdate(DCC_Guild:guild, DCC_User:user);
			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildMemberUpdate", guild->GetPawnId(), user->GetPawnId());
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::GUILD_MEMBERS_CHUNK, [](json const &data)
	{
		Snowflake_t guild_id;
		if (!utils::TryGetJsonValue(data, guild_id, "guild_id"))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"guild_id\" in \"{}\"", data.dump());
			return;
		}

		if (!utils::IsValidJson(data, "members", json::value_t::array))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected array \"members\" in \"{}\"", data.dump());
		}

		PawnDispatcher::Get()->Dispatch([guild_id, data]() mutable
		{
			auto const &guild = GuildManager::Get()->FindGuildById(guild_id);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't sync offline guild members: guild id \"{}\" not cached", guild_id);
				return;
			}

			for (auto &m : data["members"])
			{
				if (!utils::IsValidJson(m, "user", json::value_t::object))
				{
					// we break here because all other array entries are likely
					// to be invalid too, and we don't want to spam an error message
					// for every single element in this array
					Logger::Get()->Log(samplog_LogLevel::ERROR,
						"invalid JSON: expected \"user\" in \"{}\"", m.dump());
					break;
				}

				Guild::Member member;
				// returns correct user if he already exists
				auto const userid = UserManager::Get()->AddUser(m["user"]);
				member.UserId = userid;
				member.Update(m);

				guild->AddMember(std::move(member));
			}
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::VOICE_STATE_UPDATE, [](json const &data)
	{
		if (!utils::IsValidJson(data,
			"guild_id", json::value_t::string,
			"user_id", json::value_t::string,
			"channel_id", json::value_t::string, json::value_t::null))
		{
			Logger::Get()->Log(samplog_LogLevel::ERROR,
				"invalid JSON: expected \"guild_id\", \"user_id\" and \"channel_id\"in \"{}\"", data.dump());
			return;
		}

		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			Snowflake_t const &guild_id = data["guild_id"];
			auto const &guild = GuildManager::Get()->FindGuildById(guild_id);
			if (!guild)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild member voice channel: guild id \"{}\" not cached", guild_id);
				return;
			}

			Snowflake_t const &user_id = data["user_id"];
			auto const &user = UserManager::Get()->FindUserById(user_id);
			if (!user)
			{
				Logger::Get()->Log(samplog_LogLevel::ERROR,
					"can't update guild member voice channel: user id \"{}\" not cached", user_id);
				return;
			}
			
			auto const &channel_id = data["channel_id"];
			ChannelId_t channel_PawnId = INVALID_CHANNEL_ID;			
			if (!channel_id.is_null()) // User joined voice channel, thus check if channel is cached and get its pawnId
			{
				auto const &channel = ChannelManager::Get()->FindChannelById(channel_id);
				if (!channel)
				{
					Logger::Get()->Log(samplog_LogLevel::ERROR,
						"can't update guild member voice channel: channel id \"{}\" not cached", channel_id.get<Snowflake_t>());
					return;
				}
				channel_PawnId = channel->GetPawnId();
			}
			guild->UpdateMemberVoiceChannel(user->GetPawnId(), channel_PawnId);

			pawn_cb::Error error;
			pawn_cb::Callback::CallFirst(error, "KCC_OnGuildMemberVoiceUpdate", guild->GetPawnId(), user->GetPawnId(), channel_PawnId);
		});
	});
	// TODO: events
}

bool GuildManager::IsInitialized()
{
	if (!m_IsInitialized && m_Initialized == m_InitValue)
		m_IsInitialized = true;

	return m_IsInitialized;
}

bool GuildManager::CreateGuildRole(Guild_t const &guild,
    std::string const &name, pawn_cb::Callback_t &&cb)
{
    // KOOK: POST /api/v3/guild-role/create
    json data;
    data["guild_id"] = guild->GetId();
    if (!name.empty())
    {
        // Normalize and sanitize to valid UTF-8 (Windows: always convert from ACP)
        data["name"] = SanitizeUtf8_Guild(ToUtf8FromACP_Guild(name));
    }

    std::string json_str;
    if (!utils::TryDumpJson(data, json_str))
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "can't serialize JSON: {}", json_str);
        return false;
    }

    auto guild_pawn_id = guild->GetPawnId();
    Logger::Get()->Log(samplog_LogLevel::DEBUG,
        "KOOK guild-role/create request: {}", json_str);

    Network::Get()->Http().Post(std::string("/guild-role/create"), json_str,
        [this, cb, guild_pawn_id](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::DEBUG,
                "KOOK guild-role/create response: status {}; body: {}; add: {}",
                r.status, r.body, r.additional_data);
            if (r.status / 100 != 2)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR,
                    "KOOK role create failed: {} {}", r.status, r.reason);
                return;
            }

            // Parse KOOK response: { code, message, data: [ { role fields... } ] }
            json j = json::parse(r.body, nullptr, false);
            if (j.is_discarded()) return;
            json role_obj;
            if (j.is_object())
            {
                auto it_data = j.find("data");
                if (it_data != j.end())
                {
                    if (it_data->is_array() && !it_data->empty())
                        role_obj = (*it_data)[0];
                    else if (it_data->is_object())
                        role_obj = *it_data;
                }
            }
            else if (j.is_array() && !j.empty())
            {
                role_obj = j[0];
            }
            if (!role_obj.is_object())
                return;

            auto const &guild = GuildManager::Get()->FindGuild(guild_pawn_id);
            if (!guild)
            {
                Logger::Get()->Log(samplog_LogLevel::ERROR, "lost cached guild between network calls");
                return;
            }

            auto const role = RoleManager::Get()->AddRole(role_obj);
            guild->AddRole(role);

            if (cb)
            {
                PawnDispatcher::Get()->Dispatch([=]()
                {
                    m_CreatedRoleId = role;
                    cb->Execute();
                    m_CreatedRoleId = INVALID_ROLE_ID;
                });
            }
        });

    return true;
}

GuildId_t GuildManager::AddGuild(json const &data)
{
	Snowflake_t sfid;
	if (!utils::TryGetJsonValue(data, sfid, "id"))
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"invalid JSON: expected \"id\" in \"{}\"", data.dump());
		return INVALID_GUILD_ID;
	}

	auto const &guild = FindGuildById(sfid);
	if (guild)
	{
		// guild already exists
		// we don't log an error here because this function can be called on
		// existing guilds after a gateway reconnect
		return INVALID_GUILD_ID;
	}

	GuildId_t id = 1;
	while (m_Guilds.find(id) != m_Guilds.end())
		++id;

	if (!m_Guilds.emplace(id, Guild_t(new Guild(id, data))).second)
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"can't create guild: duplicate key '{}'", id);
		return INVALID_GUILD_ID;
	}

	Logger::Get()->Log(samplog_LogLevel::INFO, "successfully created guild with id '{}'", id);
	return id;
}

GuildId_t GuildManager::AddGuildById(Snowflake_t const &sfid)
{
    auto const &existing = FindGuildById(sfid);
    if (existing)
        return existing->GetPawnId();

    json j;
    j["id"] = sfid;

    GuildId_t id = 1;
    while (m_Guilds.find(id) != m_Guilds.end())
        ++id;

    if (!m_Guilds.emplace(id, Guild_t(new Guild(id, j))).second)
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR,
            "can't create placeholder guild: duplicate key '{}'", id);
        return INVALID_GUILD_ID;
    }

    Logger::Get()->Log(samplog_LogLevel::INFO, "successfully created placeholder guild with id '{}'", id);
    return id;
}

void GuildManager::DeleteGuild(Guild_t const &guild)
{
	m_Guilds.erase(guild->GetPawnId());
}

std::vector<GuildId_t> GuildManager::GetAllGuildIds() const
{
	std::vector<GuildId_t> guild_ids;
	for (auto &g : m_Guilds)
		guild_ids.push_back(g.first);

	return guild_ids;
}

Guild_t const &GuildManager::FindGuild(GuildId_t id)
{
	static Guild_t invalid_guild;
	auto it = m_Guilds.find(id);
	if (it == m_Guilds.end())
		return invalid_guild;
	return it->second;
}

Guild_t const &GuildManager::FindGuildByName(std::string const &name)
{
	static Guild_t invalid_guild;
	for (auto const &g : m_Guilds)
	{
		Guild_t const &guild = g.second;
		if (guild->GetName().compare(name) == 0)
			return guild;
	}
	return invalid_guild;
}

Guild_t const &GuildManager::FindGuildById(Snowflake_t const &sfid)
{
	static Guild_t invalid_guild;
	for (auto const &g : m_Guilds)
	{
		Guild_t const &guild = g.second;
		if (guild->GetId().compare(sfid) == 0)
			return guild;
	}
	return invalid_guild;
}