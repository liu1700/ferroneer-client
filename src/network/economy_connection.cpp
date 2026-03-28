/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_connection.cpp Implementation of the economy server WebSocket connection. */

#include "../stdafx.h"
#include "economy_connection.h"

#ifdef WITH_ECONOMY_SERVER

#include "economy_protocol.h"
#include "economy_data.h"
#include "../debug.h"
#include "../console_func.h"

#include "../safeguards.h"

/* Global instance pointer. */
EconomyConnection *_economy_connection = nullptr;

/* Global economy data cache. */
EconomyData _economy_data;

EconomyConnection::EconomyConnection() = default;

EconomyConnection::~EconomyConnection()
{
	this->Disconnect();
}

void EconomyConnection::Connect(const std::string &url, const std::string &player_name)
{
	this->player_name = player_name;
	this->player_id = 0;
	this->connected = false;

	this->ws.setUrl(url);

	/* Enable automatic reconnection with exponential backoff. */
	this->ws.enableAutomaticReconnection();
	this->ws.setMaxWaitBetweenReconnectionRetries(10000); /* 10 seconds max */

	this->ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
			case ix::WebSocketMessageType::Message:
				this->OnMessage(msg->str);
				break;

			case ix::WebSocketMessageType::Open:
				Debug(net, 1, "[economy] WebSocket connection opened, sending handshake");
				/* Send the Connect handshake. */
				this->Send(EconomyProtocol::MakeConnect(this->player_name, "0.1.0"));
				break;

			case ix::WebSocketMessageType::Close:
				Debug(net, 1, "[economy] WebSocket connection closed (code={}, reason={})", msg->closeInfo.code, msg->closeInfo.reason);
				this->connected = false;
				this->player_id = 0;
				break;

			case ix::WebSocketMessageType::Error:
				Debug(net, 0, "[economy] WebSocket error: {}", msg->errorInfo.reason);
				break;

			default:
				break;
		}
	});

	Debug(net, 1, "[economy] Connecting to economy server at {}", url);
	this->ws.start();
}

void EconomyConnection::Disconnect()
{
	this->ws.stop();
	this->connected = false;
	this->player_id = 0;
	Debug(net, 1, "[economy] Disconnected from economy server");
}

bool EconomyConnection::IsConnected() const
{
	return this->connected;
}

uint32_t EconomyConnection::GetPlayerId() const
{
	return this->player_id;
}

uint32_t EconomyConnection::SendCommand(const nlohmann::json &request_json, CommandCallback callback)
{
	uint32_t request_id = this->next_request_id++;

	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->pending_commands[request_id] = std::move(callback);
	}

	this->Send(request_json);
	return request_id;
}

void EconomyConnection::Send(const nlohmann::json &msg)
{
	std::string payload = msg.dump();
	Debug(net, 6, "[economy] Sending: {}", payload);
	this->ws.send(payload);
}

void EconomyConnection::Poll()
{
	bool was_connected = this->was_connected;

	std::queue<nlohmann::json> to_process;

	{
		std::lock_guard<std::mutex> lock(this->mutex);
		std::swap(to_process, this->incoming_queue);
	}

	while (!to_process.empty()) {
		this->ProcessMessage(to_process.front());
		to_process.pop();
	}

	/* Detect disconnect on main thread and notify via console. */
	if (was_connected && !this->connected) {
		IConsolePrint(CC_WARNING, "[Economy] Disconnected from economy server — commands will execute locally");
	}

	this->was_connected = this->connected;
}

void EconomyConnection::SetWorldEventCallback(WorldEventCallback callback)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	this->world_event_callback = std::move(callback);
}

void EconomyConnection::OnMessage(const std::string &msg)
{
	Debug(net, 6, "[economy] Received: {}", msg);

	try {
		nlohmann::json parsed = nlohmann::json::parse(msg);
		std::lock_guard<std::mutex> lock(this->mutex);
		this->incoming_queue.push(std::move(parsed));
	} catch (const nlohmann::json::parse_error &e) {
		Debug(net, 0, "[economy] Failed to parse server message: {}", e.what());
	}
}

