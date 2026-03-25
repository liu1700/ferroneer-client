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
#include "../debug.h"

#include "../safeguards.h"

/* Global instance pointer. */
EconomyConnection *_economy_connection = nullptr;

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
				Debug(net, 1, "[economy] WebSocket connected to server");
				/* Send the Connect handshake. */
				this->Send(EconomyProtocol::MakeConnect(this->player_name, "0.1.0"));
				break;

			case ix::WebSocketMessageType::Close:
				Debug(net, 1, "[economy] WebSocket disconnected (code={}, reason={})", msg->closeInfo.code, msg->closeInfo.reason);
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
	std::queue<nlohmann::json> to_process;

	{
		std::lock_guard<std::mutex> lock(this->mutex);
		std::swap(to_process, this->incoming_queue);
	}

	while (!to_process.empty()) {
		this->ProcessMessage(to_process.front());
		to_process.pop();
	}
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

		case EconomyProtocol::ServerMsgType::Unknown:
			Debug(net, 0, "[economy] Unknown server message type");
			break;
	}
}

#endif /* WITH_ECONOMY_SERVER */
