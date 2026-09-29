#include "XunshiInspectionCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/InputSettings.h"
#include "InputCoreTypes.h"

AXunshiInspectionCharacter::AXunshiInspectionCharacter()
	: MoveSpeed(850.0f)
	, VerticalSpeed(850.0f)
	, RotationSpeed(0.18f)
	, PanSpeed(2.0f)
	, MinPitch(-89.0f)
	, MaxPitch(89.0f)
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(34.0f, 88.0f);

	// A free-fly inspection pawn should not be pulled down by gravity.
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->GravityScale = 0.0f;
	Movement->SetMovementMode(MOVE_Flying);
	Movement->MaxFlySpeed = MoveSpeed;
	Movement->MaxAcceleration = 1000000.0f;
	Movement->BrakingDecelerationFlying = 1000000.0f;
	Movement->bOrientRotationToMovement = false;

	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("InspectionCamera"));
	Camera->SetupAttachment(GetCapsuleComponent());
	Camera->SetRelativeLocation(FVector(0.0f, 0.0f, 55.0f));
	Camera->bUsePawnControlRotation = false;

	AutoPossessPlayer = EAutoReceiveInput::Player0;
}

void AXunshiInspectionCharacter::BeginPlay()
{
	Super::BeginPlay();
	GetCharacterMovement()->MaxFlySpeed = FMath::Max(MoveSpeed, VerticalSpeed);

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->bShowMouseCursor = true;
		PC->bEnableClickEvents = true;
		PC->bEnableMouseOverEvents = true;
		// GameOnly still receives mouse button/axis input, while bShowMouseCursor
		// keeps the cursor visible for an inspection-style control scheme.
		FInputModeGameOnly InputMode;
		PC->SetInputMode(InputMode);
		PC->bShowMouseCursor = true;
	}
}

void AXunshiInspectionCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Read accumulated mouse movement while held. This remains reliable after
	// the initial click changes the viewport's capture state.
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		float MouseDeltaX = 0.0f;
		float MouseDeltaY = 0.0f;
		PC->GetInputMouseDelta(MouseDeltaX, MouseDeltaY);

		if (bPanHeld)
		{
			AddActorWorldOffset(-GetActorRightVector() * MouseDeltaX * PanSpeed, true);
			AddActorWorldOffset(GetActorUpVector() * MouseDeltaY * PanSpeed, true);
		}
		if (bRotateHeld)
		{
			AddActorLocalRotation(FRotator(0.0f, MouseDeltaX * RotationSpeed, 0.0f));
			if (Camera)
			{
				FRotator Rotation = Camera->GetRelativeRotation();
				Rotation.Pitch = FMath::ClampAngle(Rotation.Pitch + MouseDeltaY * RotationSpeed, MinPitch, MaxPitch);
				Camera->SetRelativeRotation(Rotation);
			}
		}
	}

	const float MaxSpeed = FMath::Max(FMath::Max(MoveSpeed, VerticalSpeed), 1.0f);
	const FRotator YawOnly(0.0f, GetActorRotation().Yaw, 0.0f);
	const FVector Direction = FRotationMatrix(YawOnly).GetUnitAxis(EAxis::X) * ForwardInput * MoveSpeed
		+ FRotationMatrix(YawOnly).GetUnitAxis(EAxis::Y) * RightInput * MoveSpeed
		+ FVector::UpVector * VerticalInput * VerticalSpeed;

	if (!Direction.IsNearlyZero())
	{
		// Direct translation intentionally avoids CharacterMovement acceleration/inertia.
		AddActorWorldOffset(Direction * DeltaSeconds, true);
	}
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->Velocity = FVector::ZeroVector;
		Movement->MaxFlySpeed = MaxSpeed;
	}
}

void AXunshiInspectionCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	PlayerInputComponent->BindAxis(TEXT("MoveForward"), this, &AXunshiInspectionCharacter::MoveForward);
	PlayerInputComponent->BindAxis(TEXT("MoveRight"), this, &AXunshiInspectionCharacter::MoveRight);
	PlayerInputComponent->BindAxis(TEXT("MoveVertical"), this, &AXunshiInspectionCharacter::MoveVertical);

	PlayerInputComponent->BindAction(TEXT("Pan"), IE_Pressed, this, &AXunshiInspectionCharacter::BeginPan);
	PlayerInputComponent->BindAction(TEXT("Pan"), IE_Released, this, &AXunshiInspectionCharacter::EndPan);
	PlayerInputComponent->BindAction(TEXT("Rotate"), IE_Pressed, this, &AXunshiInspectionCharacter::BeginRotate);
	PlayerInputComponent->BindAction(TEXT("Rotate"), IE_Released, this, &AXunshiInspectionCharacter::EndRotate);
}

void AXunshiInspectionCharacter::MoveForward(float Value)
{
	ForwardInput = FMath::Clamp(Value, -1.0f, 1.0f);
}

void AXunshiInspectionCharacter::MoveRight(float Value)
{
	RightInput = FMath::Clamp(Value, -1.0f, 1.0f);
}

void AXunshiInspectionCharacter::MoveVertical(float Value)
{
	VerticalInput = FMath::Clamp(Value, -1.0f, 1.0f);
}

void AXunshiInspectionCharacter::MousePanX(float Value)
{
	if (bPanHeld && !FMath::IsNearlyZero(Value))
	{
		AddActorWorldOffset(-GetActorRightVector() * Value * PanSpeed, true);
	}
}

void AXunshiInspectionCharacter::MousePanY(float Value)
{
	if (bPanHeld && !FMath::IsNearlyZero(Value))
	{
		AddActorWorldOffset(GetActorUpVector() * Value * PanSpeed, true);
	}
}

void AXunshiInspectionCharacter::MouseRotateX(float Value)
{
	if (bRotateHeld && !FMath::IsNearlyZero(Value))
	{
		AddActorLocalRotation(FRotator(0.0f, Value * RotationSpeed, 0.0f));
	}
}

void AXunshiInspectionCharacter::MouseRotateY(float Value)
{
	if (bRotateHeld && Camera && !FMath::IsNearlyZero(Value))
	{
		FRotator Rotation = Camera->GetRelativeRotation();
		Rotation.Pitch = FMath::ClampAngle(Rotation.Pitch - Value * RotationSpeed, MinPitch, MaxPitch);
		Camera->SetRelativeRotation(Rotation);
	}
}

void AXunshiInspectionCharacter::BeginPan() { bPanHeld = true; UpdateCursorAndInputMode(); }
void AXunshiInspectionCharacter::EndPan() { bPanHeld = false; }
void AXunshiInspectionCharacter::BeginRotate() { bRotateHeld = true; UpdateCursorAndInputMode(); }
void AXunshiInspectionCharacter::EndRotate() { bRotateHeld = false; }

void AXunshiInspectionCharacter::UpdateCursorAndInputMode()
{
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		FInputModeGameOnly InputMode;
		InputMode.SetConsumeCaptureMouseDown(false);
		PC->SetInputMode(InputMode);
		PC->bShowMouseCursor = true;
	}
}
