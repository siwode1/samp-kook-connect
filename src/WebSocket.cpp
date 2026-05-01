#include "Network.hpp"
#include "Http.hpp"
#include "Logger.hpp"
#include "sdk.hpp"
#include "utils.hpp"

#include <unordered_map>

extern logprintf_t logprintf;

WebSocket::WebSocket() :
	_ioContext(),
	_resolver(asio::make_strand(_ioContext)),
	_sslContext(asio::ssl::context::tlsv12_client),
	_reconnectTimer(_ioContext),
	m_HeartbeatTimer(_ioContext),
	m_HeartbeatInterval()
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::WebSocket");
}

WebSocket::~WebSocket()
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::~WebSocket");

	Disconnect();

	if (_netThread)
		_netThread->join();
}

void WebSocket::Initialize(std::string token, std::string gateway_host, std::string gateway_path)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::Initialize");

	_gatewayUrl = gateway_host;
	_gatewayPath = gateway_path;
	_apiToken = token;

	Connect();

	_netThread = std::make_unique<std::thread>([this]()
	{
		_ioContext.run();
	});
}

void WebSocket::Connect()
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::Connect");

	_resolver.async_resolve(
		_gatewayUrl,
		"443",
		beast::bind_front_handler(
			&WebSocket::OnResolve,
			this));
}

void WebSocket::OnResolve(beast::error_code ec, 
	asio::ip::tcp::resolver::results_type results)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::OnResolve");

	if (ec)
	{
		Logger::Get()->Log(SampLogLevel::ERROR, 
			"Can't resolve KOOK gateway URL '{}': {} ({})",
			_gatewayUrl, ec.message(), ec.value());
		Disconnect(true);
		return;
	}

	_websocket.reset(
		new WebSocketStream_t(asio::make_strand(_ioContext), _sslContext));

	beast::get_lowest_layer(*_websocket).expires_after(
		std::chrono::seconds(30));
	beast::get_lowest_layer(*_websocket).async_connect(
		results, 
		beast::bind_front_handler(
			&WebSocket::OnConnect,
			this));
}

void WebSocket::OnConnect(beast::error_code ec,
	asio::ip::tcp::resolver::results_type::endpoint_type ep)
{
	boost::ignore_unused(ep);

	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::OnConnect");

	if (ec)
	{
		Logger::Get()->Log(SampLogLevel::ERROR, 
			"Can't connect to KOOK gateway: {} ({})",
			ec.message(), ec.value());
		Disconnect(true);
		return;
	}

	beast::get_lowest_layer(*_websocket).expires_after(std::chrono::seconds(30));
	_websocket->next_layer().async_handshake(
		asio::ssl::stream_base::client, 
		beast::bind_front_handler(
			&WebSocket::OnSslHandshake,
			this));
}

void WebSocket::OnSslHandshake(beast::error_code ec)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::OnSslHandshake");

	if (ec)
	{
		Logger::Get()->Log(SampLogLevel::ERROR,
			"Can't establish secured connection to KOOK gateway: {} ({})",
			ec.message(), ec.value());
		Disconnect(true);
		return;
	}

	// websocket stream has its own timeout system
	beast::get_lowest_layer(*_websocket).expires_never();

	_websocket->set_option(
		beast::websocket::stream_base::timeout::suggested(
			beast::role_type::client));

	// set a decorator to change the User-Agent of the handshake
	_websocket->set_option(beast::websocket::stream_base::decorator(
		[](beast::websocket::request_type &req)
	{
		req.set(beast::http::field::user_agent,
			std::string(BOOST_BEAST_VERSION_STRING) +
			" samp-kook-connector");
	}));

	// Build KOOK gateway handshake path using provided base path
	std::string path = _gatewayPath;
	if (path.find('?') == std::string::npos)
		path += "?";
	else
		path += "&";
	// disable compression for MVP and include token; include resume params if reconnecting
	path += "compress=0&token=" + _apiToken;
	if (_reconnect)
	{
		path += "&resume=1&sn=" + std::to_string(_sequenceNumber);
		if (!m_SessionId.empty())
			path += "&session_id=" + m_SessionId;
	}

	_websocket->async_handshake(
		_gatewayUrl + ":443",
		path,
		beast::bind_front_handler(
			&WebSocket::OnHandshake,
			this));
}

void WebSocket::OnHandshake(beast::error_code ec)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::OnHandshake");

	if (ec)
	{
		Logger::Get()->Log(SampLogLevel::ERROR,
			"Can't upgrade to WSS protocol: {} ({})",
			ec.message(), ec.value());
		Disconnect(true);
		return;
	}

	_reconnectCount = 0;

	// For KOOK, no explicit identify payload is needed. Start reading; HELLO (s=1)
	// from server will arrive and then we'll start heartbeat.
	Read();
}

