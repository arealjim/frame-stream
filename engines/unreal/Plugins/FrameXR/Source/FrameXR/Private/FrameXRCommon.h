// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

// Every OpenXR entry point is fetched through xrGetInstanceProcAddr from a loader we dlopen ourselves:
// nothing links against the loader, so a Mac without OXRSys just leaves the plugin dormant.
#define XR_NO_PROTOTYPES 1
#define XR_USE_GRAPHICS_API_METAL 1
THIRD_PARTY_INCLUDES_START
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
THIRD_PARTY_INCLUDES_END

DECLARE_LOG_CATEGORY_EXTERN(LogFrameXR, Log, All);

/**
 * A rigid pose in OpenXR conventions (+X right, +Y up, -Z forward, metres, right-handed).
 * FQuat/FVector are used purely as maths containers here; convert with FrameXR::ToUnreal*.
 */
struct FXrPose
{
	FQuat Orientation = FQuat::Identity;
	FVector Position = FVector::ZeroVector;

	FXrPose() = default;
	FXrPose(const FQuat& InOrientation, const FVector& InPosition)
		: Orientation(InOrientation), Position(InPosition)
	{
	}

	explicit FXrPose(const XrPosef& Pose)
		: Orientation(Pose.orientation.x, Pose.orientation.y, Pose.orientation.z, Pose.orientation.w)
		, Position(Pose.position.x, Pose.position.y, Pose.position.z)
	{
		Orientation.Normalize();
	}

	XrPosef ToXr() const
	{
		XrPosef P;
		P.orientation = { (float)Orientation.X, (float)Orientation.Y, (float)Orientation.Z, (float)Orientation.W };
		P.position = { (float)Position.X, (float)Position.Y, (float)Position.Z };
		return P;
	}

	FXrPose Inverse() const
	{
		const FQuat Inv = Orientation.Inverse();
		return FXrPose(Inv, -Inv.RotateVector(Position));
	}

	/** this * Other: Other expressed in this pose's frame, brought into the parent frame. */
	FXrPose operator*(const FXrPose& Other) const
	{
		return FXrPose(Orientation * Other.Orientation, Position + Orientation.RotateVector(Other.Position));
	}
};

namespace FrameXR
{
	/**
	 * OpenXR -> Unreal basis change: X_ue = -Z, Y_ue = X, Z_ue = Y. It flips handedness (det = -1), so a
	 * rotation's axis maps as an axial vector: q_ue = (qz, -qx, -qy, qw) == (-qz, qx, qy, -qw).
	 */
	inline FVector ToUnrealPosition(const FVector& P, float WorldToMeters)
	{
		return FVector(-P.Z, P.X, P.Y) * WorldToMeters;
	}

	inline FQuat ToUnrealRotation(const FQuat& Q)
	{
		return FQuat(-Q.Z, Q.X, Q.Y, -Q.W);
	}

	/** The inverses: Unreal -> OpenXR (a stereo layer's transform into its OpenXR pose). */
	inline FVector FromUnrealPosition(const FVector& P, float WorldToMeters)
	{
		return FVector(P.Y, P.Z, -P.X) / WorldToMeters;
	}

	inline FQuat FromUnrealRotation(const FQuat& Q)
	{
		return FQuat(Q.Y, Q.Z, -Q.X, -Q.W);
	}

	/** Heading of a pose (rotation about +Y only), plus ExtraYaw radians. */
	inline FQuat YawOnly(const FQuat& Orientation, double ExtraYaw = 0.0)
	{
		const FVector Forward = Orientation.RotateVector(FVector(0, 0, -1));
		const double Yaw = FMath::Atan2(-Forward.X, -Forward.Z) + ExtraYaw;
		return FQuat(FVector(0, 1, 0), Yaw);
	}
}
