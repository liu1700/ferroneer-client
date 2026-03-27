/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_send.h Economy server command routing. */

#ifndef NETWORK_ECONOMY_SEND_H
#define NETWORK_ECONOMY_SEND_H

#ifdef WITH_ECONOMY_SERVER

#include "../command_type.h"
#include "../company_type.h"

/**
 * Send a command to the economy server for validation.
 * @param cmd The command type.
 * @param err_message Error message string ID.
 * @param callback Callback for when the command completes.
 * @param company The company issuing the command.
 * @param cmd_data Serialized command arguments.
 * @return true if the command was sent (economy server is connected), false to fall through to local execution.
 */
bool NetworkSendEconomyCommand(Commands cmd, StringID err_message,
	CommandCallback *callback, CompanyID company,
	const CommandDataBuffer &cmd_data);

/**
 * Compile-time check whether a command should be routed to the economy server.
 * Non-economy commands eliminate this branch entirely at compile time.
 * @param cmd The command to check.
 * @return true if this is an economy-relevant command.
 */
constexpr bool IsEconomyServerCommand(Commands cmd)
{
	switch (cmd) {
		case Commands::BuildRoad:
		case Commands::BuildRoadLong:
			return true;
		default:
			return false;
	}
}

#endif /* WITH_ECONOMY_SERVER */
#endif /* NETWORK_ECONOMY_SEND_H */
