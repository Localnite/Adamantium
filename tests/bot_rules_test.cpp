// Standalone tests for the pure bot decision rules (no game, no Wine, no DLL build).
// g++ -std=c++17 -O1 -Wall -Wextra -I"../Project Reboot 3.0" -o bot_rules_test bot_rules_test.cpp && ./bot_rules_test
#include "BotRules.h"

#include <cstdio>
#include <string>

static int Failures = 0;

static void Check(bool Ok, const std::string& Name)
{
	if (!Ok)
	{
		++Failures;
		std::printf("FAIL  %s\n", Name.c_str());
	}
}

int main()
{
	using namespace BotRules;

	// 1. only real travel renews the stall deadline
	{
		Progress P;
		Observe(P, 0, 0, 10);
		Check(StalledFor(P, 12) == 2.0, "new progress starts the clock");

		// standing still (however many path requests) must not renew it
		for (int i = 0; i < 100; ++i) Observe(P, 0, 0, 12 + i * 0.01, 30.0);
		Check(StalledFor(P, 20) == 10.0, "no movement does not renew progress");

		// walking 30u does
		Observe(P, 30, 0, 20.5, 30.0);
		Check(StalledFor(P, 21) == 0.5, "30u of real travel renews progress");
	}

	// 2. loop detection by distance travelled, not by elapsed time
	{
		std::vector<LoopPoint> Trail;
		RememberLoopPoint(Trail, 0, 0, 0);
		Check(!IsLoop(Trail, 50, 0, 100), "near the anchor before walking far is not a loop");
		Check(IsLoop(Trail, 60, 20, 700), "returning after 700u of travel is a loop");
		Check(!IsLoop(Trail, 3000, 3000, 700), "being far away after 700u is not a loop");

		for (int i = 0; i < 20; ++i) RememberLoopPoint(Trail, i, 0, i * 100.0);
		Check(Trail.size() <= 8, "trail is bounded");
	}

	// 3. build-fight height budget
	{
		Check(!HeightCapped(0, 0, 0), "level ground: free to build");
		Check(!HeightCapped(700, 0, 0), "under two storeys above: free to build");
		Check(HeightCapped(800, 0, 0), "two storeys above the enemy: capped");
		Check(HeightCapped(1700, 1000, 0), "capped relative to the enemy too");
		Check(HeightCapped(1700, 0, 300), "four storeys above the fight's start: capped");
	}

	// 4. difficulty gate is stateless and stable within a slot
	{
		Check(StableGate(7, 3, 1, 1.0), "probability 1 always passes");
		Check(!StableGate(7, 3, 1, 0.0), "probability 0 never passes");
		const bool A = StableGate(7, 3, 1, 0.5);
		Check(A == StableGate(7, 3, 1, 0.5), "same bot and slot gives the same answer");

		int Passes = 0;
		for (uint64_t I = 0; I < 1000; ++I)
			Passes += StableGate(I, 3, 1, 0.5) ? 1 : 0;
		Check(Passes > 400 && Passes < 600, "half the bots pass at 50%");
	}

	// 5. segment test for an own shot blocker
	{
		Check(SegmentNearPoint(0, 0, 1000, 0, 500, 40, 100), "wall on the firing line blocks");
		Check(!SegmentNearPoint(0, 0, 1000, 0, 500, 400, 100), "wall far off the line does not");
		Check(SegmentNearPoint(0, 0, 1000, 0, -20, 0, 100), "wall just behind the muzzle still blocks");
	}

	// 6. one bounded interest objective
	{
		Interest I;
		Check(I.Offer(100, 200, 10), "first cue is accepted");
		Check(!I.Offer(500, 600, 11), "a second cue inside the window is dropped");
		Check(I.Active(17.9) && !I.Active(18.1), "interest lasts 8 seconds");
		Check(I.Offer(500, 600, 18.1), "a new cue is accepted after expiry");
	}

	// 7. damage bearing for an unseen attacker
	{
		double X = 0, Y = 0;
		Check(!BearingFromMomentum(0, 0, 0, 0, 800, X, Y), "zero knockback has no bearing");
		Check(BearingFromMomentum(0, 0, 0, 100, 800, X, Y), "knockback +Y gives a bearing");
		Check(std::abs(X) < 1 && Y < -799, "search point is 800u opposite the knockback");
	}

	// 8. crest braking
	{
		Check(CrestInput(0, 550, 1000) == 0.0, "at the crest there is no forward input");
		Check(CrestInput(10000, 550, 1000) == 1.0, "far from the crest: full speed");
		const double Mid = CrestInput(60, 550, 1000);
		Check(Mid > 0.12 && Mid < 1.0, "near the crest: smooth braking");
	}

	// 9. ammo pickup threat filter
	{
		Check(!PickupSafeFromThreat(400, 500), "refill next to a close threat is refused");
		Check(!PickupSafeFromThreat(1000, 200), "refill behind the threat is refused");
		Check(PickupSafeFromThreat(1000, 900), "refill away from a distant threat is allowed");
	}

	// 10. storm timing margins
	{
		const double Eta = WalkETA(0, 0, 10000, 0, 3000); // 7000u to walk, ~39 s
		Check(Eta > 30 && Eta < 50, "walk ETA is derived from the circle edge");
		Check(ShouldRotate(false, 60, 10, Eta), "start walking well before the shrink");
		Check(!ShouldRotate(false, 200, 10, Eta), "no need to rotate yet");
		Check(CanCommitFight(Eta, 80, 10), "plenty of time to finish a fight");
		Check(!CanCommitFight(Eta, 40, 10), "not enough time to finish a fight");
	}

	// 11. spatial hash
	{
		SpatialIndex<int> Index;
		Index.Add(1, 0, 0);
		Index.Add(2, 1000, 0);
		Index.Add(3, 100000, 0); // a different cell far away
		Index.Add(4, 1e12, 0);   // rejected as non-finite-world
		Check(Index.Size() == 3, "out-of-world points are rejected");

		std::vector<int> Out;
		Index.Nearby(0, 0, 1500, Out);
		Check(Out.size() == 2, "only nearby cells are returned");

		Index.Nearby(100000, 0, 100, Out);
		Check(Out.size() == 1 && Out[0] == 3, "a far cell is still found");

		Index.Nearby(0, 0, -1, Out);
		Check(Out.empty(), "negative radius returns nothing");

		Index.Clear();
		Check(Index.Size() == 0, "clear empties the index");
	}

	// 12. combat strafe goal
	{
		double X = 0, Y = 0;

		// enemy 1000u away, desired range 1000 => pure sideways step
		CombatGoal(0, 0, 1000, 0, 1000, 1, X, Y);
		Check(std::abs(X) < 1 && std::abs(Y - 300) < 1, "at range the goal is a sideways step");

		// enemy far away => closes in and steps only a little
		CombatGoal(0, 0, 5000, 0, 1000, 1, X, Y);
		Check(X > 700 && X < 900 && std::abs(Y - 120) < 1, "out of range the goal advances and strafes less");

		// enemy too close => backs off, clamped to 350
		CombatGoal(0, 0, 200, 0, 1000, -1, X, Y);
		Check(X <= -300 && std::abs(Y + 300) < 1, "too close the goal backs off");
	}

	// 13. footfall generator
	{
		Footfall F;
		Check(!F.Sample(0, 0, 0, true, 550, false), "first sample seeds");
		Check(!F.Sample(1, 0, 0.1, true, 550, false), "one unit is not a step");
		bool Emitted = false;
		for (int i = 1; i <= 10; ++i) Emitted |= F.Sample(i * 20.0, 0, 0.1 * i, true, 550, false);
		Check(Emitted, "walking 200u emits a footstep");
		Check(!F.Sample(100000, 0, 5, true, 550, false), "a teleport-like jump emits nothing");
	}

	// 14. aim alignment
	{
		Check(AimAligned(0, 3, 0, 2, 8), "small offsets are aligned");
		Check(!AimAligned(0, 10, 0, 2, 8), "yaw off by 10 is not aligned");
		Check(!AimAligned(0, 3, 0, 2, 20), "body off by 20 is not aligned");
		Check(AimAligned(179, -179, 0, 0, -179), "yaw wrap is handled");
	}

	// 15. incoming shot tube
	{
		Check(IncomingShot(0, 0, 300, 30, 1, 0), "a shot passing beside us is incoming");
		Check(!IncomingShot(0, 0, 300, 300, 1, 0), "a distant shot is not incoming");
		Check(!IncomingShot(0, 0, -300, 30, 1, 0), "a shot behind us is not incoming");
		Check(!IncomingShot(0, 0, 300, 30, 2, 0), "a non-normal direction is rejected");
	}

	// 16. arrival
	{
		Check(Reached(0, 0, 0, 50, 50, 100, 100), "inside the radius and height band");
		Check(!Reached(0, 0, 0, 50, 50, 300, 100), "too high is not arrival");
	}

	// 17. burst cadence
	{
		FireBurst B;
		double R = 0.5;
		Check(B.Update(0, false, 0.5, 1.5, 0.2, 0.4, R), "a burst starts when due");
		Check(B.Update(0.1, true, 0.5, 1.5, 0.2, 0.4, R), "held stays held");
		Check(!B.Update(1.5, true, 0.5, 1.5, 0.2, 0.4, R), "burst releases after its window");
	}

	// 18. aim error
	{
		AimError E;
		E.Roll(0, 100, 0.5, 0.5, 0.5);
		const double X = E.X;
		E.Roll(0.1, 100, 0.5, 0.5, 0.5);
		Check(E.X == X, "aim error holds until re-rolled");
		E.Roll(1.0, 100, 0.5, 0.5, 0.5);
		Check(E.X == 0.0, "perfect skill has no horizontal error");
	}

	// 19. heal safety
	{
		Check(HealSafeUseTime(true, false, false, false, 100, 550, 10, 100, false) == 0, "under fire: no heal");
		Check(HealSafeUseTime(false, true, false, false, 100, 550, 10, 100, false) == 0, "enemy visible: no heal");
		const double W = HealSafeUseTime(false, false, false, true, 2000, 550, 10, 100, true);
		Check(W > 0 && W <= 6.0, "long item in cover gets a bounded window");
		Check(HealCancelled(100, 0, false, false, false, 100, 2, 1000, 550, 2.5), "moving cancels a heal");
		Check(!HealCancelled(0, 0, false, false, false, 100, 2, 1000, 550, 2.5), "safe heal continues");
	}

	// 20. loot scoring
	{
		Check(WeaponScore(true, 90, 3, true, true, 1) > WeaponScore(false, 90, 3, true, true, 1), "usable gun dominates");
		Check(WeaponScore(true, 90, 3, true, true, 1) > WeaponScore(true, 90, 3, true, false, 1), "followup bonus");
		Check(LootCost(1000, 400, 0.45) < LootCost(1000, 400, 1.2), "needed item is cheaper");
	}

	// 21. storm policy
	{
		Check(!SafeDestination(0, 0, 5000, 0, 0, 0, 1000), "cannot move from safe ring into storm");
		Check(SafeDestination(2000, 0, 1000, 0, 0, 0, 1000), "moving toward the ring is allowed");
		Check(AvoidStormReentry(1200, 900, 1000), "staying inside is allowed");
		Check(!AvoidStormReentry(900, 1200, 1000), "re-entering the storm is refused");
		Check(ZoneWaypointDistance(0, 0, 10000, 0, 3000) <= 1200, "waypoint step is bounded");
	}

	// 22. landing math
	{
		const double T = BusDepartureTime(10, 0, 0, 3000, 0, 300, 0, 10, 20);
		Check(T > 19 && T < 21, "bus jump time is projected from velocity");
		Check(ReachableFromRoute(500, 0, 0, 0, 1000, 0, 100), "a point on the route is reachable");
		Check(!ReachableFromRoute(500, 2000, 0, 0, 1000, 0, 100), "a point far off the route is not");
	}

	// 23. damage pressure
	{
		DamagePressure P;
		Check(!P.Active(0), "no pressure initially");
		P.Add(1, 20, 100);
		Check(!P.Active(2), "one hit is not pressure");
		P.Add(2, 20, 100);
		Check(P.Active(3), "two hits in 4s create pressure");
		P.Reset();
		Check(!P.Active(3), "reset clears pressure");
	}

	// 24. squad allocator
	{
		int Next = 3, HumanTeam = 0, BotTeam = 0, Humans = 0, Bots = 0;
		Check(PickTeam(false, 2, Next, HumanTeam, BotTeam, Humans, Bots) == 3, "first human team");
		Check(PickTeam(false, 2, Next, HumanTeam, BotTeam, Humans, Bots) == 3, "second human joins the same team");
		Check(PickTeam(true, 2, Next, HumanTeam, BotTeam, Humans, Bots) == 4, "bot starts a different team");
		Check(PickTeam(true, 2, Next, HumanTeam, BotTeam, Humans, Bots) == 4, "second bot joins the bot team");
		Check(PickTeam(false, 2, Next, HumanTeam, BotTeam, Humans, Bots) == 5, "another human starts a new team");
	}

	// 25. posture / crouch rhythm and pickup wait
	{
		CrouchRhythm P;
		P.Request(CrouchMode::Healing, 0, 12, 0.3);
		Check(P.Sample(0.1, 0.3, true), "healing crouches immediately");
		Check(!P.Sample(0.1, 0.3, false), "recent damage stands the bot up");
		Check(P.Sample(0.4, 0.3, true), "healing toggles back after one tempo");
		Check(!P.Sample(0.65, 0.3, true), "and pulses down again on the off-beat");
		P.Request(CrouchMode::Harvest, 1, 6, 0.3);
		Check(P.Sample(3.0, 0.3, true), "harvest holds a steady crouch");
		P.Clear();
		Check(!P.Sample(3.1, 0.3, true), "clear stands up");
		Check(PickupWait(0.4, 0.1), "pickup claim holds the bot still");
		Check(!PickupWait(1.6, 0.1), "pickup hold expires");
	}

	if (Failures == 0)
		std::printf("bot_rules_test: all checks passed\n");
	return Failures != 0;
}
