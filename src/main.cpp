#include "sdk.hpp"
#include "natives.hpp"
#include "PawnDispatcher.hpp"
#include "Network.hpp"
#include "Guild.hpp"
#include "User.hpp"
#include "Channel.hpp"
#include "Message.hpp"
#include "SampConfigReader.hpp"
#include "Logger.hpp"
#include "version.hpp"

// OMP SDK headers must be included before samplog to avoid LogLevel conflict
#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>

#include <samplog/samplog.hpp>
#include <thread>
#include <cstdlib>


extern void	*pAMXFunctions;
logprintf_t logprintf;


void InitializeEverything(std::string const &bot_token)
{
	GuildManager::Get()->Initialize();
	UserManager::Get()->Initialize();
	ChannelManager::Get()->Initialize();
	MessageManager::Get()->Initialize();

	Network::Get()->Initialize(bot_token);
}

void DestroyEverything()
{
	MessageManager::Singleton::Destroy();
	ChannelManager::Singleton::Destroy();
	UserManager::Singleton::Destroy();
	GuildManager::Singleton::Destroy();
	Network::Singleton::Destroy();
}

bool WaitForInitialization()
{
	unsigned int const
		SLEEP_TIME_MS = 20,
		TIMEOUT_TIME_MS = 20 * 1000;
	unsigned int waited_time = 0;
	while (true)
	{
		if (GuildManager::Get()->IsInitialized()
			&& UserManager::Get()->IsInitialized()
			&& ChannelManager::Get()->IsInitialized())
		{
			return true;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TIME_MS));
		waited_time += SLEEP_TIME_MS;
		if (waited_time > TIMEOUT_TIME_MS)
			break;
	}
	return false;
}

