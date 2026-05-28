//Easy Multi Save - Copyright (C) 2026 by Michael Hegemann.  

#include "EMSAsyncSaveGame.h"
#include "EMSObject.h"
#include "EMSData.h"
#include "EMSMisc.h"
#include "Modules/ModuleManager.h"
#include "Engine/Engine.h"
#include "Async/Async.h"
#include "TimerManager.h"

/**
Init
**/

UEMSAsyncSaveGame::UEMSAsyncSaveGame()
{
	Mode = ESaveGameMode::MODE_All;
	Data = 0;
	bIsActive = false;
	bFinishedStep = false;
	bHasFailed = false;
	bAutoSaveLevel = false;
	bMemoryOnly = false;
}

UWorld* UEMSAsyncSaveGame::GetWorld() const
{
	//This is used for ShouldCancelSaveTask
	if (EMS)
	{
		return EMS->GetWorld();
	}

	return nullptr;
}

/**
Activation
**/

UEMSAsyncSaveGame* UEMSAsyncSaveGame::AsyncSaveActors(UObject* WorldContextObject, int32 Data)
{
	if (UEMSObject* EMSObject = UEMSObject::Get(WorldContextObject))
	{
		const ESaveGameMode Mode = FAsyncSaveHelpers::GetMode(Data);

		if (EMSObject->IsAsyncSaveOrLoadTaskActive(Mode))
		{
			return nullptr;
		}

		UEMSAsyncSaveGame* SaveTask = NewObject<UEMSAsyncSaveGame>(GetTransientPackage());
		if (SaveTask)
		{
			SaveTask->EMS = EMSObject;
			SaveTask->Data = Data;
			SaveTask->Mode = Mode;
			return SaveTask;
		}	
	}

	return nullptr;
}

void UEMSAsyncSaveGame::AutoSaveLevelActors(UEMSObject* EMSObject)
{
	UEMSAsyncSaveGame* SaveTask = NewObject<UEMSAsyncSaveGame>(GetTransientPackage());
	if (SaveTask)
	{
		SaveTask->EMS = EMSObject;
		SaveTask->Data = TOFLAG(ESaveTypeFlags::SF_Level);
		SaveTask->Mode = ESaveGameMode::MODE_Level;
		SaveTask->bAutoSaveLevel = true;
		SaveTask->bMemoryOnly = FSettingHelpers::IsMemoryOnlySave();
		SaveTask->RegisterWithGameInstance(EMSObject);
		SaveTask->Activate();
	}
}

void UEMSAsyncSaveGame::Activate()
{
	//No data
	if (Data <= 0)
	{
		UE_LOG(LogEasyMultiSave, Warning, TEXT("Save Game Actors has no Data Flags selected."));
		bHasFailed = true;
		CompleteSavingTask();
		return;
	}

	if (EMS)
	{
		//Cancel when still streaming
		if (CheckLevelStreaming())
		{
			bHasFailed = true;
			FinishSaving();
			return;
		}

		bIsActive = true;

		//For World Partition auto-saving, we will only save placed Actors in the cells, prepare is also very costly
		if (!bAutoSaveLevel)
		{
			EMS->PrepareLoadAndSaveActors(Data, EAsyncCheckType::CT_Save, EPrepareType::PT_Default);
		}

		EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncSaveGame::StartSaving);
	}
}

void UEMSAsyncSaveGame::StartSaving()
{
	bHasFailed = false;

	if (EMS)
	{
		const bool bSaveToMemory = bAutoSaveLevel && bMemoryOnly;
		if (!bSaveToMemory)
		{
			//Save current slot
			EMS->SaveSlotInfoObject(EMS->GetCurrentSaveGameName());
		}

		EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncSaveGame::SavePlayer);
	}
}

/**
Player
**/

void UEMSAsyncSaveGame::SavePlayer()
{
	bFinishedStep = false;

	if (EMS)
	{
		if (FSettingHelpers::IsMultiThreadSaving())
		{
			CurrentSaveTask = FFunctionGraphTask::CreateAndDispatchWhenReady([this]()
			{
				InternalSavePlayer();

			}, TStatId(), nullptr, ENamedThreads::AnyNormalThreadNormalTask);
		}
		else
		{
			InternalSavePlayer();
		}

		TryMoveToNextStep(ENextStepType::SaveLevel);
	}
}

void UEMSAsyncSaveGame::InternalSavePlayer()
{
	if (EMSFLAG::IsPlayer(Data))
	{
		if (!EMS->SavePlayerActors(EMS->GetPlayerController(), EMS->PlayerSaveFile()))
		{
			bHasFailed = true;
		}
	}

	bFinishedStep = true;
}

/**
Level
**/

