/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_connection.cpp Implementation of the economy server WebSocket connection. */

#include "../stdafx.h"
#include "economy_connection.h"

#include "economy_protocol.h"
#include "economy_data.h"
#include "../debug.h"
#include "../console_func.h"
#include "../core/string_consumer.hpp"
#include "../ferroneer_welcome_gui.h"
#include "../company_base.h"
#include "../company_func.h"

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

			/* Apply new_cash if present (real-time balance sync on commands). */
			if (msg.contains("new_cash")) {
				double cash = msg["new_cash"].get<double>();
				_economy_data.player_cash = cash;
				_economy_data.player_cash_valid = true;
				Company *c = Company::GetIfValid(_local_company);
				if (c != nullptr) c->money = (Money)std::llround(cash);
				Debug(net, 3, "[economy] Cash updated via CommandResult: ${:.2f}", cash);
			}

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

			/* Show welcome guide once on first snapshot. */
			{
				static bool welcome_shown = false;
				if (!welcome_shown) {
					welcome_shown = true;
					ShowFerroneerWelcomeWindow();
				}
			}

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

		case EconomyProtocol::ServerMsgType::PlayerCashSync: {
			double cash = msg.value("cash", 0.0);
			double total_earned = msg.value("total_earned", 0.0);
			double total_spent = msg.value("total_spent", 0.0);

			_economy_data.player_cash = cash;
			_economy_data.player_total_earned = total_earned;
			_economy_data.player_total_spent = total_spent;
			_economy_data.player_cash_valid = true;

			/* Override Company::money with authoritative server value (Approach A). */
			Company *c = Company::GetIfValid(_local_company);
			if (c != nullptr) c->money = (Money)std::llround(cash);

			Debug(net, 3, "[economy] PlayerCashSync: cash=${:.2f} earned=${:.2f} spent=${:.2f}",
				cash, total_earned, total_spent);
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

/*
 * Console command handlers for economy server interaction.
 * Each function follows the IConsoleCmdProc signature.
 */

/** Buy a resource site. Usage: economy_buy_site <site_id> */
bool ConEconomyBuySite(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Purchase a resource site. Usage: 'economy_buy_site <site_id>'.");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	if (argv.size() != 2) {
		IConsolePrint(CC_ERROR, "Usage: economy_buy_site <site_id>");
		return true;
	}

	auto site_id = ParseInteger<uint32_t>(argv[1]);
	if (!site_id.has_value()) {
		IConsolePrint(CC_ERROR, "Invalid site_id: {}", argv[1]);
		return true;
	}

	std::string msg = EconomyProtocol::MakePurchaseResourceSite(_economy_connection->NextRequestId(), *site_id);
	_economy_connection->Send(nlohmann::json::parse(msg));
	IConsolePrint(CC_INFO, "[Economy] Sent PurchaseResourceSite for site #{}", *site_id);
	return true;
}

/** Upgrade a resource site. Usage: economy_upgrade_site <site_id> */
bool ConEconomyUpgradeSite(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Upgrade a resource site. Usage: 'economy_upgrade_site <site_id>'.");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	if (argv.size() != 2) {
		IConsolePrint(CC_ERROR, "Usage: economy_upgrade_site <site_id>");
		return true;
	}

	auto site_id = ParseInteger<uint32_t>(argv[1]);
	if (!site_id.has_value()) {
		IConsolePrint(CC_ERROR, "Invalid site_id: {}", argv[1]);
		return true;
	}

	std::string msg = EconomyProtocol::MakeUpgradeResourceSite(_economy_connection->NextRequestId(), *site_id);
	_economy_connection->Send(nlohmann::json::parse(msg));
	IConsolePrint(CC_INFO, "[Economy] Sent UpgradeResourceSite for site #{}", *site_id);
	return true;
}

/** Build a factory. Usage: economy_build_factory <type> <x> <y> */
bool ConEconomyBuildFactory(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build a factory. Usage: 'economy_build_factory <type> <x> <y>'.");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	if (argv.size() != 4) {
		IConsolePrint(CC_ERROR, "Usage: economy_build_factory <type> <x> <y>");
		return true;
	}

	auto factory_type = ParseInteger<uint8_t>(argv[1]);
	auto x = ParseInteger<int32_t>(argv[2]);
	auto y = ParseInteger<int32_t>(argv[3]);
	if (!factory_type.has_value() || !x.has_value() || !y.has_value()) {
		IConsolePrint(CC_ERROR, "Invalid arguments. type, x, y must be integers.");
		return true;
	}

	std::string msg = EconomyProtocol::MakeBuildFactory(_economy_connection->NextRequestId(), *factory_type, *x, *y);
	_economy_connection->Send(nlohmann::json::parse(msg));
	IConsolePrint(CC_INFO, "[Economy] Sent BuildFactory type={} at ({},{})", *factory_type, *x, *y);
	return true;
}

