#include "Role.hpp"
#include "Logger.hpp"
#include "utils.hpp"


Role::Role(RoleId_t pawn_id, json const &data) :
    m_PawnId(pawn_id)
{
    // Accept both Discord and KOOK schemas
    // Discord:   { id: "...", ... }
    // KOOK:      { role_id: <number>, ... }
    if (!utils::TryGetJsonValue(data, m_Id, "id"))
    {
        unsigned long long rid_num = 0ULL;
        if (utils::TryGetJsonValue(data, rid_num, "role_id"))
        {
            m_Id = std::to_string(rid_num);
        }
        else
        {
            Logger::Get()->Log(samplog_LogLevel::ERROR,
                "invalid JSON: expected \"id\"/\"role_id\" in \"{}\"", data.dump());
            return;
        }
    }

    Update(data);
}

void Role::Update(json const &data)
{
    // Basic fields
    bool ok_name = utils::TryGetJsonValue(data, m_Name, "name");
    bool ok_color = utils::TryGetJsonValue(data, m_Color, "color");
    bool ok_pos = utils::TryGetJsonValue(data, m_Position, "position");

    // hoist may be bool or 0/1
    bool ok_hoist = false;
    if (!(ok_hoist = utils::TryGetJsonValue(data, m_Hoist, "hoist")))
    {
        unsigned int hoist_u = 0u;
        if ((ok_hoist = utils::TryGetJsonValue(data, hoist_u, "hoist")))
            m_Hoist = (hoist_u != 0u);
    }

    // mentionable may be bool or 0/1
    bool ok_mentionable = false;
    if (!(ok_mentionable = utils::TryGetJsonValue(data, m_Mentionable, "mentionable")))
    {
        unsigned int mention_u = 0u;
        if ((ok_mentionable = utils::TryGetJsonValue(data, mention_u, "mentionable")))
            m_Mentionable = (mention_u != 0u);
    }

    // permissions can be string (Discord) or number (KOOK unsigned int)
    bool ok_perms = false;
    std::string permissions_str;
    if ((ok_perms = utils::TryGetJsonValue(data, permissions_str, "permissions")))
    {
        try { m_Permissions = std::stoull(permissions_str); }
        catch (...) { m_Permissions = 0ULL; ok_perms = false; }
    }
    if (!ok_perms)
    {
        unsigned long long perms_ull = 0ULL;
        if ((ok_perms = utils::TryGetJsonValue(data, perms_ull, "permissions")))
        {
            m_Permissions = perms_ull;
        }
        else
        {
            unsigned int perms_u = 0u;
            if ((ok_perms = utils::TryGetJsonValue(data, perms_u, "permissions")))
                m_Permissions = static_cast<unsigned long long>(perms_u);
        }
    }

    _valid = ok_name && ok_color && ok_pos && ok_hoist && ok_mentionable && ok_perms;
    if (!_valid)
    {
        Logger::Get()->Log(samplog_LogLevel::ERROR,
            "can't update role: invalid/unsupported JSON: \"{}\"", data.dump());
    }
}


RoleId_t RoleManager::AddRole(json const &data)
{
    Snowflake_t sfid;
    if (!utils::TryGetJsonValue(data, sfid, "id"))
    {
        // Try KOOK numeric role_id
        unsigned long long rid_num = 0ULL;
        if (utils::TryGetJsonValue(data, rid_num, "role_id"))
        {
            sfid = std::to_string(rid_num);
        }
        else
        {
            Logger::Get()->Log(samplog_LogLevel::ERROR,
                "invalid JSON: expected \"id\"/\"role_id\" in \"{}\"", data.dump());
            return INVALID_ROLE_ID;
        }
    }

    Role_t const &role = FindRoleById(sfid);
	if (role)
	{
		return role->GetPawnId();
	}

    RoleId_t id = 1;
    while (m_Roles.find(id) != m_Roles.end())
        ++id;

	if (!m_Roles.emplace(id, Role_t(new Role(id, data))).first->second)
	{
		Logger::Get()->Log(samplog_LogLevel::ERROR,
			"can't create role: duplicate key '{}'", id);
		return INVALID_ROLE_ID;
	}
	return id;
}

void RoleManager::RemoveRole(Role_t const &role)
{
	m_Roles.erase(role->GetPawnId());
}

Role_t const &RoleManager::FindRole(RoleId_t id)
{
	static Role_t invalid_role;
	auto it = m_Roles.find(id);
	if (it == m_Roles.end())
		return invalid_role;
	return it->second;
}

Role_t const &RoleManager::FindRoleById(Snowflake_t const &sfid)
{
	static Role_t invalid_role;
	for (auto const &g : m_Roles)
	{
		Role_t const &role = g.second;
		if (role->GetId().compare(sfid) == 0)
			return role;
	}
	return invalid_role;
}