void WebSocket::Disconnect(bool reconnect /*= false*/)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::Disconnect");

	_reconnect = reconnect;

	if (_websocket)
	{
		_websocket->async_close(
			beast::websocket::close_code::normal,
			beast::bind_front_handler(
				&WebSocket::OnClose,
				this));
	}
}

void WebSocket::OnClose(beast::error_code ec)
{
	boost::ignore_unused(ec);

	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::OnClose");

	m_HeartbeatTimer.cancel();

	if (_reconnect)
	{
		auto time = std::chrono::seconds(
			std::min(_reconnectCount * 5, 60u));

		if (time.count() > 0)
		{
			Logger::Get()->Log(SampLogLevel::INFO,
				"reconnecting in {:d} seconds",
				time.count());
		}

		_reconnectTimer.expires_from_now(time);
		_reconnectTimer.async_wait(
			beast::bind_front_handler(
				&WebSocket::OnReconnect,
				this));
	}
}

void WebSocket::OnReconnect(beast::error_code ec)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::OnReconnect");

	if (ec)
	{
		switch (ec.value())
		{
		case boost::asio::error::operation_aborted:
			// timer was chancelled, do nothing
			Logger::Get()->Log(SampLogLevel::DEBUG, "reconnect timer chancelled");
			break;
		default:
			Logger::Get()->Log(SampLogLevel::ERROR, "reconnect timer error: {} ({})",
				ec.message(), ec.value());
			break;
		}
		return;
	}

	++_reconnectCount;
	Connect();
}

void WebSocket::Read()
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::Read");

	_websocket->async_read(
		_buffer,
		beast::bind_front_handler(
			&WebSocket::OnRead,
			this));
}

