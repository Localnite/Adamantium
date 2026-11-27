#pragma once

// Pure decision rules shared by the bot brain and its standalone unit test.
// No engine types, no globals, no side effects: everything is derived from the arguments.
// Adapted from the design of Syndicate's BotNavigation::Progress / BotRoute::Memory /
// CombatSkillPolicy / BotDifficulty (see README credits). The rules below are ours, but the
// separation of "intent" from "observed progress" and the stateless difficulty gate are theirs.

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace BotRules
{
	inline double Dist2D(double Ax, double Ay, double Bx, double By)
	{
		return std::sqrt((Ax - Bx) * (Ax - Bx) + (Ay - By) * (Ay - By));
	}

	// ---- 1. observed progress, not request acceptance (Syndicate BotNavigation::Progress) ----
	// A path being accepted, a build being committed or a timer being rearmed is not movement.
	// Only a real displacement of at least MinStep counts as renewing the deadline.
	struct Progress
	{
		double X = 0.0, Y = 0.0; // position at the last counted step
		double Residual = 0.0;   // path length walked since that position
		double Mark = 0.0;       // time of the last counted step
		bool bInit = false;
	};

	inline void Observe(Progress& P, double X, double Y, double Now, double MinStep = 30.0)
	{
		if (!P.bInit)
		{
			P.X = X; P.Y = Y; P.Mark = Now; P.bInit = true;
			return;
		}

		P.Residual += Dist2D(P.X, P.Y, X, Y);
		P.X = X; P.Y = Y;

		if (P.Residual >= MinStep)
		{
			P.Residual = 0.0;
			P.Mark = Now; // only real travel renews this; callers must never write Mark themselves
		}
	}

	inline double StalledFor(const Progress& P, double Now)
	{
		return P.bInit ? Now - P.Mark : 0.0;
	}

	// ---- 2. loop detection by travelled distance, not by time (Syndicate BotRoute::Memory) ----
	struct LoopPoint { double X = 0.0, Y = 0.0, Travel = 0.0; };

	//true when the bot has walked at least MinTravel since dropping a point and has now come back
	//within Radius of it. A bot pacing in a 300u circle for 20 s is caught here, but one that is
	//genuinely crossing a long route is not.
	inline bool IsLoop(const std::vector<LoopPoint>& Trail, double X, double Y, double Travel,
		double MinTravel = 500.0, double Radius = 150.0)
	{
		for (const auto& P : Trail)
			if (Travel - P.Travel >= MinTravel && Dist2D(P.X, P.Y, X, Y) <= Radius)
				return true;
		return false;
	}

	inline void RememberLoopPoint(std::vector<LoopPoint>& Trail, double X, double Y, double Travel, size_t Max = 8)
	{
		Trail.push_back({ X, Y, Travel });
		if (Trail.size() > Max)
			Trail.erase(Trail.begin());
	}

	// ---- 3. build-fight height budget (Syndicate CombatSkillPolicy) ----
	// Without a cap two duellists ramp above each other forever. Stop escalating once the bot is
	// two storeys above the enemy, or four storeys above where the fight started, whichever is lower.
	inline bool HeightCapped(double BotZ, double EnemyZ, double BaseZ, double Storey = 384.0)
	{
		return BotZ >= std::fmin(EnemyZ + 2.0 * Storey, BaseZ + 4.0 * Storey);
	}

	// ---- 4. stateless difficulty gate (Syndicate BotDifficulty::Allow) ----
	// Stable per bot and per 2-second slot, so a behaviour does not flicker on/off inside a window.
	// No RNG state to seed or reset; the integer hash is the whole implementation.
	inline bool StableGate(uint64_t Id, uint64_t Slot, uint64_t Key, double Probability)
	{
		if (Probability >= 1.0) return true;
		if (Probability <= 0.0) return false;

		const uint64_t H = (Slot * 2654435761ull + Id * 2246822519ull + Key * 3266489917ull) % 100;
		return H < static_cast<uint64_t>(Probability * 100.0);
	}

	// ---- 5. own-shot occlusion, cheap 2D version of Syndicate's muzzle trace ----
	// A wall this bot just built should not let it fire through itself. The full engine version
	// traces from the muzzle; this pure form is the segment test used when a trace is not available.
	inline bool SegmentNearPoint(double Ax, double Ay, double Bx, double By, double Px, double Py, double Radius)
	{
		const double Dx = Bx - Ax, Dy = By - Ay;
		const double Len2 = Dx * Dx + Dy * Dy;
		if (Len2 < 1e-6) return Dist2D(Ax, Ay, Px, Py) <= Radius;

		double T = ((Px - Ax) * Dx + (Py - Ay) * Dy) / Len2;
		if (T < 0.0) T = 0.0;
		else if (T > 1.0) T = 1.0;

		return Dist2D(Ax + T * Dx, Ay + T * Dy, Px, Py) <= Radius;
	}

	// ---- 6. one bounded interest objective (Syndicate BotTactics::Offer) ----
	// Several gunshots in a row must not make a bot flip its investigation target every frame.
	// One 8-second objective wins; new cues are dropped until it expires.
	struct Interest
	{
		double X = 0.0, Y = 0.0;
		double Until = 0.0;
		bool Active(double Now) const { return Now < Until; }
		bool Offer(double X_, double Y_, double Now, double Seconds = 8.0)
		{
			if (Active(Now)) return false;
			X = X_; Y = Y_; Until = Now + Seconds;
			return true;
		}
		void Clear() { Until = 0.0; }
	};

	// ---- 7. damage bearing for an unseen attacker (Syndicate PlayerBots::RecordDamage) ----
	// A hit from cover gives a direction, not the shooter's live position. Derived from the damage
	// knockback so the bot searches a plausible bearing instead of appearing omniscient.
	inline bool BearingFromMomentum(double HereX, double HereY, double PushX, double PushY,
		double Range, double& OutX, double& OutY)
	{
		const double Len = std::sqrt(PushX * PushX + PushY * PushY);
		if (Len < 1e-3 || Range <= 0.0) return false;
		OutX = HereX - PushX / Len * Range; // knockback points away from the shooter
		OutY = HereY - PushY / Len * Range;
		return true;
	}

	// ---- 8. crest approach braking (Syndicate BotSkillPolicy::CrestInput) ----
	// Instead of full sprint then hard brake at a ledge, ease off continuously as the remaining
	// distance approaches the distance needed to stop. Returns a multiplier in [0.12, 1].
	inline double CrestInput(double Remaining, double Speed, double Braking)
	{
		if (Remaining <= 12.0) return 0.0;
		const double Stop = Speed * Speed / (2.0 * (std::fmax)(100.0, Braking)) + Speed / 30.0 + 12.0;
		double T = (Remaining - 12.0) / (std::fmax)(40.0, Stop);
		if (T < 0.12) T = 0.12;
		else if (T > 1.0) T = 1.0;
		return T;
	}

	// ---- 9. ammo pickup must not sit next to a threat (Syndicate BotTactics::AmmoEscapeTarget) ----
	inline bool PickupSafeFromThreat(double ThreatDist, double PickupDistFromThreat,
		double MinThreatDist = 800.0)
	{
		return ThreatDist >= MinThreatDist && PickupDistFromThreat + 150.0 >= ThreatDist;
	}

	// ---- 10. storm rotation timing with separate "start walking" / "finish the fight" margins
	// (Syndicate BotTactics::ShouldRotate / CanCommitFight) ----
	inline double WalkETA(double HereX, double HereY, double CenterX, double CenterY, double Radius)
	{
		const double Inset = (std::fmin)(600.0, Radius * 0.25);
		return (std::fmax)(0.0, Dist2D(HereX, HereY, CenterX, CenterY) - (std::fmax)(0.0, Radius - Inset)) / 180.0;
	}

	inline bool ShouldRotate(bool InStorm, double ShrinkStarts, double Now, double Eta)
	{
		return InStorm || ShrinkStarts <= Now + Eta + 120.0;
	}

	inline bool CanCommitFight(double Eta, double ShrinkAt, double Now)
	{
		return Eta <= 0.0 || ShrinkAt - Now > Eta + 25.0;
	}

	// ---- 11. spatial hash for local actor queries (Syndicate BotSpatial::Index) ----
	// Stores indices, never pointers: the caller owns the vector and rebuilds this when it changes.
	// Nearby() only walks the cells touching the query circle, so cost depends on local density,
	// not on the size of the world's actor list.
	template <class T>
	class SpatialIndex
	{
		struct Entry { T Item; double X, Y; };
		std::unordered_map<uint64_t, std::vector<Entry>> Cells;
		static constexpr double CellSize = 2000.0;

		static int Cell(double V) { return static_cast<int>(std::floor(V / CellSize)); }

		static uint64_t Key(int X, int Y)
		{
			return (static_cast<uint64_t>(static_cast<uint32_t>(X)) << 32) | static_cast<uint32_t>(Y);
		}

	public:
		void Clear() { Cells.clear(); }

		void Add(const T& Item, double X, double Y)
		{
			if (!std::isfinite(X) || !std::isfinite(Y) || std::fabs(X) > 1e7 || std::fabs(Y) > 1e7) return;
			Cells[Key(Cell(X), Cell(Y))].push_back({ Item, X, Y });
		}

		void Nearby(double X, double Y, double Radius, std::vector<T>& Out) const
		{
			Out.clear();
			if (!std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Radius)
				|| Radius < 0.0 || Radius > 20000.0 || std::fabs(X) > 1e7 || std::fabs(Y) > 1e7)
				return;

			const double Square = Radius * Radius;

			for (int Cx = Cell(X - Radius); Cx <= Cell(X + Radius); ++Cx)
			{
				for (int Cy = Cell(Y - Radius); Cy <= Cell(Y + Radius); ++Cy)
				{
					const auto It = Cells.find(Key(Cx, Cy));
					if (It == Cells.end()) continue;

					for (const auto& E : It->second)
					{
						const double Dx = E.X - X, Dy = E.Y - Y;
						if (Dx * Dx + Dy * Dy <= Square) Out.push_back(E.Item);
					}
				}
			}
		}

		size_t Size() const
		{
			size_t N = 0;
			for (const auto& Cell : Cells) N += Cell.second.size();
			return N;
		}
	};

	// ---- 12. combat advance/strafe goal (Syndicate BotTactics::CombatGoal) ----
	// A target point combining approach/retreat with a sideways step. Feeding this to the movement
	// controller produces organic circle-strafing without a pathfinder. Side should flip every 4-7 s.
	inline void CombatGoal(double HereX, double HereY, double EnemyX, double EnemyY,
		double Range, double Side, double& OutX, double& OutY)
	{
		const double Dx = EnemyX - HereX, Dy = EnemyY - HereY;
		const double Length = (std::fmax)(1.0, std::sqrt(Dx * Dx + Dy * Dy));
		const double Ux = Dx / Length, Uy = Dy / Length;

		double Advance = Length - Range;
		if (Advance < -350.0) Advance = -350.0;
		else if (Advance > 800.0) Advance = 800.0;

		const double Strafe = Length > Range * 1.4 ? 120.0 : 300.0;
		OutX = HereX + Ux * Advance - Uy * (Side * Strafe);
		OutY = HereY + Uy * Advance + Ux * (Side * Strafe);
	}

	// ---- 13. distance-accumulated footfall (Syndicate BotTactics::Footfall) ----
	// Turns continuous movement into discrete noise events and rejects airborne travel,
	// network teleports and stationary shuffling. Emits every 110u (130u crouched).
	struct Footfall
	{
		double X = 0.0, Y = 0.0, At = 0.0, Travel = 0.0;
		bool Seeded = false;

		bool Sample(double X_, double Y_, double Now, bool Grounded, double Speed, bool Crouched)
		{
			if (!std::isfinite(X_) || !std::isfinite(Y_) || !std::isfinite(Now)) { Seeded = false; return false; }
			if (!Seeded) { X = X_; Y = Y_; At = Now; Seeded = true; return false; }

			const double Dt = Now - At;
			const double D = Dist2D(X, Y, X_, Y_);
			X = X_; Y = Y_; At = Now;

			if (!Grounded || Speed < 70.0 || Dt <= 0.0 || Dt > 1.0
				|| D > (std::fmax)(200.0, Speed * Dt * 1.6) || D < 2.0)
			{
				Travel = 0.0;
				return false;
			}

			Travel += D;

			if (Travel >= (Crouched ? 130.0 : 110.0))
			{
				Travel = 0.0;
				return true;
			}

			return false;
		}
	};

	inline double WrappedDegrees(double A)
	{
		while (A > 180.0) A -= 360.0;
		while (A < -180.0) A += 360.0;
		return A;
	}

	// ---- 14. aim alignment (Syndicate BotTactics::AimAligned) ----
	inline bool AimAligned(double DesiredYaw, double ControlYaw, double DesiredPitch, double ControlPitch, double BodyYaw)
	{
		return std::fabs(WrappedDegrees(DesiredYaw - ControlYaw)) <= 7.0
			&& std::fabs(WrappedDegrees(DesiredPitch - ControlPitch)) <= 7.0
			&& std::fabs(WrappedDegrees(DesiredYaw - BodyYaw)) <= 15.0;
	}

	// ---- 15. incoming shot tube (Syndicate BotTactics::IncomingShot) ----
	inline bool IncomingShot(double FromX, double FromY, double HereX, double HereY,
		double DirX, double DirY, double AlongMin = 100.0, double AlongMax = 6000.0, double Radius = 180.0)
	{
		const double Norm = std::sqrt(DirX * DirX + DirY * DirY);
		if (Norm < 0.9 || Norm > 1.1) return false;

		const double Dx = HereX - FromX, Dy = HereY - FromY;
		const double Along = (Dx * DirX + Dy * DirY) / Norm;
		if (Along <= AlongMin || Along >= AlongMax) return false;

		const double Perp2 = Dx * Dx + Dy * Dy - Along * Along;
		return Perp2 >= -1.0 && Perp2 < Radius * Radius;
	}

	// ---- 16. universal arrival (Syndicate BotTactics::Reached) ----
	inline bool Reached(double HereX, double HereY, double HereZ, double GoalX, double GoalY, double GoalZ, double Radius)
	{
		return Dist2D(HereX, HereY, GoalX, GoalY) <= Radius && std::fabs(HereZ - GoalZ) <= 140.0;
	}

	// ---- 17. burst fire cadence (Syndicate Combat) ----
	struct FireBurst
	{
		double BurstEnd = 0.0, NextBurst = 0.0;

		bool Update(double Now, bool Held, double OnMin, double OnMax, double OffMin, double OffMax,
			double& Random01)
		{
			if (Held && Now > BurstEnd)
			{
				BurstEnd = 0.0;
				NextBurst = Now + OffMin + (OffMax - OffMin) * Random01;
				return false; // release
			}

			if (!Held && Now >= NextBurst)
			{
				BurstEnd = Now + OnMin + (OnMax - OnMin) * Random01;
				return true; // press
			}

			return Held;
		}
	};

	// ---- 18. periodic aim error re-roll (Syndicate PlayerBots::Combat) ----
	struct AimError
	{
		double X = 0.0, Y = 0.0, Z = 0.0, NextAt = 0.0;

		void Roll(double Now, double Skill, double Random01, double Random02, double Random03)
		{
			if (Now < NextAt) return;
			NextAt = Now + 0.45 + 0.45 * Random01;
			const double Scale = 0.2 + Skill / 100.0; // higher skill = smaller error
			X = (Random01 * 2.0 - 1.0) * 14.0 * Scale;
			Y = (Random02 * 2.0 - 1.0) * 14.0 * Scale;
			Z = 32.0 + 26.0 * Random03;
		}
	};

	// ---- 19. heal time-to-contact (Syndicate BotSustain::SafeUseTime) ----
	inline double HealSafeUseTime(bool UnderFire, bool EnemyVisible, bool InStorm, bool InCover,
		double EnemyDist, double ClosingSpeed, double SinceDamage, double ZoneSlack, bool LongItem)
	{
		if (UnderFire || EnemyVisible || InStorm) return 0.0;

		const double Contact = EnemyDist / (std::fmax)(550.0, ClosingSpeed) + (InCover ? 2.5 : 0.0);
		double Window = (std::fmin)(Contact, ZoneSlack) - 1.0;
		if (Window < 0.0) Window = 0.0;

		if (LongItem)
		{
			if (SinceDamage < 3.0) return InCover ? (std::fmin)(Window, 3.5) : 0.0;
		}
		else if (!InCover && EnemyDist < 2000.0 && SinceDamage < 3.0)
			return (std::fmin)(Window, 3.5);

		return Window;
	}

	// true when an in-progress heal must be abandoned (Syndicate BotSustain::PlanHeal)
	inline bool HealCancelled(double Moved, double VitalsLost, bool EnemyVisible, bool UnderFire, bool InStorm,
		double ZoneSlackLeft, double TimeLeft, double EnemyDist, double ClosingSpeed, double CoverDelay)
	{
		if (Moved > 60.0 || VitalsLost > 0.5 || EnemyVisible || UnderFire || InStorm) return true;
		if (ZoneSlackLeft < TimeLeft) return true;
		return EnemyDist / (std::fmax)(550.0, ClosingSpeed) + CoverDelay < TimeLeft;
	}

	// ---- 20. loot / weapon scoring (Syndicate BotLootPlan + BotMission) ----
	inline double WeaponScore(bool Usable, double RangeTier, double Rarity, bool Loaded, bool Followup, int Role)
	{
		return (Usable ? 1000.0 : 0.0) + RangeTier + (std::fmin)(Rarity, 10.0) * 2.0
			+ (Followup && Role == 1 ? 70.0 : 0.0) + (Loaded ? 8.0 : 0.0);
	}

	inline double LootCost(double Distance, double HeightDelta, double Factor)
	{
		const double C = Distance + (std::fmax)(0.0, HeightDelta - 150.0) * 1.5;
		return C * Factor; // Factor: gun missing .45 / upgrade .7 / extra 1.2, heal .6 / 1.3, etc.
	}

	// ---- 21. storm rotation policy (Syndicate BotTactics) ----
	inline bool SafeDestination(double HereX, double HereY, double TargetX, double TargetY,
		double SafeX, double SafeY, double SafeRadius)
	{
		const double HereD = Dist2D(HereX, HereY, SafeX, SafeY);
		const double TargetD = Dist2D(TargetX, TargetY, SafeX, SafeY);

		if (HereD <= SafeRadius) return TargetD <= SafeRadius;
		return TargetD < HereD || TargetD <= 1200.0;
	}

	inline bool AvoidStormReentry(double HereR, double TargetR, double SafeRadius)
	{
		return HereR > SafeRadius || TargetR <= SafeRadius - (std::fmin)(40.0, SafeRadius * 0.05) || TargetR <= HereR;
	}

	inline double ZoneWaypointDistance(double HereX, double HereY, double CenterX, double CenterY, double Radius)
	{
		const double D = Dist2D(HereX, HereY, CenterX, CenterY);
		const double Inset = (std::fmin)(900.0, Radius * 0.3);
		const double Edge = (std::fmax)(0.0, Radius - Inset);
		return (std::fmin)(1200.0, (std::fmax)(0.0, D - Edge));
	}

	// ---- 22. landing math (Syndicate BotLanding) ----
	inline double BusDepartureTime(double Now, double BusX, double BusY, double TargetX, double TargetY,
		double Vx, double Vy, double Begin, double End)
	{
		const double Speed2 = Vx * Vx + Vy * Vy;
		double Closest = Speed2 > 1.0 ? Now + ((TargetX - BusX) * Vx + (TargetY - BusY) * Vy) / Speed2 : Begin;
		const double Lo = (std::fmax)(Now, Begin);
		const double Hi = (std::fmax)(Lo, End);
		return Closest < Lo ? Lo : (Closest > Hi ? Hi : Closest);
	}

	inline bool ReachableFromRoute(double TargetX, double TargetY, double StartX, double StartY,
		double EndX, double EndY, double Range)
	{
		const double Dx = EndX - StartX, Dy = EndY - StartY;
		const double L2 = Dx * Dx + Dy * Dy;
		if (L2 < 1.0) return Dist2D(TargetX, TargetY, StartX, StartY) <= Range;

		double T = ((TargetX - StartX) * Dx + (TargetY - StartY) * Dy) / L2;
		if (T < 0.0) T = 0.0;
		else if (T > 1.0) T = 1.0;

		return Dist2D(TargetX, TargetY, StartX + Dx * T, StartY + Dy * T) <= Range;
	}

	// ---- 23. successive-hit pressure (Syndicate CombatSkills::ConfirmPressureHit) ----
	struct DamagePressure
	{
		int Hits = 0;
		double At = 0.0, Until = 0.0;

		void Add(double Now, double Damage, double HealthAndShield)
		{
			if (Now - At > 4.0) Hits = 0;
			++Hits;
			At = Now;
			if (Hits >= 2 || (Damage > 0.0 && HealthAndShield - Damage <= 65.0))
				Until = Now + 10.0;
		}

		bool Active(double Now) const { return Now < Until; }
		void Reset() { Hits = 0; Until = 0.0; }
	};

	// ---- 24. squad allocator ( Syndicate Validation/team-test.cpp) ----
	// Humans and bots share one increasing team counter but never mix in the same team.
	// ---- 25. posture / crouch rhythm (Syndicate BotTactics::Posture + CrouchRhythm) ----
	enum class CrouchMode { Standing, Harvest, Pickup, Healing };

	struct CrouchRhythm
	{
		CrouchMode Mode = CrouchMode::Standing;
		double Until = 0.0, PhaseAt = 0.0;
		bool Down = false;

		void Request(CrouchMode NewMode, double Now, double Duration, double Tempo)
		{
			if (NewMode == Mode && Now < Until) return;
			Mode = NewMode;
			Until = Now + Duration;
			Down = NewMode != CrouchMode::Standing;
			PhaseAt = Now + Tempo;
		}

		bool Sample(double Now, double Tempo, bool Permitted)
		{
			if (!Permitted || Mode == CrouchMode::Standing || Now >= Until)
			{
				Down = false;
				return false;
			}

			if (Now >= PhaseAt)
			{
				// Healing pulses (a real player shifts); harvesting/pickup holds steady.
				if (Mode == CrouchMode::Healing) Down = !Down;
				PhaseAt = Now + (Mode == CrouchMode::Healing ? Tempo * 0.8 : Tempo);
			}

			return Down;
		}

		void Clear() { Mode = CrouchMode::Standing; Down = false; }
	};

	inline bool PickupWait(double Now, double Started, double Hold = 1.5)
	{
		return Started > 0.0 && Now >= Started && Now - Started < Hold;
	}

	inline int PickTeam(bool IsBot, int SquadSize, int& NextTeam, int& HumanTeam, int& BotTeam,
		int& HumanCount, int& BotCount)
	{
		int& Count = IsBot ? BotCount : HumanCount;
		int& Team = IsBot ? BotTeam : HumanTeam;

		if (Team == 0 || Count >= SquadSize)
		{
			Team = NextTeam++;
			Count = 0;
		}

		++Count;
		return Team;
	}
}
