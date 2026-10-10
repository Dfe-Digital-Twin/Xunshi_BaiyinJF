#include "XunshiGameMode.h"
#include "XunshiInspectionPlayerController.h"
#include "UObject/ConstructorHelpers.h"

AXunshiGameMode::AXunshiGameMode()
{
	// This project intentionally starts without a controllable pawn.
	DefaultPawnClass = nullptr;
	PlayerControllerClass = AXunshiInspectionPlayerController::StaticClass();
	static ConstructorHelpers::FClassFinder<APawn> PawnClass(TEXT("/Game/Xunshi/Blueprints/Pawn/BaiyinPawn"));
	if (PawnClass.Succeeded())
	{
		DefaultPawnClass = PawnClass.Class;
	}
}