void EconomyConnection::ProcessMessage(const nlohmann::json &msg)
{
	EconomyProtocol::ServerMsgType type = EconomyProtocol::ParseServerMsgType(msg);

	switch (type) {
		case EconomyProtocol::ServerMsgType::ConnectAck:
			this->player_id = msg.value("player_id", 0u);
			this->connected = true;
			Debug(net, 1, "[economy] Connected! Assigned player_id={}", this->player_id);
			IConsolePrint(CC_INFO, "[Economy] Connected to economy server (player_id={})", this->player_id);
			break;

		case EconomyProtocol::ServerMsgType::CommandResult: {
			uint32_t request_id = msg.value("request_id", 0u);
			bool accepted = EconomyProtocol::IsCommandResultOk(msg);
			std::string reason = accepted ? "" : EconomyProtocol::GetRejectionReason(msg);

			Debug(net, 3, "[economy] CommandResult: request_id={}, accepted={}, reason={}", request_id, accepted, reason);

			CommandCallback cb;
			{
				std::lock_guard<std::mutex> lock(this->mutex);
				auto it = this->pending_commands.find(request_id);
				if (it != this->pending_commands.end()) {
					cb = std::move(it->second);
					this->pending_commands.erase(it);
				}
			}

			if (cb) cb(request_id, accepted, reason);
			break;
		}

		case EconomyProtocol::ServerMsgType::WorldEvent: {
			Debug(net, 3, "[economy] WorldEvent received");
			WorldEventCallback cb;
			{
				std::lock_guard<std::mutex> lock(this->mutex);
				cb = this->world_event_callback;
			}
			if (cb) cb(msg.value("event", nlohmann::json{}));
			break;
		}

		case EconomyProtocol::ServerMsgType::ServerTick:
			/* Heartbeat from server -- could track latency here. */
			break;

		case EconomyProtocol::ServerMsgType::Pong:
			/* Could measure RTT here. */
			break;

		case EconomyProtocol::ServerMsgType::EconomySnapshot: {
			_economy_data.day = msg.value("day", uint64_t{0});
			_economy_data.price_index = msg.value("price_index", uint32_t{100});
			_economy_data.total_money = msg.value("total_money", 0.0);
			_economy_data.avg_money = msg.value("avg_money", 0.0);
			_economy_data.gini = msg.value("gini", 0.0);
			_economy_data.daily_faucet = msg.value("daily_faucet", 0.0);
			_economy_data.daily_drain = msg.value("daily_drain", 0.0);
			_economy_data.daily_net = msg.value("daily_net", 0.0);

			/* Parse phase counts. */
			if (msg.contains("phase_counts")) {
				const auto &pc = msg["phase_counts"];
				_economy_data.phase_transport = pc.value("transport", uint32_t{0});
				_economy_data.phase_industry = pc.value("industry", uint32_t{0});
				_economy_data.phase_expansion = pc.value("expansion", uint32_t{0});
			}

			/* Parse commodity prices. */
			_economy_data.prices.clear();
			if (msg.contains("prices")) {
				for (auto &[key, val] : msg["prices"].items()) {
					_economy_data.prices[key] = val.get<double>();
				}
			}

			_economy_data.valid = true;

			IConsolePrint(CC_INFO, "[Economy] Day {} | Price Index: {} | Faucet: ${:.0f} | Drain: ${:.0f}",
				_economy_data.day, _economy_data.price_index,
				_economy_data.daily_faucet, _economy_data.daily_drain);

			Debug(net, 3, "[economy] Snapshot received: day={} prices={} pi={}",
				_economy_data.day, _economy_data.prices.size(), _economy_data.price_index);

			for (auto &[k, v] : _economy_data.prices) {
				Debug(net, 4, "[economy]   {} = ${:.2f}", k, v);
			}
			break;
		}

		case EconomyProtocol::ServerMsgType::PlayerEconomyState: {
			uint32_t pid = msg.value("player_id", 0u);
			double cash = msg.value("cash", 0.0);
			std::string phase = msg.value("phase", "transport");
			IConsolePrint(CC_INFO, "[Economy] Player {} | Cash: ${:.0f} | Phase: {}", pid, cash, phase);
			if (msg.contains("owned_sites")) {
				for (const auto &s : msg["owned_sites"]) {
					IConsolePrint(CC_INFO, "[Economy]   Site #{} level {} output {:.1f}/day",
						s.value("site_id", 0u), s.value("level", 1u), s.value("daily_output", 0.0));
				}
			}
			if (msg.contains("owned_factories")) {
				for (const auto &f : msg["owned_factories"]) {
					IConsolePrint(CC_INFO, "[Economy]   Factory type {} at ({},{})",
						f.value("factory_type", 0u), f.value("tile_x", 0), f.value("tile_y", 0));
				}
			}
			break;
		}

		case EconomyProtocol::ServerMsgType::ContractList: {
			if (msg.contains("contracts")) {
				IConsolePrint(CC_INFO, "[Economy] Contracts ({}):", msg["contracts"].size());
				for (const auto &c : msg["contracts"]) {
					IConsolePrint(CC_INFO, "[Economy]   #{} {} {:.0f}t {}→{} | ${:.0f} | status: {} | deadline: day {}",
						c.value("contract_id", 0ull),
						c.value("commodity", "?"),
						c.value("quantity", 0.0),
						c.value("origin_town", "?"),
						c.value("destination_town", "?"),
						c.value("payment", 0.0),
						c.value("status", "?"),
						c.value("deadline_day", 0ull));
				}
			}
			break;
		}

		case EconomyProtocol::ServerMsgType::MarketOrderList: {
			if (msg.contains("orders")) {
				IConsolePrint(CC_INFO, "[Economy] Market Orders ({}):", msg["orders"].size());
				for (const auto &o : msg["orders"]) {
					IConsolePrint(CC_INFO, "[Economy]   #{} {} {} {:.1f}t @ ${:.0f} | player {} | expires day {}",
						o.value("order_id", 0ull),
						o.value("side", "?"),
						o.value("commodity", "?"),
						o.value("quantity", 0.0),
						o.value("price_per_ton", 0.0),
						o.value("player_id", 0u),
						o.value("expires_day", 0ull));
				}
			}
			break;
		}

		case EconomyProtocol::ServerMsgType::Unknown:
			Debug(net, 0, "[economy] Unknown server message type");
			break;
	}
}

#endif /* WITH_ECONOMY_SERVER */
