// Free-fly inspection character for the Xunshi walkthrough.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "XunshiInspectionCharacter.generated.h"

class UCameraComponent;

/**
 * A lightweight editor-style inspection pawn.  It deliberately uses the
 * legacy axis bindings so it can be configured entirely in DefaultInput.ini.
 */
UCLASS(config=Game)
class XUNSHI_API AXunshiInspectionCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AXunshiInspectionCharacter();

	/** Horizontal movement speed in cm/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls", meta=(ClampMin="1.0"))
	float MoveSpeed;

	/** World-space vertical movement speed in cm/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls", meta=(ClampMin="1.0"))
	float VerticalSpeed;

	/** Camera rotation sensitivity in degrees per mouse unit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls", meta=(ClampMin="0.01"))
	float RotationSpeed;

	/** Pan sensitivity in cm per mouse unit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls", meta=(ClampMin="0.01"))
	float PanSpeed;

	/** Minimum and maximum camera pitch while rotating with RMB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float MinPitch;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Inspection Controls")
	float MaxPitch;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Inspection")
	TObjectPtr<UCameraComponent> Camera;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	void MoveForward(float Value);
	void MoveRight(float Value);
	void MoveVertical(float Value);
	void MousePanX(float Value);
	void MousePanY(float Value);
	void MouseRotateX(float Value);
	void MouseRotateY(float Value);
	void BeginPan();
	void EndPan();
	void BeginRotate();
	void EndRotate();
	void UpdateCursorAndInputMode();

	bool bPanHeld = false;
	bool bRotateHeld = false;
	float ForwardInput = 0.0f;
	float RightInput = 0.0f;
	float VerticalInput = 0.0f;
};
