//Easy Multi Save - Copyright (C) 2026 by Michael Hegemann.  

#include "EMSAsyncCheck.h"
#include "EMSObject.h"
#include "TimerManager.h"

UEMSAsyncCheck::UEMSAsyncCheck()
{
	Type = ESaveFileCheckType::CheckForGame;
	CheckResult = EIntegrityCheckResult::Success;
	bCheckGameVersion = false;
}

UEMSAsyncCheck* UEMSAsyncCheck::CheckSaveFiles(UObject* WorldContextObject, ESaveFileCheckType CheckType, FString CustomSaveName, bool bComplexCheck)
{
	if (UEMSObject* EMSObject = UEMSObject::Get(WorldContextObject))
	{
		if (!EMSObject->IsAsyncSaveOrLoadTaskActive())
		{
			UEMSAsyncCheck* CheckTask = NewObject<UEMSAsyncCheck>(GetTransientPackage());
			CheckTask->EMS = EMSObject;
			CheckTask->Type = CheckType;
			CheckTask->bCheckGameVersion = bComplexCheck;
			CheckTask->SaveFileName = CustomSaveName;
			return CheckTask;
		}
	}

	return nullptr;
}

void UEMSAsyncCheck::Activate()
{
	if (EMS)
	{
		EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncCheck::StartCheck);
	}
	else
	{
		CheckResult = EIntegrityCheckResult::Unknown;
		CompleteCheck();
	}
}

void UEMSAsyncCheck::StartCheck()
{
	switch (Type)
	{
	case ESaveFileCheckType::CheckForCustom:
		CheckCustom();
		break;

	case ESaveFileCheckType::CheckForCustomSlot:
		CheckCustomSlot();
		break;

	default:
		CheckResult = EMS->CheckSaveGameIntegrity(EMS->SlotInfoSaveFile(), bCheckGameVersion);

		if (Type == ESaveFileCheckType::CheckForSlotOnly)
		{
			EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncCheck::CompleteCheck);
		}
		else
		{
			EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncCheck::CheckPlayer);
		}
		break;
	}
}

void UEMSAsyncCheck::CheckPlayer()
{
	CheckResult = EMS->CheckSaveGameIntegrity(EMS->PlayerSaveFile(), bCheckGameVersion);
	EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncCheck::CheckLevel);
}

void UEMSAsyncCheck::CheckLevel()
{
	CheckResult = EMS->CheckSaveGameIntegrity(EMS->ActorSaveFile(), bCheckGameVersion);
	EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncCheck::CompleteCheck);
}

void UEMSAsyncCheck::CheckCustom()
{
	CheckResult = EMS->CheckSaveGameIntegrity(EMS->CustomSaveFile(SaveFileName, FString()), bCheckGameVersion);
	EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncCheck::CompleteCheck);
}

void UEMSAsyncCheck::CheckCustomSlot()
{
	CheckResult = EMS->CheckSaveGameIntegrity(EMS->CustomSaveFile(SaveFileName, EMS->GetCurrentSaveGameName()), bCheckGameVersion);
	EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncCheck::CompleteCheck);
}

void UEMSAsyncCheck::CompleteCheck()
{
	SetReadyToDestroy();

	if (CheckResult == EIntegrityCheckResult::Success)
	{
		OnCompleted.Broadcast();
		return;
	}
	else if (CheckResult == EIntegrityCheckResult::VersionMismatch)
	{
		OnVersionMismatch.Broadcast();
		return;
	}

	OnFailed.Broadcast();
}