/** Place a market order. Usage: economy_place_order <buy|sell> <commodity 0-6> <qty> <price> */
bool ConEconomyPlaceOrder(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Place a market order. Usage: 'economy_place_order <buy|sell> <commodity 0-6> <qty> <price>'.");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	if (argv.size() != 5) {
		IConsolePrint(CC_ERROR, "Usage: economy_place_order <buy|sell> <commodity 0-6> <qty> <price>");
		return true;
	}

	uint8_t side;
	if (argv[1] == "buy") {
		side = 0;
	} else if (argv[1] == "sell") {
		side = 1;
	} else {
		IConsolePrint(CC_ERROR, "Side must be 'buy' or 'sell', got: {}", argv[1]);
		return true;
	}

	auto commodity = ParseInteger<uint8_t>(argv[2]);
	if (!commodity.has_value() || *commodity > 6) {
		IConsolePrint(CC_ERROR, "Commodity must be 0-6, got: {}", argv[2]);
		return true;
	}

	auto qty = ParseInteger<uint32_t>(argv[3]);
	auto price = ParseInteger<uint32_t>(argv[4]);
	if (!qty.has_value() || !price.has_value()) {
		IConsolePrint(CC_ERROR, "Quantity and price must be integers.");
		return true;
	}

	std::string msg = EconomyProtocol::MakePlaceMarketOrder(
		_economy_connection->NextRequestId(), side, *commodity,
		static_cast<double>(*qty), static_cast<double>(*price));
	_economy_connection->Send(nlohmann::json::parse(msg));
	IConsolePrint(CC_INFO, "[Economy] Sent PlaceMarketOrder {} commodity={} qty={} price={}",
		argv[1], *commodity, *qty, *price);
	return true;
}

/** Accept a contract. Usage: economy_accept_contract <contract_id> */
bool ConEconomyAcceptContract(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Accept an NPC contract. Usage: 'economy_accept_contract <contract_id>'.");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	if (argv.size() != 2) {
		IConsolePrint(CC_ERROR, "Usage: economy_accept_contract <contract_id>");
		return true;
	}

	auto contract_id = ParseInteger<uint64_t>(argv[1]);
	if (!contract_id.has_value()) {
		IConsolePrint(CC_ERROR, "Invalid contract_id: {}", argv[1]);
		return true;
	}

	std::string msg = EconomyProtocol::MakeAcceptContract(_economy_connection->NextRequestId(), *contract_id);
	_economy_connection->Send(nlohmann::json::parse(msg));
	IConsolePrint(CC_INFO, "[Economy] Sent AcceptContract #{}", *contract_id);
	return true;
}

/** Deliver goods for a contract. Usage: economy_deliver_contract <contract_id> <qty> */
bool ConEconomyDeliverContract(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Deliver goods for a contract. Usage: 'economy_deliver_contract <contract_id> <qty>'.");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	if (argv.size() != 3) {
		IConsolePrint(CC_ERROR, "Usage: economy_deliver_contract <contract_id> <qty>");
		return true;
	}

	auto contract_id = ParseInteger<uint64_t>(argv[1]);
	auto qty = ParseInteger<uint32_t>(argv[2]);
	if (!contract_id.has_value() || !qty.has_value()) {
		IConsolePrint(CC_ERROR, "Invalid arguments. contract_id and qty must be integers.");
		return true;
	}

	std::string msg = EconomyProtocol::MakeDeliverContract(
		_economy_connection->NextRequestId(), *contract_id, static_cast<double>(*qty));
	_economy_connection->Send(nlohmann::json::parse(msg));
	IConsolePrint(CC_INFO, "[Economy] Sent DeliverContract #{} qty={}", *contract_id, *qty);
	return true;
}

/** Query own player economy state. Usage: economy_query_player */
bool ConEconomyQueryPlayer(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Query your player economy state. Usage: 'economy_query_player'.");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	std::string msg = EconomyProtocol::MakeQueryPlayerEconomy();
	_economy_connection->Send(nlohmann::json::parse(msg));
	IConsolePrint(CC_INFO, "[Economy] Sent QueryPlayerEconomy");
	return true;
}

/** Query available contracts. Usage: economy_query_contracts */
bool ConEconomyQueryContracts(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Query available NPC contracts. Usage: 'economy_query_contracts'.");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	std::string msg = EconomyProtocol::MakeQueryContracts();
	_economy_connection->Send(nlohmann::json::parse(msg));
	IConsolePrint(CC_INFO, "[Economy] Sent QueryContracts");
	return true;
}

/** Query market orders. Usage: economy_query_market [commodity] */
bool ConEconomyQueryMarket(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Query market orders. Usage: 'economy_query_market [commodity]'.");
		IConsolePrint(CC_HELP, "  commodity is optional (e.g. 'IronOre', 'Coal', 'Steel').");
		return true;
	}

	if (_economy_connection == nullptr || !_economy_connection->IsConnected()) {
		IConsolePrint(CC_ERROR, "Not connected to economy server.");
		return true;
	}

	std::string commodity;
	if (argv.size() >= 2) {
		commodity = std::string(argv[1]);
	}

	std::string msg = EconomyProtocol::MakeQueryMarketOrders(commodity);
	_economy_connection->Send(nlohmann::json::parse(msg));
	if (commodity.empty()) {
		IConsolePrint(CC_INFO, "[Economy] Sent QueryMarketOrders (all)");
	} else {
		IConsolePrint(CC_INFO, "[Economy] Sent QueryMarketOrders for {}", commodity);
	}
	return true;
}
