#include "Message.hpp"
#include "User.hpp"
#include "Channel.hpp"
#include "Role.hpp"
#include "Guild.hpp"
#include "Network.hpp"
#include "PawnDispatcher.hpp"
#include "Callback.hpp"
#include "Logger.hpp"
#include "utils.hpp"
#include "Emoji.hpp"
#include "Embed.hpp"
#include <regex>
#include <string>
#include <unordered_map>
#include "emoji.h"

// Convert ":shortcode:" to UTF-8 emoji (common subset; extend as needed)
static std::string EmojiShortcodeToUnicode(const std::string& sc)
{
    if (sc.size() < 3 || sc.front() != ':' || sc.back() != ':') return std::string();
    // First, try the large built-in table from emoji.h
    {
        auto it = emojicpp::EMOJIS.find(sc);
        if (it != emojicpp::EMOJIS.end()) return it->second;
    }
    return std::string();
}

Message::Message(MessageId_t pawn_id, json const &data) : m_PawnId(pawn_id)
{
    // KOOK event payload support
    // Incoming may be { s: 0, d: { ... } } or just { ... }
    json payload = data;
    int s_flag = -1;
    if (utils::TryGetJsonValue(data, s_flag, "s") && s_flag == 0 && utils::IsValidJson(data, "d", json::value_t::object))
    {
        payload = data["d"];
    }

    std::string channel_type;
    int msg_type = 0;
    std::string target_id;
    std::string author_id;

    // Required KOOK fields for a message event
    _valid =
        utils::TryGetJsonValue(payload, channel_type, "channel_type") &&
        utils::TryGetJsonValue(payload, msg_type, "type") &&
        utils::TryGetJsonValue(payload, target_id, "target_id") &&
        utils::TryGetJsonValue(payload, author_id, "author_id") &&
        utils::TryGetJsonValue(payload, m_Content, "content") &&
        utils::TryGetJsonValue(payload, m_Id, "msg_id");

    if (!_valid)
    {
        // Fallback to legacy Discord message parsing if this isn't a KOOK event
        std::string legacy_author_id, legacy_channel_id, legacy_guild_id;
        _valid =
            utils::TryGetJsonValue(data, m_Id, "id") &&
            utils::TryGetJsonValue(data, legacy_author_id, "author", "id") &&
            utils::TryGetJsonValue(data, legacy_channel_id, "channel_id") &&
            utils::TryGetJsonValue(data, m_Content, "content");

        // Defaults for legacy
        utils::TryGetJsonValue(data, m_IsTts, "tts");
        utils::TryGetJsonValue(data, m_MentionsEveryone, "mention_everyone");

        if (!_valid)
        {
            Logger::Get()->Log(samplog_LogLevel::ERROR,
                "can't construct message object: invalid JSON: \"{}\"", data.dump());
            return;
        }

        // KMarkdown: parse (rol)ROLE_ID(rol) patterns from content
        if (!m_Content.empty())
        {
            // Determine guild id for role resolution
            GuildId_t gid_local = INVALID_GUILD_ID;
            if (m_Channel != INVALID_CHANNEL_ID)
            {
                Channel_t const &ch_for_gid = ChannelManager::Get()->FindChannel(m_Channel);
                if (ch_for_gid) gid_local = ch_for_gid->GetGuildId();
            }

            try {
                std::regex rol_re("\\(rol\\)(\\d+)\\(rol\\)");
                auto begin = std::sregex_iterator(m_Content.begin(), m_Content.end(), rol_re);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it)
                {
                    std::string rid = (*it)[1].str();
                    if (rid.empty()) continue;

                    bool added = false;
                    Role_t const &mr0 = RoleManager::Get()->FindRoleById(rid);
                    if (mr0)
                    {
                        m_RoleMentions.push_back(mr0->GetPawnId());
                        added = true;
                    }
                    else if (gid_local != INVALID_GUILD_ID)
                    {
                        // Fetch guild roles and populate cache if missing
                        std::string gid_str;
                        Guild_t const &g = GuildManager::Get()->FindGuild(gid_local);
                        if (g) gid_str = g->GetId();
                        if (!gid_str.empty())
                        {
                            Network::Get()->Http().Get(std::string("/guild-role/list?guild_id=") + gid_str,
                                [](Http::Response r)
                                {
                                    if (r.status / 100 == 2)
                                    {
                                        json j = json::parse(r.body, nullptr, false);
                                        if (j.is_object())
                                        {
                                            auto it_data = j.find("data");
                                            if (it_data != j.end() && it_data->is_object())
                                            {
                                                auto it_items = it_data->find("items");
                                                if (it_items != it_data->end() && it_items->is_array())
                                                {
                                                    for (auto &item : *it_items)
                                                    {
                                                        if (!item.is_object()) continue;
                                                        json role_json;
                                                        auto it_rid2 = item.find("role_id");
                                                        if (it_rid2 == item.end()) continue;
                                                        try {
                                                            auto role_id_val = *it_rid2;
                                                            role_json["id"] = role_id_val.is_string() ? role_id_val.get<std::string>() : std::to_string(role_id_val.get<unsigned long long>());
                                                        } catch (...) { continue; }
                                                        role_json["name"] = item.value("name", "");
                                                        role_json["color"] = item.value("color", 0u);
                                                        role_json["position"] = item.value("position", 0u);
                                                        role_json["hoist"] = item.value("hoist", 0u) != 0u;
                                                        role_json["mentionable"] = item.value("mentionable", 0u) != 0u;
                                                        unsigned long long perms = 0ULL;
                                                        try { perms = item.value("permissions", 0u); } catch (...) { perms = 0ULL; }
                                                        role_json["permissions"] = std::to_string(perms);
                                                        RoleManager::Get()->AddRole(role_json);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                });
                            for (int i = 0; i < 10; ++i)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                Role_t const &mr1 = RoleManager::Get()->FindRoleById(rid);
                                if (mr1)
                                {
                                    m_RoleMentions.push_back(mr1->GetPawnId());
                                    added = true;
                                    break;
                                }
                            }
                        }
                    }
                    // if still not added, skip
                }
            } catch (...) {
                // regex failure ignored
            }
        }

        // Card message: content may be a JSON array of cards; extract kmarkdown texts and scan for (rol)/(met)
        if (!m_Content.empty())
        {
            json card;
            try { card = json::parse(m_Content, nullptr, false); } catch (...) { card = nullptr; }
            if (card.is_array())
            {
                // Prepare helpers
                auto ensure_role = [&](const std::string &rid, GuildId_t gid_local)
                {
                    if (rid.empty()) return;
                    Role_t const &mr0 = RoleManager::Get()->FindRoleById(rid);
                    if (mr0) { m_RoleMentions.push_back(mr0->GetPawnId()); return; }
                    if (gid_local == INVALID_GUILD_ID) return;
                    // fetch roles list
                    Guild_t const &g = GuildManager::Get()->FindGuild(gid_local);
                    std::string gid_str = g ? g->GetId() : std::string();
                    if (gid_str.empty()) return;
                    Network::Get()->Http().Get(std::string("/guild-role/list?guild_id=") + gid_str,
                        [](Http::Response r)
                        {
                            if (r.status / 100 == 2)
                            {
                                json j = json::parse(r.body, nullptr, false);
                                if (j.is_object())
                                {
                                    auto it_data = j.find("data");
                                    if (it_data != j.end() && it_data->is_object())
                                    {
                                        auto it_items = it_data->find("items");
                                        if (it_items != it_data->end() && it_items->is_array())
                                        {
                                            for (auto &item : *it_items)
                                            {
                                                if (!item.is_object()) continue;
                                                json role_json;
                                                auto it_rid2 = item.find("role_id");
                                                if (it_rid2 == item.end()) continue;
                                                try {
                                                    auto role_id_val = *it_rid2;
                                                    role_json["id"] = role_id_val.is_string() ? role_id_val.get<std::string>() : std::to_string(role_id_val.get<unsigned long long>());
                                                } catch (...) { continue; }
                                                role_json["name"] = item.value("name", "");
                                                role_json["color"] = item.value("color", 0u);
                                                role_json["position"] = item.value("position", 0u);
                                                role_json["hoist"] = item.value("hoist", 0u) != 0u;
                                                role_json["mentionable"] = item.value("mentionable", 0u) != 0u;
                                                unsigned long long perms = 0ULL;
                                                try { perms = item.value("permissions", 0u); } catch (...) { perms = 0ULL; }
                                                role_json["permissions"] = std::to_string(perms);
                                                RoleManager::Get()->AddRole(role_json);
                                            }
                                        }
                                    }
                                }
                            }
                        });
                    for (int i = 0; i < 10; ++i)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        Role_t const &mr1 = RoleManager::Get()->FindRoleById(rid);
                        if (mr1) { m_RoleMentions.push_back(mr1->GetPawnId()); return; }
                    }
                };

                auto ensure_user = [&](const std::string &uid)
                {
                    if (uid.empty()) return;
                    User_t const &mu0 = UserManager::Get()->FindUserById(uid);
                    if (mu0) { m_UserMentions.push_back(mu0->GetPawnId()); return; }
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
                        User_t const &mu1 = UserManager::Get()->FindUserById(uid);
                        if (mu1) { m_UserMentions.push_back(mu1->GetPawnId()); return; }
                    }
                };

                // Gather guild id
                GuildId_t gid_local = INVALID_GUILD_ID;
                if (m_Channel != INVALID_CHANNEL_ID)
                {
                    Channel_t const &ch_for_gid = ChannelManager::Get()->FindChannel(m_Channel);
                    if (ch_for_gid) gid_local = ch_for_gid->GetGuildId();
                }

                std::regex rol_re("\\(rol\\)(\\d+)\\(rol\\)");
                std::regex met_re("\\(met\\)([0-9a-zA-Z]+|all|here)\\(met\\)");

                auto scan_kmd = [&](const std::string &kmd)
                {
                    // roles
                    for (auto it = std::sregex_iterator(kmd.begin(), kmd.end(), rol_re); it != std::sregex_iterator(); ++it)
                    {
                        ensure_role((*it)[1].str(), gid_local);
                    }
                    // users (skip here/all)
                    for (auto it = std::sregex_iterator(kmd.begin(), kmd.end(), met_re); it != std::sregex_iterator(); ++it)
                    {
                        std::string uid = (*it)[1].str();
                        if (uid == "all" || uid == "here") continue;
                        ensure_user(uid);
                    }
                };

                // Traverse cards -> modules -> text(s)
                for (auto &cardItem : card)
                {
                    if (!cardItem.is_object()) continue;
                    auto it_modules = cardItem.find("modules");
                    if (it_modules == cardItem.end() || !it_modules->is_array()) continue;
                    for (auto &mod : *it_modules)
                    {
                        if (!mod.is_object()) continue;
                        // section.text, header.text, etc.
                        auto it_text = mod.find("text");
                        if (it_text != mod.end() && it_text->is_object())
                        {
                            std::string ttype = it_text->value("type", "");
                            if (ttype == "kmarkdown")
                            {
                                std::string content = it_text->value("content", "");
                                scan_kmd(content);
                            }
                            else if (ttype == "paragraph")
                            {
                                auto it_fields = it_text->find("fields");
                                if (it_fields != it_text->end() && it_fields->is_array())
                                {
                                    for (auto &fld : *it_fields)
                                    {
                                        if (!fld.is_object()) continue;
                                        if (fld.value("type", "") == std::string("kmarkdown"))
                                        {
                                            std::string content = fld.value("content", "");
                                            scan_kmd(content);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Channel_t const &legacy_channel = ChannelManager::Get()->FindChannelById(legacy_channel_id);
        if (!legacy_channel && !utils::TryGetJsonValue(data, legacy_guild_id, "guild_id"))
        {
            ChannelId_t cid = ChannelManager::Get()->AddDMChannel(data);
            m_Channel = ChannelManager::Get()->FindChannel(cid)->GetPawnId();
        }
        else
        {
            m_Channel = legacy_channel ? legacy_channel->GetPawnId() : INVALID_CHANNEL_ID;
        }

        User_t const &legacy_user = UserManager::Get()->FindUserById(legacy_author_id);
        m_Author = legacy_user ? legacy_user->GetPawnId() : INVALID_USER_ID;
    }
    else
    {
        // Map KOOK fields
        m_IsTts = false;
        m_MentionsEveryone = false;

        // Fill author
        // Prefer extra.author.id if present
        std::string extra_author_id;
        if (utils::TryGetJsonValue(payload, extra_author_id, "extra", "author", "id"))
        {
            author_id = extra_author_id;
        }

        // Ensure author exists in cache; if not, try to add from extra.author
        User_t const &user0 = UserManager::Get()->FindUserById(author_id);
        if (!user0)
        {
            if (utils::IsValidJson(payload, "extra", json::value_t::object) &&
                utils::IsValidJson(payload["extra"], "author", json::value_t::object))
            {
                // KOOK provides a full author object here (id, username, identify_num, bot, verified ...)
                UserManager::Get()->AddUser(payload["extra"]["author"]);
            }
        }
        User_t const &user = UserManager::Get()->FindUserById(author_id);
        m_Author = user ? user->GetPawnId() : INVALID_USER_ID;

        // Mentions and mention flags from extra
        utils::TryGetJsonValue(payload, m_MentionsEveryone, "extra", "mention_all");

        // Resolve channel early so that guild id is available for role resolution
        Channel_t const &channel_early = ChannelManager::Get()->FindChannelById(target_id);
        if (channel_early)
        {
            m_Channel = channel_early->GetPawnId();
        }
        else if (channel_type == "PERSON")
        {
            // Create a lightweight DM channel cache entry with provided target_id
            json dm_init;
            dm_init["channel_id"] = target_id;
            ChannelId_t cid = ChannelManager::Get()->AddDMChannel(dm_init);
            Channel_t const &created = ChannelManager::Get()->FindChannel(cid);
            m_Channel = created ? created->GetPawnId() : INVALID_CHANNEL_ID;
        }
        else
        {
            // For GROUP/BROADCAST without channel cached, create a placeholder to allow message sending
            // This fixes the "invalid channel id '0'" error when channels haven't synced yet
            ChannelId_t cid = ChannelManager::Get()->AddChannelById(target_id,
                channel_type == "CATEGORY" ? Channel::Type::GUILD_CATEGORY :
                channel_type == "VOICE" ? Channel::Type::GUILD_VOICE :
                Channel::Type::GUILD_TEXT);
            Channel_t const &created = ChannelManager::Get()->FindChannel(cid);
            m_Channel = created ? created->GetPawnId() : INVALID_CHANNEL_ID;
        }

        // mention (users): can be array of strings ["uid1", ...] or objects with { id }
        if (utils::IsValidJson(payload, "extra", json::value_t::object) &&
            utils::IsValidJson(payload["extra"], "mention", json::value_t::array))
        {
            for (auto &v : payload["extra"]["mention"]) {
                std::string uid;
                if (v.is_string()) {
                    uid = v.get<std::string>();
                } else if (v.is_object()) {
                    utils::TryGetJsonValue(v, uid, "id");
                }
                if (uid.empty()) continue;

                // If not cached, try to fetch from KOOK and short-wait
                User_t const &mu0 = UserManager::Get()->FindUserById(uid);
                if (!mu0)
                {
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
                        User_t const &mu1 = UserManager::Get()->FindUserById(uid);
                        if (mu1)
                        {
                            m_UserMentions.push_back(mu1->GetPawnId());
                            goto mention_next; // continue outer loop
                        }
                    }
                    // If still not available, skip adding for now
                }
                else
                {
                    m_UserMentions.push_back(mu0->GetPawnId());
                }
            mention_next:;
            }
        }

        // mention_roles: array of strings or objects with id
        if (utils::IsValidJson(payload, "extra", json::value_t::object) &&
            utils::IsValidJson(payload["extra"], "mention_roles", json::value_t::array))
        {
            // Determine guild id from resolved channel, if available
            GuildId_t guild_id_for_roles = INVALID_GUILD_ID;
            if (m_Channel != INVALID_CHANNEL_ID)
            {
                Channel_t const &ch_for_gid = ChannelManager::Get()->FindChannel(m_Channel);
                if (ch_for_gid)
                    guild_id_for_roles = ch_for_gid->GetGuildId();
            }

            for (auto &v : payload["extra"]["mention_roles"]) {
                std::string rid;
                if (v.is_string()) {
                    rid = v.get<std::string>();
                } else if (v.is_number_integer() || v.is_number_unsigned() || v.is_number()) {
                    try { rid = std::to_string(v.get<unsigned long long>()); } catch (...) {
                        try { rid = std::to_string(v.get<long long>()); } catch (...) {}
                    }
                } else if (v.is_object()) {
                    utils::TryGetJsonValue(v, rid, "id");
                }
                if (rid.empty()) continue;

                bool added = false;
                Role_t const &mr0 = RoleManager::Get()->FindRoleById(rid);
                if (mr0)
                {
                    m_RoleMentions.push_back(mr0->GetPawnId());
                    added = true;
                }
                else if (guild_id_for_roles != INVALID_GUILD_ID)
                {
                    // Try to fetch guild roles list and populate cache
                    Channel_t const &ch_tmp = ChannelManager::Get()->FindChannel(m_Channel);
                    std::string gid_str;
                    if (ch_tmp)
                    {
                        GuildId_t gid_local = ch_tmp->GetGuildId();
                        if (gid_local != INVALID_GUILD_ID)
                        {
                            Guild_t const &g = GuildManager::Get()->FindGuild(gid_local);
                            if (g) gid_str = g->GetId();
                        }
                    }
                    if (!gid_str.empty())
                    {
                        Network::Get()->Http().Get(std::string("/guild-role/list?guild_id=") + gid_str,
                            [](Http::Response r)
                            {
                                if (r.status / 100 == 2)
                                {
                                    json j = json::parse(r.body, nullptr, false);
                                    if (j.is_object())
                                    {
                                        auto it_data = j.find("data");
                                        if (it_data != j.end() && it_data->is_object())
                                        {
                                            auto it_items = it_data->find("items");
                                            if (it_items != it_data->end() && it_items->is_array())
                                            {
                                                for (auto &item : *it_items)
                                                {
                                                    if (!item.is_object()) continue;
                                                    json role_json;
                                                    // id is stringified role_id
                                                    auto it_rid = item.find("role_id");
                                                    if (it_rid == item.end()) continue;
                                                    try {
                                                        auto role_id_val = *it_rid;
                                                        role_json["id"] = role_id_val.is_string() ? role_id_val.get<std::string>() : std::to_string(role_id_val.get<unsigned long long>());
                                                    } catch (...) { continue; }
                                                    role_json["name"] = item.value("name", "");
                                                    role_json["color"] = item.value("color", 0u);
                                                    role_json["position"] = item.value("position", 0u);
                                                    role_json["hoist"] = item.value("hoist", 0u) != 0u;
                                                    role_json["mentionable"] = item.value("mentionable", 0u) != 0u;
                                                    unsigned long long perms = 0ULL;
                                                    try { perms = item.value("permissions", 0u); } catch (...) { perms = 0ULL; }
                                                    role_json["permissions"] = std::to_string(perms);
                                                    RoleManager::Get()->AddRole(role_json);
                                                }
                                            }
                                        }
                                    }
                                }
                            });
                        for (int i = 0; i < 10; ++i)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            Role_t const &mr1 = RoleManager::Get()->FindRoleById(rid);
                            if (mr1)
                            {
                                m_RoleMentions.push_back(mr1->GetPawnId());
                                added = true;
                                break;
                            }
                        }
                    }
                }
                // if still not added, skip gracefully
            }
        }

        // channel already resolved above
    }
}

void Message::DeleteMessage()
{
    // KOOK: delete message via POST /api/v3/message/delete with msg_id
    json body = {
        { "msg_id", GetId() }
    };
    std::string payload;
    if (!utils::TryDumpJson(body, payload))
        payload = "{\"msg_id\":\"" + GetId() + "\"}";
    Network::Get()->Http().Post("/message/delete", payload);
}

void Message::AddReaction(Emoji_t const& emoji)
{
    // KOOK: POST /api/v3/message/add-reaction { msg_id, emoji }
    std::string emoji_str = emoji->GetSnowflake();
    if (emoji_str.empty())
    {
        emoji_str = emoji->GetName(); // 期望：Unicode 字符 或 :shortcode:
        if (!emoji_str.empty() && emoji_str.front() == ':' && emoji_str.back() == ':')
        {
            std::string converted = EmojiShortcodeToUnicode(emoji_str);
            if (!converted.empty()) emoji_str = converted;
        }
    }

    json body = {
        { "msg_id", GetId() },
        { "emoji", emoji_str }
    };
    std::string payload;
    if (!utils::TryDumpJson(body, payload))
        payload = "{\"msg_id\":\"" + GetId() + "\",\"emoji\":\"" + emoji_str + "\"}";
    Network::Get()->Http().Post("/message/add-reaction", payload,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::INFO,
                "KOOK add-reaction response: {} {} | {}",
                r.status, r.reason, r.body);
        });
}

bool Message::DeleteReaction(EmojiId_t const emojiid)
{
    // KOOK: POST /api/v3/message/delete-reaction { msg_id, emoji[, user_id] }
    if (emojiid == INVALID_EMOJI_ID)
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "KOOK delete-reaction requires an emoji id; deleting all reactions is not supported via this endpoint");
        return false;
    }

    const auto& emoji = EmojiManager::Get()->FindEmoji(emojiid);
    if (!emoji)
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR, "invalid emoji id '{}'", emojiid);
        return false;
    }

    std::string emoji_str = emoji->GetSnowflake();
    if (emoji_str.empty())
    {
        emoji_str = emoji->GetName();
        if (!emoji_str.empty() && emoji_str.front() == ':' && emoji_str.back() == ':')
        {
            std::string converted = EmojiShortcodeToUnicode(emoji_str);
            if (!converted.empty()) emoji_str = converted;
        }
    }

    json body = {
        { "msg_id", GetId() },
        { "emoji", emoji_str }
    };
    std::string payload;
    if (!utils::TryDumpJson(body, payload))
        payload = "{\"msg_id\":\"" + GetId() + "\",\"emoji\":\"" + emoji_str + "\"}";
    Network::Get()->Http().Post("/message/delete-reaction", payload,
        [](Http::Response r)
        {
            Logger::Get()->Log(samplog_LogLevel::INFO,
                "KOOK delete-reaction response: {} {} | {}",
                r.status, r.reason, r.body);
        });
    return true;
}

bool Message::EditMessage(const std::string& msg, const EmbedId_t embedid)
{
    // KOOK: update message via POST /api/v3/message/update
    // Supports type 9 (KMarkdown) and type 10 (CardMessage). The caller should pass proper content.
    json body;
    body["msg_id"] = GetId();

    // If an Embed is provided, convert it into a KOOK Card JSON string
    if (embedid != INVALID_EMBED_ID)
    {
        const auto& embed = EmbedManager::Get()->FindEmbed(embedid);
        if (!embed)
        {
            Logger::Get()->Log(samplog_LogLevel::ERROR, "invalid embed id {}", embedid);
            return false;
        }

        json card = json::array();
        json card_obj;
        card_obj["type"] = "card";

        // color from embed
        {
            unsigned int color = embed->GetColor();
            char buf[10];
            std::snprintf(buf, sizeof(buf), "#%06X", color & 0xFFFFFF);
            card_obj["color"] = std::string(buf);
        }

        // modules
        json modules = json::array();

        // Optional message text as first section (kmarkdown)
        if (!msg.empty())
        {
            json section;
            section["type"] = "section";
            json text;
            text["type"] = "kmarkdown";
            text["content"] = msg;
            section["text"] = text;
            modules.push_back(section);
        }

        // Title as header
        if (!embed->GetTitle().empty())
        {
            json header;
            header["type"] = "header";
            json text;
            text["type"] = "plain-text";
            text["content"] = embed->GetTitle();
            header["text"] = text;
            modules.push_back(header);
        }

        // Description as section (kmarkdown)
        if (!embed->GetDescription().empty())
        {
            json section;
            section["type"] = "section";
            json text;
            text["type"] = "kmarkdown";
            text["content"] = embed->GetDescription();
            section["text"] = text;
            modules.push_back(section);
        }

        // Fields as paragraph
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
                // combine name and value as one line
                fld["content"] = fmt::format("**{}**\n{}", f._name, f._value);
                fields.push_back(fld);
            }
            para["fields"] = fields;
            json text;
            text["type"] = "paragraph";
            text["fields"] = fields;
            section["text"] = text;
            modules.push_back(section);
        }

        // Thumbnail image
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

        // Main image
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

        // Footer as context
        if (!embed->GetFooterText().empty())
        {
            json context;
            context["type"] = "context";
            json elements = json::array();
            json txt;
            txt["type"] = "plain-text";
            txt["content"] = embed->GetFooterText();
            elements.push_back(txt);
            context["elements"] = elements;
            modules.push_back(context);
        }

        card_obj["modules"] = modules;
        card.push_back(card_obj);

        std::string card_str;
        utils::TryDumpJson(card, card_str);
        body["content"] = card_str;
        // cleanup embed after use
        EmbedManager::Get()->DeleteEmbed(embedid);
    }
    else
    {
        // Plain KMarkdown/text content
        body["content"] = msg;
    }
    std::string payload;
    if (!utils::TryDumpJson(body, payload))
        payload = "{\"msg_id\":\"" + GetId() + "\",\"content\":\"" + msg + "\"}";
    Network::Get()->Http().Post("/message/update", payload);
    return true;
}

