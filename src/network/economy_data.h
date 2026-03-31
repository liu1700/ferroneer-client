/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_data.h Client-side cache of the latest economy snapshot from the server. */

#ifndef NETWORK_ECONOMY_DATA_H
#define NETWORK_ECONOMY_DATA_H


#include <map>
#include <string>
#include <cstdint>

/** Client-side cache of the latest economy snapshot from the server. */
struct EconomyData {
	uint64_t day = 0;
	std::map<std::string, double> prices;   ///< commodity name -> current price
	uint32_t price_index = 100;
	double total_money = 0.0;
	double avg_money = 0.0;
	double gini = 0.0;
	double daily_faucet = 0.0;
	double daily_drain = 0.0;
	double daily_net = 0.0;

	/** Phase distribution: transport / industry / expansion players. */
	uint32_t phase_transport = 0;
	uint32_t phase_industry = 0;
	uint32_t phase_expansion = 0;

	bool valid = false;  ///< true once the first snapshot has been received

	/* Per-player cash sync from the economy server. */
	double player_cash = 0.0;          ///< authoritative cash balance
	double player_total_earned = 0.0;  ///< lifetime earnings
	double player_total_spent = 0.0;   ///< lifetime spending
	bool player_cash_valid = false;    ///< true once first cash sync received
};

extern EconomyData _economy_data;

#endif /* NETWORK_ECONOMY_DATA_H */
