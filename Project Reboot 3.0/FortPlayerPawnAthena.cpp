#include "FortPlayerPawnAthena.h"
#include "FortInventory.h"
#include "FortPlayerControllerAthena.h"

void AFortPlayerPawnAthena::OnCapsuleBeginOverlapHook(UObject* Context, FFrame* Stack, void* Ret)
{
	using UPrimitiveComponent = UObject;

	// The hook is installed on both FortPlayerPawnAthena and FortPlayerPawn.
	// Only do Athena-specific logic if the pawn is actually an Athena pawn.
	auto Pawn = Cast<AFortPlayerPawnAthena>(Context);
	if (!Pawn)
	{
		// Zone pawn (FortPlayerPawn) - call original
		return OnCapsuleBeginOverlapOriginal(Context, Stack, Ret);
	}

	UPrimitiveComponent* OverlappedComp;
	AActor* OtherActor;
	UPrimitiveComponent* OtherComp;
	int OtherBodyIndex;
	bool bFromSweep;
	auto SweepResultPtr = (FHitResult*)std::realloc(0, FHitResult::GetStructSize());

	LOG_INFO(LogDev, "OnCapsuleBeginOverlapHook!");

	Stack->StepCompiledIn(&OverlappedComp);
	Stack->StepCompiledIn(&OtherActor);
	Stack->StepCompiledIn(&OtherComp);
	Stack->StepCompiledIn(&OtherBodyIndex);
	Stack->StepCompiledIn(&bFromSweep);
	Stack->StepCompiledIn(SweepResultPtr);

	std::free(SweepResultPtr);

	// LOG_INFO(LogDev, "OtherActor: {}", __int64(OtherActor));
	// LOG_INFO(LogDev, "OtherActorName: {}", OtherActor->IsValidLowLevel() ? OtherActor->GetName() : "BadRead")
	
	if (!Pawn->IsDBNO())
	{
		if (auto Pickup = Cast<AFortPickup>(OtherActor))
		{
			static auto PawnWhoDroppedPickupOffset = Pickup->GetOffset("PawnWhoDroppedPickup");

			if (Pickup->Get<AFortPawn*>(PawnWhoDroppedPickupOffset) != Pawn)
			{
				auto ItemDefinition = Pickup->GetPrimaryPickupItemEntry()->GetItemDefinition();

				if (!ItemDefinition)
				{
					return OnCapsuleBeginOverlapOriginal(Context, Stack, Ret);
				}

				if (!IsPrimaryQuickbar(ItemDefinition))
				{
					ServerHandlePickupHook(Pawn, Pickup, 0.4f, FVector(), true);
				}
			}
		}
	}

	return OnCapsuleBeginOverlapOriginal(Context, Stack, Ret);
}