void MessageManager::Initialize()
{
	// PAWN callbacks
	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::MESSAGE_CREATE, [](json const &data)
	{
		PawnDispatcher::Get()->Dispatch([data]() mutable
		{
			MessageId_t msg = MessageManager::Get()->Create(data);
			if (msg != INVALID_MESSAGE_ID)
			{
				// forward KCC_OnMessageCreate(DCC_Message:message);
				pawn_cb::Error error;
				pawn_cb::Callback::CallFirst(error, "KCC_OnMessageCreate", msg);

				MessageManager::Get()->Delete(msg);
			}
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::MESSAGE_DELETE, [](json const &data)
	{
		Snowflake_t sfid;
		if (!utils::TryGetJsonValue(data, sfid, "id"))
			return;

		PawnDispatcher::Get()->Dispatch([sfid]() mutable
		{
			auto const &msg = MessageManager::Get()->FindById(sfid);
			if (msg)
			{
				// forward KCC_OnMessageDelete(DCC_Message:message);
				pawn_cb::Error error;
				pawn_cb::Callback::CallFirst(error, "KCC_OnMessageDelete", msg->GetPawnId());

				MessageManager::Get()->Delete(msg->GetPawnId());
			}
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::MESSAGE_REACTION_ADD, [](json const& data)
	{
		Snowflake_t user_id, message_id, emoji_id;
		std::string name;
		if (!utils::TryGetJsonValue(data, user_id, "user_id"))
			return;

		if (!utils::TryGetJsonValue(data, message_id, "message_id"))
			return;
		if (!utils::TryGetJsonValue(data, name, "emoji", "name"))
			return;
		utils::TryGetJsonValue(data, emoji_id, "emoji", "id");

		PawnDispatcher::Get()->Dispatch([user_id, message_id, emoji_id, name]() mutable
		{
			auto const& msg = MessageManager::Get()->FindById(message_id);
			auto const& user = UserManager::Get()->FindUserById(user_id);
			if (msg && user)
			{
				auto const id = EmojiManager::Get()->AddEmoji(emoji_id, name);
				// forward KCC_OnMessageReactionAdd(DCC_Message:message, DCC_User:reaction_user, DCC_Emoji:emoji, DCC_MessageReactionType:reaction_type);
				pawn_cb::Error error;
				pawn_cb::Callback::CallFirst(error, "KCC_OnMessageReaction", msg->GetPawnId(), user->GetPawnId(), id, static_cast<int>(Message::ReactionType::REACTION_ADD));
				EmojiManager::Get()->DeleteEmoji(id);
			}
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::MESSAGE_REACTION_REMOVE, [](json const& data)
	{
		Snowflake_t user_id, message_id, emoji_id;
		std::string name;
		if (!utils::TryGetJsonValue(data, user_id, "user_id"))
			return;

		if (!utils::TryGetJsonValue(data, message_id, "message_id"))
			return;

		if (!utils::TryGetJsonValue(data, name, "emoji", "name"))
			return;
		utils::TryGetJsonValue(data, emoji_id, "emoji", "id");

		PawnDispatcher::Get()->Dispatch([data, user_id, message_id, emoji_id, name]() mutable
		{
			auto const& msg = MessageManager::Get()->FindById(message_id);
			auto const& user = UserManager::Get()->FindUserById(user_id);
			if (msg && user)
			{
				auto const id = EmojiManager::Get()->AddEmoji(emoji_id, name);
				// forward KCC_OnMessageReactionAdd(DCC_Message:message, DCC_User:reaction_user, DCC_Emoji:emoji, DCC_MessageReactionType:reaction_type);
				pawn_cb::Error error;
				pawn_cb::Callback::CallFirst(error, "KCC_OnMessageReaction", msg->GetPawnId(), user->GetPawnId(), id, static_cast<int>(Message::ReactionType::REACTION_REMOVE));
				EmojiManager::Get()->DeleteEmoji(id);
			}
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::MESSAGE_REACTION_REMOVE_ALL, [](json const& data)
	{
		Snowflake_t message_id;

		if (!utils::TryGetJsonValue(data, message_id, "message_id"))
			return;

		PawnDispatcher::Get()->Dispatch([message_id]() mutable
		{
			auto const& msg = MessageManager::Get()->FindById(message_id);
			if (msg)
			{
				// forward KCC_OnMessageReactionAdd(DCC_Message:message, DCC_User:reaction_user, DCC_Emoji:emoji, DCC_MessageReactionType:reaction_type);
				pawn_cb::Error error;
				pawn_cb::Callback::CallFirst(error, "KCC_OnMessageReaction", msg->GetPawnId(), INVALID_USER_ID, INVALID_EMOJI_ID, static_cast<int>(Message::ReactionType::REACTION_REMOVE_ALL));
			}
		});
	});

	Network::Get()->WebSocket().RegisterEvent(WebSocket::Event::MESSAGE_REACTION_REMOVE_EMOJI, [](json const& data)
	{
		Snowflake_t  message_id, emoji_id;
		std::string name;

		if (!utils::TryGetJsonValue(data, message_id, "message_id"))
			return;
		if (!utils::TryGetJsonValue(data, name, "emoji", "name"))
			return;
		utils::TryGetJsonValue(data, emoji_id, "emoji", "id");

		PawnDispatcher::Get()->Dispatch([message_id, emoji_id, name]() mutable
		{
			auto const& msg = MessageManager::Get()->FindById(message_id);
			if (msg)
			{
				auto const id = EmojiManager::Get()->AddEmoji(emoji_id, name);
				// forward KCC_OnMessageReactionAdd(DCC_Message:message, DCC_User:reaction_user, DCC_Emoji:emoji, DCC_MessageReactionType:reaction_type);
				pawn_cb::Error error;
				pawn_cb::Callback::CallFirst(error, "KCC_OnMessageReaction", msg->GetPawnId(), INVALID_USER_ID, id, static_cast<int>(Message::ReactionType::REACTION_REMOVE_EMOJI));
				EmojiManager::Get()->DeleteEmoji(id);
			}
		});
	});
}

MessageId_t MessageManager::Create(json const &data)
{
	MessageId_t id = 1;
	while (m_Messages.find(id) != m_Messages.end())
		++id;

	if (!m_Messages.emplace(id, Message_t(new Message(id, data))).second)
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"can't create message: duplicate key '{}'", id);
		return INVALID_MESSAGE_ID;
	}

	Logger::Get()->Log(samplog_LogLevel::DEBUG, "created message with id '{}'", id);
	return id;
}

bool MessageManager::Delete(MessageId_t id)
{
	auto it = m_Messages.find(id);
	if (it == m_Messages.end())
		return false;

	m_Messages.erase(it);
	Logger::Get()->Log(samplog_LogLevel::DEBUG, "deleted message with id '{}'", id);
	return true;
}

void MessageManager::CreateFromSnowflake(Snowflake_t channel, Snowflake_t message, pawn_cb::Callback_t&& callback)
{
	Network::Get()->Http().Get(fmt::format("/channels/{:s}/messages/{:s}", channel, message),
		[this, callback](Http::Response r)
		{
			Logger::Get()->Log(samplog_LogLevel::DEBUG,
				"message fetch response: status {}; body: {}; add: {}",
				r.status, r.body, r.additional_data);
			if (r.status / 100 == 2) // success
			{
				const auto & message_id = Create(json::parse(r.body));
				if (callback)
				{
					PawnDispatcher::Get()->Dispatch([this, message_id, callback]() mutable
					{
						if (message_id)
						{
							SetCreatedMessageId(message_id);
							callback->Execute();
							SetCreatedMessageId(INVALID_MESSAGE_ID);
						}
					});
				}
				if (!Find(message_id)->Persistent())
				{
					Delete(message_id);
				}
			}
		}
	);
}

Message_t const &MessageManager::Find(MessageId_t id)
{
	static Message_t invalid_msg;
	auto it = m_Messages.find(id);
	if (it == m_Messages.end())
		return invalid_msg;
	return it->second;
}

Message_t const &MessageManager::FindById(Snowflake_t const &sfid)
{
	static Message_t invalid_msg;
	for (auto const &u : m_Messages)
	{
		auto const &msg = u.second;
		if (msg->GetId().compare(sfid) == 0)
			return msg;
	}
	return invalid_msg;
}
