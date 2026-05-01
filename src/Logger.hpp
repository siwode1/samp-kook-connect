#pragma once

#include <samplog/samplog.hpp>
#include "Singleton.hpp"

#include <fmt/format.h>

using samplog::PluginLogger_t;
using samplog::AmxFuncCallInfo;

enum class samplog_LogLevel
{
	NONE    = samplog::NONE,
	DEBUG   = samplog::DEBUG,
	INFO    = samplog::INFO,
	WARNING = samplog::WARNING,
	ERROR   = samplog::ERROR,
	FATAL   = samplog::FATAL,
	VERBOSE = samplog::VERBOSE
};

using SampLogLevel = samplog_LogLevel;

class DebugInfoManager : public Singleton<DebugInfoManager>
{
	friend class Singleton<DebugInfoManager>;
	friend class ScopedDebugInfo;
private:
	DebugInfoManager() = default;
	~DebugInfoManager() = default;

private:
	bool m_Available = false;

	AMX *m_Amx = nullptr;
	std::vector<AmxFuncCallInfo> m_Info;
	const char *m_NativeName = nullptr;

private:
	void Update(AMX * const amx, const char *func);
	void Clear();

public:
	inline AMX * GetCurrentAmx()
	{
		return m_Amx;
	}
	inline const decltype(m_Info) &GetCurrentInfo()
	{
		return m_Info;
	}
	inline bool IsInfoAvailable()
	{
		return m_Available;
	}
	inline const char *GetCurrentNativeName()
	{
		return m_NativeName;
	}
};


class Logger : public Singleton<Logger>
{
	friend class Singleton<Logger>;
	friend class ScopedDebugInfo;
private:
	Logger() :
		m_Logger("kook-connector")
	{ }
	~Logger() = default;

public:
	inline bool IsLogLevel(samplog_LogLevel level)
	{
		return m_Logger.IsLogLevel(ToNativeLevel(level));
	}

	template<typename... Args>
	inline void Log(samplog_LogLevel level, const char *msg)
	{
		m_Logger.Log(ToNativeLevel(level), msg);
	}

	template<typename... Args>
	inline void Log(samplog_LogLevel level, const char *format, Args &&...args)
	{
		auto str = fmt::format(format, std::forward<Args>(args)...);
		m_Logger.Log(ToNativeLevel(level), str.c_str());
	}

	template<typename... Args>
	inline void Log(samplog_LogLevel level, std::vector<AmxFuncCallInfo> const &callinfo,
		const char *msg)
	{
		m_Logger.Log(ToNativeLevel(level), msg, callinfo);
	}

	template<typename... Args>
	inline void Log(samplog_LogLevel level, std::vector<AmxFuncCallInfo> const &callinfo,
		const char *format, Args &&...args)
	{
		auto str = fmt::format(format, std::forward<Args>(args)...);
		m_Logger.Log(ToNativeLevel(level), str.c_str(), callinfo);
	}

	// should only be called in native functions
	template<typename... Args>
	void LogNative(samplog_LogLevel level, const char *fmt, Args &&...args)
	{
		if (!IsLogLevel(level))
			return;

		if (DebugInfoManager::Get()->GetCurrentAmx() == nullptr)
			return; //do nothing, since we're not called from within a native func

		auto msg = fmt::format("{:s}: {:s}",
			DebugInfoManager::Get()->GetCurrentNativeName(),
			fmt::format(fmt, std::forward<Args>(args)...));

		if (DebugInfoManager::Get()->IsInfoAvailable())
			Log(level, DebugInfoManager::Get()->GetCurrentInfo(), msg.c_str());
		else
			Log(level, msg.c_str());
	}

private:
	static samplog::LogLevel ToNativeLevel(samplog_LogLevel level)
	{
		return static_cast<samplog::LogLevel>(static_cast<int>(level));
	}

	PluginLogger_t m_Logger;

};


class ScopedDebugInfo
{
public:
	ScopedDebugInfo(AMX * const amx, const char *func,
		cell * const params, const char *params_format = "");
	~ScopedDebugInfo()
	{
		DebugInfoManager::Get()->Clear();
	}
	ScopedDebugInfo(const ScopedDebugInfo &rhs) = delete;
};