void WebSocket::OnRead(beast::error_code ec,
	std::size_t bytes_transferred)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, 
		"WebSocket::OnRead({:d})",
		bytes_transferred);

	if (ec)
	{
		bool reconnect = false;
		switch (ec.value())
		{
		case asio::ssl::error::stream_errors::stream_truncated:
			Logger::Get()->Log(SampLogLevel::ERROR,
				"KOOK terminated websocket connection; reason: {} ({})",
				_websocket->reason().reason.c_str(),
				_websocket->reason().code);

			if (_websocket->reason().code == 4014)
			{
				logprintf(" >> plugin.kook-connector: 由于 Intent 权限问题, 机器人无法连接。请修改您的 KOOK 机器人设置并启用所有 Intent.");
				reconnect = false;
			}
			else
			{
				reconnect = true;
			}
			break;
		case asio::error::operation_aborted:
			// connection was closed, do nothing
			break;
		default:
			Logger::Get()->Log(SampLogLevel::ERROR,
				"Can't read from KOOK websocket gateway: {} ({})",
				ec.message(),
				ec.value());
			reconnect = true;
			break;
		}

		if (reconnect)
		{
			Logger::Get()->Log(SampLogLevel::INFO,
				"websocket gateway connection terminated; attempting reconnect...");
			Disconnect(true);
		}
		return;
	}

	json result = json::parse(
		beast::buffers_to_string(_buffer.data()));
	_buffer.clear();

	// KOOK signaling: s field
	int sig = -1;
	{
		auto it_s = result.find("s");
		if (it_s != result.end() && it_s->is_number_integer())
			sig = it_s->get<int>();
	}
	switch (sig)
	{
	case 0: // EVENT
	{
		auto it_sn = result.find("sn");
		if (it_sn != result.end() && (it_sn->is_number_integer() || it_sn->is_number_unsigned()))
			_sequenceNumber = it_sn->get<uint64_t>();
		json &data = result["d"];
		
		// Map KOOK event type to Discord-compatible event enum
		std::string event_type;
		if (utils::TryGetJsonValue(data, event_type, "type"))
		{
			Event mapped_event = Event::MESSAGE_CREATE; // default
			
			// KOOK event types mapping
			if (event_type == "text_msg" || event_type == "image_msg" || 
			    event_type == "video_msg" || event_type == "file_msg" ||
			    event_type == "audio_msg" || event_type == "kmarkdown_msg" ||
			    event_type == "card_msg")
			{
				mapped_event = Event::MESSAGE_CREATE;
			}
			else if (event_type == "channel_msg")
			{
				// Channel messages (system messages in channels)
				mapped_event = Event::MESSAGE_CREATE;
			}
			else if (event_type == "person_msg")
			{
				// Direct messages
				mapped_event = Event::MESSAGE_CREATE;
			}
			else if (event_type == "system_msg_add_channel" || event_type == "system_msg_delete_channel")
			{
				mapped_event = Event::CHANNEL_CREATE;
			}
			else if (event_type == "updated_channel")
			{
				mapped_event = Event::CHANNEL_UPDATE;
			}
			else if (event_type == "deleted_channel")
			{
				mapped_event = Event::CHANNEL_DELETE;
			}
			else if (event_type == "updated_guild")
			{
				mapped_event = Event::GUILD_UPDATE;
			}
			else if (event_type == "deleted_guild")
			{
				mapped_event = Event::GUILD_DELETE;
			}
			else if (event_type == "updated_guild_member")
			{
				mapped_event = Event::GUILD_MEMBER_UPDATE;
			}
			else if (event_type == "added_guild_member")
			{
				mapped_event = Event::GUILD_MEMBER_ADD;
			}
			else if (event_type == "deleted_guild_member")
			{
				mapped_event = Event::GUILD_MEMBER_REMOVE;
			}
			else if (event_type == "added_reaction" || event_type == "deleted_reaction")
			{
				// Reaction events - not directly mapped, use MESSAGE_UPDATE
				mapped_event = Event::MESSAGE_UPDATE;
			}
			else if (event_type == "updated_message")
			{
				mapped_event = Event::MESSAGE_UPDATE;
			}
			else if (event_type == "deleted_message")
			{
				mapped_event = Event::MESSAGE_DELETE;
			}
			else
			{
				// Unknown event type, log and default to MESSAGE_CREATE
				Logger::Get()->Log(SampLogLevel::DEBUG,
					"Unknown KOOK event type '{}', defaulting to MESSAGE_CREATE", event_type);
				mapped_event = Event::MESSAGE_CREATE;
			}
			
			// Dispatch to mapped event
			auto event_range = m_EventMap.equal_range(mapped_event);
			for (auto ev_it = event_range.first; ev_it != event_range.second; ++ev_it)
				ev_it->second(data);
		}
		else
		{
			// No type field, default to MESSAGE_CREATE for backward compatibility
			auto event_range = m_EventMap.equal_range(Event::MESSAGE_CREATE);
			for (auto ev_it = event_range.first; ev_it != event_range.second; ++ev_it)
				ev_it->second(data);
		}
	} break;
	case 1: // HELLO
	{
		auto &d = result["d"];
		auto it_code = d.find("code");
		if (it_code != d.end() && it_code->is_number_integer())
		{
			int code = it_code->get<int>();
			if (code != 0)
			{
				Logger::Get()->Log(SampLogLevel::ERROR, "KOOK HELLO error code: {}", code);
				Disconnect(true);
				return;
			}
		}
		{
			auto it_sid = d.find("session_id");
			if (it_sid != d.end() && it_sid->is_string())
				m_SessionId = it_sid->get<std::string>();
		}

		// Start heartbeat every 30s
		m_HeartbeatInterval = std::chrono::seconds(30);
		DoHeartbeat({});

		// Dispatch a synthetic READY event so managers can finish initialization
		// GuildManager expects a JSON with an array field "guilds"
		{
			json ready_payload;
			ready_payload["guilds"] = json::array();
			// Satisfy ChannelManager READY expectation
			ready_payload["private_channels"] = json::array();
			auto range = m_EventMap.equal_range(Event::READY);
			for (auto it = range.first; it != range.second; ++it)
				it->second(ready_payload);
		}
		auto it_guilds = result.find("d");
		if (it_guilds != result.end() && it_guilds->is_object())
		{
			auto it_items = it_guilds->find("guilds");
			if (it_items != it_guilds->end() && it_items->is_array())
			{
				for (auto &g : *it_items)
				{
					auto ig = g.find("id");
					if (ig == g.end() || !ig->is_string()) continue;
					std::string gid = ig->get<std::string>();
					// First fetch guild details (optional) and then fetch channels
					Network::Get()->Http().Get(std::string("/guild/view?guild_id=") + gid, [this, gid](Http::Response gr)
					{
						if (gr.status / 100 != 2)
						{
							Logger::Get()->Log(SampLogLevel::ERROR, "KOOK guild/view failed: {} {}", gr.status, gr.reason);
							return;
						}
						json gv = json::parse(gr.body, nullptr, false);
						if (gv.is_discarded())
						{
							Logger::Get()->Log(SampLogLevel::ERROR, "KOOK guild/view invalid JSON: {}", gr.body);
							return;
						}
						// Then fetch channels list (all types) and dispatch CHANNEL_CREATE (with type mapping)
						Network::Get()->Http().Get(std::string("/channel/list?guild_id=") + gid, [this, gid](Http::Response cr)
						{
							if (cr.status / 100 == 2)
							{
								json cj = json::parse(cr.body, nullptr, false);
								if (!cj.is_discarded())
								{
									auto cd = cj.find("data");
									if (cd != cj.end())
									{
										auto ci = cd->find("items");
										if (ci != cd->end() && ci->is_array())
										{
											for (auto &ch : *ci)
											{
												ch["guild_id"] = gid;
												int ktype = -1;
												auto it_t = ch.find("type");
												if (it_t != ch.end() && it_t->is_number_integer())
													ktype = it_t->get<int>();
												int mapped = 0; // default GUILD_TEXT
												if (ktype == 1) mapped = 0;       // text -> GUILD_TEXT
												else if (ktype == 2) mapped = 2;  // voice -> GUILD_VOICE
												else if (ktype == 0) mapped = 4;  // category -> GUILD_CATEGORY
												ch["type"] = mapped;

												// Debug log to trace caching
												std::string chid = ch.value("id", std::string());
												std::string chname = ch.value("name", std::string());
												Logger::Get()->Log(SampLogLevel::DEBUG,
													"Dispatch CHANNEL_CREATE: id='{}' name='{}' type={} guild='{}'",
													chid, chname, mapped, gid);

												// Dispatch CHANNEL_CREATE to consumers
												auto ch_range = m_EventMap.equal_range(Event::CHANNEL_CREATE);
												for (auto it = ch_range.first; it != ch_range.second; ++it)
													it->second(ch);
											}
										}
									}
								}
							}
						});
					});
				}
			}
		}
	} break;
	case 3: // PONG
		Logger::Get()->Log(SampLogLevel::DEBUG, "pong ACK");
		break;
	case 5: // RECONNECT
		Logger::Get()->Log(SampLogLevel::INFO,
			"websocket gateway requested reconnect; attempting reconnect...");
		Disconnect(true);
		return;
	case 6: // RESUME ACK
		Logger::Get()->Log(SampLogLevel::DEBUG, "resume ACK");
		break;
	default:
		Logger::Get()->Log(SampLogLevel::WARNING, "Unhandled KOOK signaling 's'={}", sig);
		Logger::Get()->Log(SampLogLevel::DEBUG, "payload: {}", result.dump(4));
		break;
	}

	Read();
}

