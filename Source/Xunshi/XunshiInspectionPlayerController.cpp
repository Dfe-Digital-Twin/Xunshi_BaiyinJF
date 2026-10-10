#include "XunshiInspectionPlayerController.h"

#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/InputSettings.h"
#include "InputCoreTypes.h"

AXunshiInspectionPlayerController::AXunshiInspectionPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
}

void AXunshiInspectionPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (APawn* ControlledPawn = GetPawn())
	{
		// The inspection pawn is a camera carrier. Make controller rotation drive
		// the pawn even when the Blueprint defaults were not configured for it.
		ControlledPawn->bUseControllerRotationPitch = true;
		ControlledPawn->bUseControllerRotationYaw = true;
		ControlledPawn->bUseControllerRotationRoll = false;
	}
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
}

void AXunshiInspectionPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	InputComponent->BindAxis(TEXT("MoveForward"), this, &AXunshiInspectionPlayerController::MoveForward);
	InputComponent->BindAxis(TEXT("MoveRight"), this, &AXunshiInspectionPlayerController::MoveRight);
	InputComponent->BindAxis(TEXT("MoveVertical"), this, &AXunshiInspectionPlayerController::MoveVertical);
	InputComponent->BindAxis(TEXT("Turn"), this, &AXunshiInspectionPlayerController::MouseX);
	InputComponent->BindAxis(TEXT("LookUp"), this, &AXunshiInspectionPlayerController::MouseY);
	InputComponent->BindAxis(TEXT("Zoom"), this, &AXunshiInspectionPlayerController::Zoom);
	InputComponent->BindAction(TEXT("Pan"), IE_Pressed, this, &AXunshiInspectionPlayerController::PanPressed);
	InputComponent->BindAction(TEXT("Pan"), IE_Released, this, &AXunshiInspectionPlayerController::PanReleased);
	InputComponent->BindAction(TEXT("Rotate"), IE_Pressed, this, &AXunshiInspectionPlayerController::RotatePressed);
	InputComponent->BindAction(TEXT("Rotate"), IE_Released, this, &AXunshiInspectionPlayerController::RotateReleased);
}

void AXunshiInspectionPlayerController::PlayerTick(float DeltaSeconds)
{
	Super::PlayerTick(DeltaSeconds);
	APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn)
	{
		return;
	}

	const FRotator YawRotation(0.0f, GetControlRotation().Yaw, 0.0f);
	const FVector Translation = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X) * ForwardInput * MoveSpeed
		+ FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y) * RightInput * MoveSpeed
		+ FVector::UpVector * VerticalInput * VerticalSpeed;
	if (!Translation.IsNearlyZero())
	{
		ControlledPawn->AddActorWorldOffset(Translation * DeltaSeconds, true);
	}
}

void AXunshiInspectionPlayerController::MoveForward(float Value) { ForwardInput = FMath::Clamp(Value, -1.0f, 1.0f); }
void AXunshiInspectionPlayerController::MoveRight(float Value) { RightInput = FMath::Clamp(Value, -1.0f, 1.0f); }
void AXunshiInspectionPlayerController::MoveVertical(float Value) { VerticalInput = FMath::Clamp(Value, -1.0f, 1.0f); }

void AXunshiInspectionPlayerController::MouseX(float Value)
{
	if (bRotateHeld && !FMath::IsNearlyZero(Value))
	{
		AddYawInput(Value * RotationSpeed);
	}
	if (bPanHeld && !FMath::IsNearlyZero(Value))
	{
		if (UCameraComponent* Camera = GetPawn() ? GetPawn()->FindComponentByClass<UCameraComponent>() : nullptr)
		{
			GetPawn()->AddActorWorldOffset(-Camera->GetRightVector() * Value * PanSpeed, true);
		}
	}
}

void AXunshiInspectionPlayerController::MouseY(float Value)
{
	if (bRotateHeld && !FMath::IsNearlyZero(Value))
	{
		FRotator Rotation = GetControlRotation();
		Rotation.Pitch = FMath::ClampAngle(Rotation.Pitch - Value * RotationSpeed, MinPitch, MaxPitch);
		Rotation.Roll = 0.0f;
		SetControlRotation(Rotation);
	}
	if (bPanHeld && !FMath::IsNearlyZero(Value))
	{
		if (UCameraComponent* Camera = GetPawn() ? GetPawn()->FindComponentByClass<UCameraComponent>() : nullptr)
		{
			GetPawn()->AddActorWorldOffset(Camera->GetUpVector() * Value * PanSpeed, true);
		}
	}
}

void AXunshiInspectionPlayerController::Zoom(float Value)
{
	if (UCameraComponent* Camera = GetPawn() ? GetPawn()->FindComponentByClass<UCameraComponent>() : nullptr)
	{
		Camera->SetFieldOfView(FMath::Clamp(Camera->FieldOfView - Value * ZoomSpeed, MinFieldOfView, MaxFieldOfView));
	}
}

void AXunshiInspectionPlayerController::PanPressed() { bPanHeld = true; }
void AXunshiInspectionPlayerController::PanReleased() { bPanHeld = false; }
void AXunshiInspectionPlayerController::RotatePressed() { bRotateHeld = true; }
void AXunshiInspectionPlayerController::RotateReleased() { bRotateHeld = false; }
