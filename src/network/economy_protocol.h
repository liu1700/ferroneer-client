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

} /* namespace EconomyProtocol */

#endif /* WITH_ECONOMY_SERVER */

#endif /* NETWORK_ECONOMY_PROTOCOL_H */
