/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_protocol_test.cpp Tests for economy server protocol serialization. */

#include "../stdafx.h"

#ifdef WITH_ECONOMY_SERVER

#include "../3rdparty/catch2/catch.hpp"

#include "../network/economy_protocol.h"

#include "../safeguards.h"

/* --- MakeConnect tests --- */

TEST_CASE("EconomyProtocol MakeConnect generates valid JSON")
{
	auto msg = EconomyProtocol::MakeConnect("Alice", "0.1.0");

	CHECK(msg["type"] == "Connect");
	CHECK(msg["player_name"] == "Alice");
	CHECK(msg["client_version"] == "0.1.0");
}

TEST_CASE("EconomyProtocol MakeConnect with empty strings")
{
	auto msg = EconomyProtocol::MakeConnect("", "");

	CHECK(msg["type"] == "Connect");
	CHECK(msg["player_name"] == "");
	CHECK(msg["client_version"] == "");
}

/* --- MakeBuildRoad tests --- */

TEST_CASE("EconomyProtocol MakeBuildRoad generates valid JSON")
{
	auto msg = EconomyProtocol::MakeBuildRoad(42, 10, 20, 30, 40, 1);

	CHECK(msg["type"] == "CommandRequest");
	CHECK(msg["request_id"] == 42);
	CHECK(msg["command"]["cmd"] == "BuildRoad");
	CHECK(msg["command"]["start_x"] == 10);
	CHECK(msg["command"]["start_y"] == 20);
	CHECK(msg["command"]["end_x"] == 30);
	CHECK(msg["command"]["end_y"] == 40);
	CHECK(msg["command"]["road_type"] == 1);
}

/* --- MakeBuildStation tests --- */

TEST_CASE("EconomyProtocol MakeBuildStation generates valid JSON")
{
	auto msg = EconomyProtocol::MakeBuildStation(99, 5, 15, 3);

	CHECK(msg["type"] == "CommandRequest");
	CHECK(msg["request_id"] == 99);
	CHECK(msg["command"]["cmd"] == "BuildStation");
	CHECK(msg["command"]["x"] == 5);
	CHECK(msg["command"]["y"] == 15);
	CHECK(msg["command"]["station_type"] == 3);
}

/* --- MakePing tests --- */

TEST_CASE("EconomyProtocol MakePing generates valid JSON")
{
	auto msg = EconomyProtocol::MakePing(1700000000000ULL);

	CHECK(msg["type"] == "Ping");
	CHECK(msg["timestamp"] == 1700000000000ULL);
}

/* --- ParseServerMsgType tests --- */

TEST_CASE("EconomyProtocol ParseServerMsgType identifies ConnectAck")
{
	nlohmann::json msg = {{"type", "ConnectAck"}, {"player_id", 1}, {"server_version", "0.1.0"}};
	CHECK(EconomyProtocol::ParseServerMsgType(msg) == EconomyProtocol::ServerMsgType::ConnectAck);
}

TEST_CASE("EconomyProtocol ParseServerMsgType identifies CommandResult")
{
	nlohmann::json msg = {{"type", "CommandResult"}, {"request_id", 1}, {"status", {{"result", "Ok"}}}};
	CHECK(EconomyProtocol::ParseServerMsgType(msg) == EconomyProtocol::ServerMsgType::CommandResult);
}

TEST_CASE("EconomyProtocol ParseServerMsgType identifies WorldEvent")
{
	nlohmann::json msg = {{"type", "WorldEvent"}, {"event", {{"event_type", "RoadBuilt"}}}};
	CHECK(EconomyProtocol::ParseServerMsgType(msg) == EconomyProtocol::ServerMsgType::WorldEvent);
}

TEST_CASE("EconomyProtocol ParseServerMsgType identifies ServerTick")
{
	nlohmann::json msg = {{"type", "ServerTick"}, {"tick_id", 100}, {"timestamp", 1700000000000ULL}};
	CHECK(EconomyProtocol::ParseServerMsgType(msg) == EconomyProtocol::ServerMsgType::ServerTick);
}

TEST_CASE("EconomyProtocol ParseServerMsgType identifies Pong")
{
	nlohmann::json msg = {{"type", "Pong"}, {"timestamp", 123456ULL}};
	CHECK(EconomyProtocol::ParseServerMsgType(msg) == EconomyProtocol::ServerMsgType::Pong);
}

TEST_CASE("EconomyProtocol ParseServerMsgType returns Unknown for bad type")
{
	nlohmann::json msg = {{"type", "SomethingWeird"}};
	CHECK(EconomyProtocol::ParseServerMsgType(msg) == EconomyProtocol::ServerMsgType::Unknown);
}

TEST_CASE("EconomyProtocol ParseServerMsgType returns Unknown for missing type")
{
	nlohmann::json msg = {{"data", "no type field"}};
	CHECK(EconomyProtocol::ParseServerMsgType(msg) == EconomyProtocol::ServerMsgType::Unknown);
}

/* --- IsCommandResultOk tests --- */

TEST_CASE("EconomyProtocol IsCommandResultOk returns true for Ok")
{
	nlohmann::json msg = {{"type", "CommandResult"}, {"request_id", 1}, {"status", {{"result", "Ok"}}}};
	CHECK(EconomyProtocol::IsCommandResultOk(msg) == true);
}

TEST_CASE("EconomyProtocol IsCommandResultOk returns false for Rejected")
{
	nlohmann::json msg = {{"type", "CommandResult"}, {"request_id", 1}, {"status", {{"result", "Rejected"}, {"reason", "out of bounds"}}}};
	CHECK(EconomyProtocol::IsCommandResultOk(msg) == false);
}

/* --- GetRejectionReason tests --- */

TEST_CASE("EconomyProtocol GetRejectionReason extracts reason")
{
	nlohmann::json msg = {{"type", "CommandResult"}, {"request_id", 1}, {"status", {{"result", "Rejected"}, {"reason", "insufficient funds"}}}};
	CHECK(EconomyProtocol::GetRejectionReason(msg) == "insufficient funds");
}

/* --- Round-trip test --- */

TEST_CASE("EconomyProtocol round-trip: MakeBuildRoad -> dump -> parse -> verify")
{
	auto original = EconomyProtocol::MakeBuildRoad(7, 100, 200, 300, 400, 2);
	std::string json_str = original.dump();
	auto parsed = nlohmann::json::parse(json_str);

	CHECK(parsed["type"] == "CommandRequest");
	CHECK(parsed["request_id"] == 7);
	CHECK(parsed["command"]["cmd"] == "BuildRoad");
	CHECK(parsed["command"]["start_x"] == 100);
	CHECK(parsed["command"]["start_y"] == 200);
	CHECK(parsed["command"]["end_x"] == 300);
	CHECK(parsed["command"]["end_y"] == 400);
	CHECK(parsed["command"]["road_type"] == 2);
}

#endif /* WITH_ECONOMY_SERVER */