void WebSocket::Write(std::string const &data)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::Write");

	_websocket->async_write(
		asio::buffer(data),
		beast::bind_front_handler(
			&WebSocket::OnWrite,
			this));
}

void WebSocket::OnWrite(beast::error_code ec,
	size_t bytes_transferred)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, 
		"WebSocket::OnWrite({:d})", 
		bytes_transferred);

	if (ec)
	{
		Logger::Get()->Log(SampLogLevel::ERROR,
			"Can't write to KOOK websocket gateway: {} ({})",
			ec.message(), ec.value());

		// we don't handle reconnects here, as the read handler already does this
	}
}

void WebSocket::DoHeartbeat(beast::error_code ec)
{
	Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::DoHeartbeat");

	if (ec)
	{
		switch (ec.value())
		{
		case boost::asio::error::operation_aborted:
			// timer was chancelled, do nothing
			Logger::Get()->Log(SampLogLevel::DEBUG, "heartbeat timer chancelled");
			break;
		default:
			Logger::Get()->Log(SampLogLevel::ERROR, "Heartbeat error: {} ({})",
				ec.message(), ec.value());
			break;
		}
		return;
	}

	// KOOK heartbeat (ping): { "s": 2, "sn": <last_sequence> }
	json heartbeat_payload = {
		{ "s", 2 },
		{ "sn", _sequenceNumber }
	};

	Logger::Get()->Log(SampLogLevel::DEBUG, "sending heartbeat");
	Write(heartbeat_payload.dump());

	m_HeartbeatTimer.expires_from_now(m_HeartbeatInterval);
	m_HeartbeatTimer.async_wait(
		beast::bind_front_handler(
			&WebSocket::DoHeartbeat,
			this));
}

// NOTE: For KOOK MVP we don't implement guild member chunking over WS.
// Provide a no-op to satisfy existing callers (Discord-era API).
void WebSocket::RequestGuildMembers(std::string guild_id)
{
    Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::RequestGuildMembers (KOOK no-op) - guild_id={}", guild_id);
    // TODO: Implement KOOK-specific bulk member fetching if applicable.
}

// NOTE: KOOK presence/activity is typically managed via REST or specific events.
// To keep binary compatibility during MVP, provide a no-op placeholder here.
void WebSocket::UpdateStatus(std::string const &status, std::string const &activity_name)
{
    Logger::Get()->Log(SampLogLevel::DEBUG, "WebSocket::UpdateStatus (KOOK no-op) - status='{}', activity='{}'",
        status, activity_name);
    // TODO: Implement KOOK presence/activity update via appropriate API if needed.
}