void UEMSAsyncSaveGame::SaveLevel()
{
	bFinishedStep = false;

	if (EMS)
	{
		if (FSettingHelpers::IsMultiThreadSaving())
		{
			CurrentSaveTask = FFunctionGraphTask::CreateAndDispatchWhenReady([this]()
			{
				InternalSaveLevel();

			}, TStatId(), nullptr, ENamedThreads::AnyNormalThreadNormalTask);
		}
		else
		{
			InternalSaveLevel();
		}

		TryMoveToNextStep(ENextStepType::FinishSave);
	}
}

void UEMSAsyncSaveGame::InternalSaveLevel()
{
	if (EMSFLAG::IsLevel(Data))
	{
		const bool bPrevHasFailed = bHasFailed;

		if(EMS->SaveLevelActors(bMemoryOnly))
		{
			bHasFailed = false;
		}
		else
		{
			bHasFailed = bPrevHasFailed;
		}
	}

	bFinishedStep = true;
}

/**
Finish
**/

void UEMSAsyncSaveGame::FinishSaving()
{
	if (EMS)
	{
		bIsActive = false;
		EMS->GetTimerManager().SetTimerForNextTick(this, &UEMSAsyncSaveGame::CompleteSavingTask);
	}
}

void UEMSAsyncSaveGame::CompleteSavingTask()
{
	if (bHasFailed)
	{
		OnFailed.Broadcast();
	}
	else
	{
		OnCompleted.Broadcast();
	}

	SetReadyToDestroy();
}

void UEMSAsyncSaveGame::ForceDestroy()
{
	if (CurrentSaveTask.IsValid() && !CurrentSaveTask->IsComplete())
	{
		FTaskGraphInterface::Get().WaitUntilTaskCompletes(CurrentSaveTask);
	}

	CurrentSaveTask.SafeRelease();
	FPlatformProcess::Sleep(0.01f);

	if (EMS)
	{
		bIsActive = false;
		EMS->GetTimerManager().ClearAllTimersForObject(this);
	}

	SetReadyToDestroy();
}

/**
Helper Functions
**/

void UEMSAsyncSaveGame::TryMoveToNextStep(ENextStepType Step)
{
	//This is used to delay further execution until multi-thread code finished, but without blocking.

	if (FAsyncSaveHelpers::ShouldCancelSaveTask(this))
	{
		return;
	}

	//For multi thread save we need weak ref
	TWeakObjectPtr<UEMSAsyncSaveGame> WeakThis(this);

	FTimerDelegate TimerDelegate;
	TimerDelegate.BindLambda([WeakThis, Step]()
	{
		//Check if EMS is still there
		UEMSAsyncSaveGame* StrongThis = WeakThis.Get();
		if (!StrongThis || FAsyncSaveHelpers::ShouldCancelSaveTask(StrongThis))
		{
			return;
		}

		if (StrongThis->bFinishedStep)
		{
			if (Step == ENextStepType::FinishSave)
			{
				//Make sure its valid
				if (StrongThis->EMS)
				{
					StrongThis->EMS->GetTimerManager().SetTimerForNextTick(StrongThis, &UEMSAsyncSaveGame::FinishSaving);
				}
			}
			else
			{
				if (StrongThis->EMS)
				{
					StrongThis->EMS->GetTimerManager().SetTimerForNextTick(StrongThis, &UEMSAsyncSaveGame::SaveLevel);
				}
			}
		}
		else
		{
			//Recursive call 
			StrongThis->TryMoveToNextStep(Step);
		}
	});

	if (EMS)
	{
		EMS->GetTimerManager().SetTimerForNextTick(TimerDelegate);
	}
}

bool UEMSAsyncSaveGame::CheckLevelStreaming()
{
	if (!EMS)
	{
		return false;
	}

	//Not relevant for WP auto save
	if (bAutoSaveLevel)
	{
		return false;
	}

	//Early out
	if (!EMS->HasStreamingLevels() || !EMSFLAG::IsLevel(Data))
	{
		return false;
	}

	auto LogStreamingWarning = [](const FString& Msg)
	{
		UE_LOG(LogEasyMultiSave, Warning, TEXT("%s"), *Msg);
		if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Yellow, Msg);
	};

	if (EMS->AutoSaveLoadWorldPartition())
	{
		//Handle World Partition
		if (!EMS->WorldPartitionLoadComplete())
		{
			if (!EMS->SkipInitialWorldPartitionLoad())
			{
				LogStreamingWarning(TEXT("World Partition is still loading (automatic load). Save operation cancelled to prevent data loss."));
				return true;
			}
			else
			{
				LogStreamingWarning(TEXT("Save occurred during World Partition load (manual load). Data was overwritten(!). Use 'Is Level Streaming Active' to check."));
			}
		}
	}
	else
	{
		//Handle traditional level streaming
		if (EMS->IsLevelStreaming())
		{
			LogStreamingWarning(TEXT("Level streaming is still active. Save operation cancelled to prevent data loss."));
			return true;
		}
	}
	
	return false;
}

void UEMSAsyncSaveGame::BeginDestroy()
{
	if (EMS)
	{
		EMS->GetTimerManager().ClearAllTimersForObject(this);
	}

	Super::BeginDestroy();
}

