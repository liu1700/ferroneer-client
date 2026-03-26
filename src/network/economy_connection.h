/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_connection.h WebSocket connection manager for the Ferroneer economy server. */

#ifndef NETWORK_ECONOMY_CONNECTION_H
#define NETWORK_ECONOMY_CONNECTION_H

#ifdef WITH_ECONOMY_SERVER

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>
#include <mutex>
#include <queue>
#include <functional>
#include <unordered_map>

/**
 * Manages the WebSocket connection to the Ferroneer economy server.
 *
 * This class handles:
 * - Connection lifecycle (connect, disconnect, automatic reconnection)
 * - Sending messages (Connect, CommandRequest, Ping)
 * - Receiving and queueing server responses for processing in the game loop
 * - Tracking pending command requests via request_id -> callback mapping
 */
class EconomyConnection {
public:
	/** Callback invoked when a command result is received from the server. */
	using CommandCallback = std::function<void(uint32_t request_id, bool accepted, const std::string &reason)>;

	/** Callback invoked when a world event is received from another player. */
	using WorldEventCallback = std::function<void(const nlohmann::json &event)>;

	EconomyConnection();
	~EconomyConnection();

	/**
	 * Connect to the economy server.
	 * @param url WebSocket URL (e.g. "ws://127.0.0.1:9870/ws")
	 * @param player_name The player's display name.
	 */
	void Connect(const std::string &url, const std::string &player_name);

	/** Disconnect from the economy server. */
	void Disconnect();

	/** @return true if currently connected and authenticated (received ConnectAck). */
	bool IsConnected() const;

	/** @return the player_id assigned by the server, or 0 if not connected. */
	uint32_t GetPlayerId() const;

	/**
	 * Send a game command to the economy server for validation.
	 * @param request_json The serialized command request JSON.
	 * @param callback Called when the server responds with CommandResult.
	 * @return The request_id assigned to this command.
	 */
	uint32_t SendCommand(const nlohmann::json &request_json, CommandCallback callback);

	/**
	 * Send a raw JSON message to the server.
	 * @param msg The JSON message to send.
	 */
	void Send(const nlohmann::json &msg);

	/**
	 * Process received messages. Must be called from the main game loop.
	 * Drains the internal message queue and invokes callbacks.
	 */
	void Poll();

	/**
	 * Set the callback for world events (other players' actions).
	 * @param callback Called for each WorldEvent received.
	 */
	void SetWorldEventCallback(WorldEventCallback callback);

private:
	ix::WebSocket ws;                                            ///< The ixwebsocket WebSocket instance.
	std::string player_name;                                     ///< Player name sent during Connect.
	uint32_t player_id = 0;                                      ///< Server-assigned player id.
	bool connected = false;                                      ///< Whether we have received ConnectAck.
	uint32_t next_request_id = 1;                                ///< Monotonic request id counter.

	mutable std::mutex mutex;                                    ///< Protects the message queue and callbacks.
	std::queue<nlohmann::json> incoming_queue;                   ///< Messages received from the server, awaiting processing.

	std::unordered_map<uint32_t, CommandCallback> pending_commands; ///< request_id -> callback.
	WorldEventCallback world_event_callback;                     ///< Callback for WorldEvent messages.

	/** Handle a message received on the WebSocket (called from ixwebsocket thread). */
	void OnMessage(const std::string &msg);

	/** Process a single server message (called from Poll on the main thread). */
	void ProcessMessage(const nlohmann::json &msg);
};

/** Global economy connection instance (nullptr if economy server is disabled). */
extern EconomyConnection *_economy_connection;

void EconomyConnectionInit(const std::string &url, const std::string &player_name);

#endif /* WITH_ECONOMY_SERVER */

#endif /* NETWORK_ECONOMY_CONNECTION_H */
