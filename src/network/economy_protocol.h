/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_protocol.h Message protocol for communication with the Ferroneer economy server. */

#ifndef NETWORK_ECONOMY_PROTOCOL_H
#define NETWORK_ECONOMY_PROTOCOL_H

#ifdef WITH_ECONOMY_SERVER

#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>

/** Messages sent from client to economy server. */
namespace EconomyProtocol {

/** Serialize a Connect message. */
inline nlohmann::json MakeConnect(const std::string &player_name, const std::string &client_version)
{
	return {
		{"type", "Connect"},
		{"player_name", player_name},
		{"client_version", client_version}
	};
}

/** Serialize a BuildRoad command request. */
inline nlohmann::json MakeBuildRoad(uint32_t request_id, int32_t start_x, int32_t start_y, int32_t end_x, int32_t end_y, uint8_t road_type)
{
	return {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"cmd", "BuildRoad"},
			{"start_x", start_x},
			{"start_y", start_y},
			{"end_x", end_x},
			{"end_y", end_y},
			{"road_type", road_type}
		}}
	};
}

/** Serialize a BuildStation command request. */
inline nlohmann::json MakeBuildStation(uint32_t request_id, int32_t x, int32_t y, uint8_t station_type)
{
	return {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"cmd", "BuildStation"},
			{"x", x},
			{"y", y},
			{"station_type", station_type}
		}}
	};
}

/** Serialize a Ping message. */
inline nlohmann::json MakePing(uint64_t timestamp)
{
	return {
		{"type", "Ping"},
		{"timestamp", timestamp}
	};
}

/** Parsed server response types. */
enum class ServerMsgType {
	ConnectAck,
	CommandResult,
	WorldEvent,
	ServerTick,
	Pong,
	EconomySnapshot,
	PlayerEconomyState,
	ContractList,
	MarketOrderList,
	Unknown,
};

/** Parse the "type" field from a server JSON message. */
inline ServerMsgType ParseServerMsgType(const nlohmann::json &msg)
{
	std::string type = msg.value("type", "");
	if (type == "ConnectAck") return ServerMsgType::ConnectAck;
	if (type == "CommandResult") return ServerMsgType::CommandResult;
	if (type == "WorldEvent") return ServerMsgType::WorldEvent;
	if (type == "ServerTick") return ServerMsgType::ServerTick;
	if (type == "Pong") return ServerMsgType::Pong;
	if (type == "EconomySnapshot") return ServerMsgType::EconomySnapshot;
	if (type == "PlayerEconomyState") return ServerMsgType::PlayerEconomyState;
	if (type == "ContractList") return ServerMsgType::ContractList;
	if (type == "MarketOrderList") return ServerMsgType::MarketOrderList;
	return ServerMsgType::Unknown;
}

/** Check if a CommandResult has status "Ok". */
inline bool IsCommandResultOk(const nlohmann::json &msg)
{
	if (!msg.contains("status")) return false;
	const auto &status = msg["status"];
	return status.value("result", "") == "Ok";
}

/** Get the rejection reason from a CommandResult. Returns empty string if Ok. */
inline std::string GetRejectionReason(const nlohmann::json &msg)
{
	if (!msg.contains("status")) return "unknown";
	const auto &status = msg["status"];
	return status.value("reason", "");
}

/** Serialize a PurchaseResourceSite command request. */
inline std::string MakePurchaseResourceSite(uint32_t request_id, uint32_t site_id)
{
	nlohmann::json msg = {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"type", "PurchaseResourceSite"},
			{"site_id", site_id}
		}}
	};
	return msg.dump();
}

/** Serialize an UpgradeResourceSite command request. */
inline std::string MakeUpgradeResourceSite(uint32_t request_id, uint32_t site_id)
{
	nlohmann::json msg = {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"type", "UpgradeResourceSite"},
			{"site_id", site_id}
		}}
	};
	return msg.dump();
}

