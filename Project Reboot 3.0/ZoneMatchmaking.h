#pragma once

// Save the World matchmaking for a Zone server. The game finds its mission the old way: a session search on the backend, then a party
// reservation on the server's reservation beacon, then "allowed to proceed", then the connection to the game port. A real dedicated server
// set all of that up in AFortGameSession (InitHostBeacon, CreateDedicatedSession); this client build never runs that path, so the pieces are
// put together here (see ZoneMatchmaking.cpp for the addresses and how they were found).
class UObject;

namespace Zone
{
	// After the Zone world listens: open the reservation beacon (FortOnlineBeaconHost + FortPartyBeaconHost) and start registering this
	// server with the backend, which hands out the session the clients reserve against.
	void StartMatchmaking();

	// Every TickFlush frame of a Zone world: heartbeats to the backend and the delayed "allowed to proceed" after a reservation.
	void MatchmakingTick();

	// The mission is over: no more reservations, and the backend stops offering this server.
	void CloseMatchmaking();

	// The game port the world's net driver really bound (UWorld::Listen), which is what the backend hands to clients.
	void SetGamePort(int Port);

	// Whether this net driver is the reservation/lobby beacon's (it has no replication graph, so TickFlush replicates it the Reboot way).
	bool IsBeaconNetDriver(UObject* NetDriver);
}
