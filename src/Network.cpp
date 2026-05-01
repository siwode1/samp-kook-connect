#include "Network.hpp"
#include "Logger.hpp"


void Network::Initialize(std::string const &token)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "Network::Initialize");

	m_Http = std::unique_ptr<::Http>(new ::Http(token));

	// retrieve KOOK WebSocket gateway URL (compress=0 for plain JSON frames in MVP)
	m_Http->Get("/gateway/index?compress=0", [this, token](Http::Response res)
	{
		if (res.status != 200)
		{
			Logger::Get()->Log(SampLogLevel::ERROR, "Can't retrieve KOOK gateway URL: {} ({})",
				res.reason, res.status);
			return;
		}
		auto gateway_res = json::parse(res.body);
		// KOOK: { code, message, data: { url: "wss://..." } }
		std::string gateway_url;
		auto dit = gateway_res.find("data");
		if (dit != gateway_res.end())
		{
			auto uit = dit->find("url");
			if (uit != dit->end() && uit->is_string())
				gateway_url = uit->get<std::string>();
		}
		if (gateway_url.empty())
		{
			Logger::Get()->Log(SampLogLevel::ERROR, "KOOK gateway response missing data.url: {}", res.body);
			return;
		}

		// Split into host and path
		std::string host, path;
		{
			// strip scheme
			size_t scheme_pos = gateway_url.find("://");
			std::string without_scheme = (scheme_pos != std::string::npos)
				? gateway_url.substr(scheme_pos + 3)
				: gateway_url;
			// split host/path
			size_t slash_pos = without_scheme.find('/');
			if (slash_pos == std::string::npos)
			{
				host = without_scheme;
				path = "/gateway"; // default
			}
			else
			{
				host = without_scheme.substr(0, slash_pos);
				path = without_scheme.substr(slash_pos);
			}
		}

		m_WebSocket->Initialize(token, host, path);
	});

}

Network::~Network()
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "Network::~Network");
}

::Http &Network::Http()
{
	return *m_Http;
}

::WebSocket &Network::WebSocket()
{
	return *m_WebSocket;
}