/** Serialize a BuildFactory command request. */
inline std::string MakeBuildFactory(uint32_t request_id, uint8_t factory_type, int32_t tile_x, int32_t tile_y)
{
	nlohmann::json msg = {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"type", "BuildFactory"},
			{"factory_type", factory_type},
			{"tile_x", tile_x},
			{"tile_y", tile_y}
		}}
	};
	return msg.dump();
}

/** Serialize a DeliverCargo command request. */
inline std::string MakeDeliverCargo(uint32_t request_id, uint8_t cargo_type, double quantity,
	int32_t origin_x, int32_t origin_y, int32_t dest_x, int32_t dest_y, uint32_t travel_ticks)
{
	nlohmann::json msg = {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"type", "DeliverCargo"},
			{"cargo_type", cargo_type},
			{"quantity", quantity},
			{"origin_x", origin_x},
			{"origin_y", origin_y},
			{"dest_x", dest_x},
			{"dest_y", dest_y},
			{"travel_ticks", travel_ticks}
		}}
	};
	return msg.dump();
}

/** Serialize a PlaceMarketOrder command request. */
inline std::string MakePlaceMarketOrder(uint32_t request_id, uint8_t side, uint8_t commodity,
	double quantity, double price)
{
	nlohmann::json msg = {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"type", "PlaceMarketOrder"},
			{"side", side},
			{"commodity", commodity},
			{"quantity", quantity},
			{"price_per_ton", price}
		}}
	};
	return msg.dump();
}

/** Serialize an AcceptContract command request. */
inline std::string MakeAcceptContract(uint32_t request_id, uint64_t contract_id)
{
	nlohmann::json msg = {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"type", "AcceptContract"},
			{"contract_id", contract_id}
		}}
	};
	return msg.dump();
}

/** Serialize a DeliverContract command request. */
inline std::string MakeDeliverContract(uint32_t request_id, uint64_t contract_id, double quantity)
{
	nlohmann::json msg = {
		{"type", "CommandRequest"},
		{"request_id", request_id},
		{"command", {
			{"type", "DeliverContract"},
			{"contract_id", contract_id},
			{"quantity", quantity}
		}}
	};
	return msg.dump();
}

/** Serialize a QueryPlayerEconomy message. */
inline std::string MakeQueryPlayerEconomy()
{
	nlohmann::json msg = {
		{"type", "QueryPlayerEconomy"}
	};
	return msg.dump();
}

/** Serialize a QueryContracts message. */
inline std::string MakeQueryContracts()
{
	nlohmann::json msg = {
		{"type", "QueryContracts"}
	};
	return msg.dump();
}

/** Serialize a QueryMarketOrders message with optional commodity filter. */
inline std::string MakeQueryMarketOrders(const std::string &commodity = "")
{
	nlohmann::json msg = {
		{"type", "QueryMarketOrders"}
	};
	if (!commodity.empty()) msg["commodity"] = commodity;
	return msg.dump();
}

/**
 * Map OpenTTD CargoID labels to Ferroneer CommodityType indices.
 * Temperate climate only for MVP.
 * @return commodity index (0-6) or 0xFF if unsupported
 */
inline uint8_t MapCargoToEconomyCommodity(const std::string &label)
{
	if (label == "IORE") return 0; /* IronOre */
	if (label == "COAL") return 1; /* Coal */
	if (label == "WOOD") return 2; /* Timber */
	if (label == "GRAI" || label == "WHEA") return 3; /* Crops */
	if (label == "STEL") return 4; /* Steel */
	if (label == "GOOD") return 5; /* Lumber (mapped) */
	if (label == "FOOD") return 6; /* Food */
	return 0xFF; /* Unsupported */
}

} /* namespace EconomyProtocol */

#endif /* WITH_ECONOMY_SERVER */

#endif /* NETWORK_ECONOMY_PROTOCOL_H */
