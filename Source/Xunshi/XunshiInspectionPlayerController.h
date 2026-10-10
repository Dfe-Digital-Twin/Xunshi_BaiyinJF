#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "XunshiInspectionPlayerController.generated.h"

class UCameraComponent;

UCLASS()
class XUNSHI_API AXunshiInspectionPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AXunshiInspectionPlayerController();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float MoveSpeed = 850.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float VerticalSpeed = 850.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float RotationSpeed = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float PanSpeed = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float ZoomSpeed = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float MinPitch = -89.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float MaxPitch = 89.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float MinFieldOfView = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float MaxFieldOfView = 90.0f;

protected:
	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaSeconds) override;
	virtual void SetupInputComponent() override;

private:
	void MoveForward(float Value);
	void MoveRight(float Value);
	void MoveVertical(float Value);
	void MouseX(float Value);
	void MouseY(float Value);
	void Zoom(float Value);
	void PanPressed();
	void PanReleased();
	void RotatePressed();
	void RotateReleased();

	float ForwardInput = 0.0f;
	float RightInput = 0.0f;
	float VerticalInput = 0.0f;
	bool bPanHeld = false;
	bool bRotateHeld = false;
};