std::string GetEnvironmentVar(const char *key)
{
	const char *value = getenv(key);
	return value != nullptr ? std::string(value) : std::string();
}

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports()
{
	return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void **ppData)
{
	pAMXFunctions = ppData[PLUGIN_DATA_AMX_EXPORTS];
	logprintf = (logprintf_t)ppData[PLUGIN_DATA_LOGPRINTF];

	bool ret_val = true;
	auto bot_token = GetEnvironmentVar("SAMP_KOOK_BOT_TOKEN");

	if (bot_token.empty())
		SampConfigReader::Get()->GetVar("kook_bot_token", bot_token);

	if (!bot_token.empty())
	{
		InitializeEverything(bot_token);

		if (WaitForInitialization())
		{
			logprintf(" >> plugin.kook-connector: " PLUGIN_VERSION " Loaded!");
		}
		else
		{
			logprintf(" >> plugin.kook-connector: Failed to initialize data.");

			std::thread init_thread([bot_token]()
			{
				while (true)
				{
					std::this_thread::sleep_for(std::chrono::minutes(1));

					DestroyEverything();
					InitializeEverything(bot_token);
					if (WaitForInitialization())
						break;
				}
			});
			init_thread.detach();

			logprintf("                         Plugin will continue to retry in the background...");
		}
	}
	else
	{
		logprintf(" >> plugin.kook-connector: Environment variable or server configuration did not specify the bot token.");
		ret_val = false;
	}
	return ret_val;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload()
{
	logprintf("plugin.kook-connector: Unloading plugin...");

	DestroyEverything();
	Logger::Singleton::Destroy();

	samplog::Api::Destroy();

	logprintf("plugin.ector: Plugin unloaded.");
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick()
{
	PawnDispatcher::Get()->Process();
}


extern "C" const AMX_NATIVE_INFO native_list[] =
{
	//AMX_DEFINE_NATIVE(native_name)
	AMX_DEFINE_NATIVE(DCC_FindChannelByName)
	AMX_DEFINE_NATIVE(KCC_FindChannelById)
	AMX_DEFINE_NATIVE(KCC_GetChannelId)
	AMX_DEFINE_NATIVE(DCC_GetChannelType)
	AMX_DEFINE_NATIVE(KCC_GetChannelGuild)
	AMX_DEFINE_NATIVE(KCC_GetChannelName)
	AMX_DEFINE_NATIVE(KCC_GetChannelTopic)
	AMX_DEFINE_NATIVE(KCC_GetChannelPosition)
	AMX_DEFINE_NATIVE(DCC_IsChannelNsfw)
	AMX_DEFINE_NATIVE(DCC_GetChannelParentCategory)
	AMX_DEFINE_NATIVE(KCC_SendChannelMessage)
	AMX_DEFINE_NATIVE(KCC_SetChannelName)
	AMX_DEFINE_NATIVE(KCC_SetChannelTopic)
	AMX_DEFINE_NATIVE(DCC_SetChannelPosition)
	AMX_DEFINE_NATIVE(DCC_SetChannelNsfw)
	AMX_DEFINE_NATIVE(DCC_SetChannelParentCategory)
	AMX_DEFINE_NATIVE(KCC_DeleteChannel)

	AMX_DEFINE_NATIVE(KCC_GetMessageId)
	AMX_DEFINE_NATIVE(KCC_GetMessageChannel)
	AMX_DEFINE_NATIVE(KCC_GetMessageAuthor)
	AMX_DEFINE_NATIVE(KCC_GetMessageContent)
	AMX_DEFINE_NATIVE(DCC_IsMessageTts)
	AMX_DEFINE_NATIVE(KCC_IsMessageMentioningEveryone)
	AMX_DEFINE_NATIVE(KCC_GetMessageUserMentionCount)
	AMX_DEFINE_NATIVE(KCC_GetMessageUserMention)
	AMX_DEFINE_NATIVE(KCC_GetMessageRoleMentionCount)
	AMX_DEFINE_NATIVE(KCC_GetMessageRoleMention)
	AMX_DEFINE_NATIVE(KCC_DeleteMessage)
	AMX_DEFINE_NATIVE(KCC_GetCreatedMessage)

	AMX_DEFINE_NATIVE(KCC_FindUserByName)
	AMX_DEFINE_NATIVE(KCC_FindUserById)
	AMX_DEFINE_NATIVE(KCC_GetUserId)
	AMX_DEFINE_NATIVE(KCC_GetUserName)
	AMX_DEFINE_NATIVE(KCC_GetUserDiscriminator)
	AMX_DEFINE_NATIVE(KCC_IsUserBot)
	AMX_DEFINE_NATIVE(KCC_IsUserVerified)

	AMX_DEFINE_NATIVE(KCC_FindRoleByName)
	AMX_DEFINE_NATIVE(KCC_FindRoleById)
	AMX_DEFINE_NATIVE(KCC_GetRoleId)
	AMX_DEFINE_NATIVE(KCC_GetRoleName)
	AMX_DEFINE_NATIVE(KCC_GetRoleColor)
	AMX_DEFINE_NATIVE(KCC_GetRolePermissions)
	AMX_DEFINE_NATIVE(KCC_IsRoleHoist)
	AMX_DEFINE_NATIVE(KCC_GetRolePosition)
	AMX_DEFINE_NATIVE(KCC_IsRoleMentionable)

	AMX_DEFINE_NATIVE(KCC_FindGuildByName)
	AMX_DEFINE_NATIVE(KCC_FindGuildById)
	AMX_DEFINE_NATIVE(KCC_GetGuildId)
	AMX_DEFINE_NATIVE(KCC_GetGuildName)
	AMX_DEFINE_NATIVE(KCC_GetGuildOwnerId)
	AMX_DEFINE_NATIVE(KCC_GetGuildRole)
	AMX_DEFINE_NATIVE(KCC_GetGuildRoleCount)
	AMX_DEFINE_NATIVE(KCC_GetGuildMember)
	AMX_DEFINE_NATIVE(KCC_GetGuildMemberCount)
	AMX_DEFINE_NATIVE(KCC_GetGuildMemberVoiceChannel)
	AMX_DEFINE_NATIVE(KCC_GetGuildMemberNickname)
	AMX_DEFINE_NATIVE(KCC_GetGuildMemberRole)
	AMX_DEFINE_NATIVE(KCC_GetGuildMemberRoleCount)
	AMX_DEFINE_NATIVE(KCC_HasGuildMemberRole)
	AMX_DEFINE_NATIVE(DCC_GetGuildMemberStatus)
	AMX_DEFINE_NATIVE(KCC_GetGuildChannel)
	AMX_DEFINE_NATIVE(KCC_GetGuildChannelCount)
	AMX_DEFINE_NATIVE(KCC_GetAllGuilds)
	AMX_DEFINE_NATIVE(DCC_SetGuildName)
	AMX_DEFINE_NATIVE(KCC_CreateGuildChannel)
	AMX_DEFINE_NATIVE(KCC_GetCreatedGuildChannel)
	AMX_DEFINE_NATIVE(KCC_SetGuildMemberNickname)
	AMX_DEFINE_NATIVE(DCC_SetGuildMemberVoiceChannel)
	AMX_DEFINE_NATIVE(KCC_AddGuildMemberRole)
	AMX_DEFINE_NATIVE(KCC_RemoveGuildMemberRole)
	AMX_DEFINE_NATIVE(KCC_RemoveGuildMember)
	AMX_DEFINE_NATIVE(KCC_CreateGuildMemberBan)
	AMX_DEFINE_NATIVE(KCC_RemoveGuildMemberBan)
	AMX_DEFINE_NATIVE(DCC_SetGuildRolePosition)
	AMX_DEFINE_NATIVE(KCC_SetGuildRoleName)
	AMX_DEFINE_NATIVE(KCC_SetGuildRolePermissions)
	AMX_DEFINE_NATIVE(KCC_SetGuildRoleColor)
	AMX_DEFINE_NATIVE(KCC_SetGuildRoleHoist)
	AMX_DEFINE_NATIVE(KCC_SetGuildRoleMentionable)
	AMX_DEFINE_NATIVE(KCC_CreateGuildRole)
	AMX_DEFINE_NATIVE(KCC_GetCreatedGuildRole)
	AMX_DEFINE_NATIVE(KCC_DeleteGuildRole)

	AMX_DEFINE_NATIVE(DCC_GetBotPresenceStatus)
	AMX_DEFINE_NATIVE(DCC_TriggerBotTypingIndicator)
	AMX_DEFINE_NATIVE(DCC_SetBotNickname)
	AMX_DEFINE_NATIVE(KCC_CreatePrivateChannel)
	AMX_DEFINE_NATIVE(KCC_GetCreatedPrivateChannel)
	AMX_DEFINE_NATIVE(DCC_SetBotPresenceStatus)
	AMX_DEFINE_NATIVE(DCC_SetBotActivity)

	AMX_DEFINE_NATIVE(KCC_EscapeMarkdown)

	AMX_DEFINE_NATIVE(KCC_CreateEmbed)
	AMX_DEFINE_NATIVE(KCC_DeleteEmbed)
	AMX_DEFINE_NATIVE(KCC_SendChannelEmbedMessage)
	AMX_DEFINE_NATIVE(KCC_AddEmbedField)
	AMX_DEFINE_NATIVE(KCC_SetEmbedTitle)
	AMX_DEFINE_NATIVE(KCC_SetEmbedDescription)
	AMX_DEFINE_NATIVE(KCC_SetEmbedUrl)
	AMX_DEFINE_NATIVE(KCC_SetEmbedTimestamp)
	AMX_DEFINE_NATIVE(KCC_SetEmbedColor)
	AMX_DEFINE_NATIVE(KCC_SetEmbedFooter)
	AMX_DEFINE_NATIVE(KCC_SetEmbedThumbnail)
	AMX_DEFINE_NATIVE(KCC_SetEmbedImage)

	AMX_DEFINE_NATIVE(KCC_DeleteInternalMessage)

	AMX_DEFINE_NATIVE(KCC_CreateEmoji)
	AMX_DEFINE_NATIVE(KCC_DeleteEmoji)
	AMX_DEFINE_NATIVE(KCC_GetEmojiName)

	AMX_DEFINE_NATIVE(KCC_CreateReaction)
	AMX_DEFINE_NATIVE(KCC_DeleteMessageReaction)

	AMX_DEFINE_NATIVE(KCC_EditMessage)
	AMX_DEFINE_NATIVE(KCC_SetMessagePersistent)
	AMX_DEFINE_NATIVE(KCC_CacheChannelMessage)
	{ NULL, NULL }
};

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX *amx)
{
	samplog::Api::Get()->RegisterAmx(amx);
	pawn_cb::AddAmx(amx);
	return amx_Register(amx, native_list, -1);
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX *amx)
{
	samplog::Api::Get()->EraseAmx(amx);
	pawn_cb::RemoveAmx(amx);
	return AMX_ERR_NONE;
}

// ============================================================================
// OMP Component Implementation
// ============================================================================

class KookComponent : public IComponent, public PawnEventHandler, public CoreEventHandler
{
	PROVIDE_UID(0xBFB3CB4F2543686C); // "KOOKCONN" in hex
	IPawnComponent* pawnComponent = nullptr;
	static ICore* core;
	
	static void logprintfwrapped(char const* format, ...)
	{
		va_list params;
		va_start(params, format);
		core->vlogLn(LogLevel::Message, format, params);
		va_end(params);
	}

	StringView componentName() const override
	{
		return "kook-connector";
	}

	SemanticVersion componentVersion() const override
	{
		return SemanticVersion(0, 3, 5, 0);
	}

	void onLoad(ICore* c) override
	{
		core = c;
		logprintf = KookComponent::logprintfwrapped;

		auto bot_token = GetEnvironmentVar("SAMP_KOOK_BOT_TOKEN");

		if (bot_token.empty()) {
			auto token = core->getConfig().getString("kook.bot_token");
			if (!token.empty()) {
				bot_token = token.data();
			}
		}

		if (!bot_token.empty())
		{
			InitializeEverything(bot_token.data());

			if (WaitForInitialization())
			{
				logprintf(" >> kook-connector: " PLUGIN_VERSION " successfully loaded.");
			}
			else
			{
				logprintf(" >> kook-connector: timeout while initializing data.");

				std::thread init_thread([bot_token]()
				{
					while (true)
					{
						std::this_thread::sleep_for(std::chrono::minutes(1));

						DestroyEverything();
						InitializeEverything(bot_token.data());
						if (WaitForInitialization())
							break;
					}
				});
				init_thread.detach();

				logprintf("                    component will proceed to retry connecting in the background.");
			}
		}
		else
		{
			logprintf(" >> kook-connector: bot token not specified in environment variable or server config.");
		}
	}

	void onInit(IComponentList* components) override
	{
		pawnComponent = components->queryComponent<IPawnComponent>();
		core->getEventDispatcher().addEventHandler(this);
		if (pawnComponent)
		{
			pAMXFunctions = (void*)&pawnComponent->getAmxFunctions();
			pawnComponent->getEventDispatcher().addEventHandler(this, EventPriority_FairlyHigh);
		}
		else
		{
			core->logLn(LogLevel::Error, "kook-connector: Pawn component not loaded.");
		}
	}

	void onAmxLoad(IPawnScript& script) override
	{
		amx_Register(script.GetAMX(), native_list, -1);
		samplog::Api::Get()->RegisterAmx(script.GetAMX());
		pawn_cb::AddAmx(script.GetAMX());
	}

	void onAmxUnload(IPawnScript& script) override
	{
		samplog::Api::Get()->EraseAmx(script.GetAMX());
		pawn_cb::RemoveAmx(script.GetAMX());
	}

	void onFree(IComponent* component) override
	{
		if (component == pawnComponent)
		{
			pawnComponent = nullptr;
			pAMXFunctions = nullptr;
		}
	}

	void free() override
	{
		if (pawnComponent)
		{
			pawnComponent->getEventDispatcher().removeEventHandler(this);
		}
		core->getEventDispatcher().removeEventHandler(this);

		logprintf("kook-connector: Unloading component...");

		DestroyEverything();
		Logger::Singleton::Destroy();

		samplog::Api::Destroy();

		logprintf("kook-connector: Component unloaded.");
		delete this;
	}

	void reset() override
	{
		// Nothing to reset for now.
	}

	void provideConfiguration(ILogger& logger, IEarlyConfig& config, bool defaults) override
	{
		if (defaults)
		{
			config.setString("kook.bot_token", "");
		}
		else
		{
			if (config.getType("kook.bot_token") == ConfigOptionType_None)
			{
				config.setString("kook.bot_token", "");
			}
		}
	}

	void onTick(Microseconds elapsed, TimePoint now) override
	{
		PawnDispatcher::Get()->Process();
	}
};

ICore* KookComponent::core = nullptr;

COMPONENT_ENTRY_POINT()
{
	return new KookComponent();
}