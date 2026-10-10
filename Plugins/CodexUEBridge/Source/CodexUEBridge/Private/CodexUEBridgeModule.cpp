#include "CodexUEBridgeModule.h"

#include "Async/Async.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Runtime/Launch/Resources/Version.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogCodexUEBridge, Log, All);

namespace CodexUEBridge
{
	constexpr int32 Port = 18080;
	const TCHAR* Route = TEXT("/codex-ue");

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle RouteHandle;

	TSharedPtr<FJsonObject> MakeResult(bool bOk)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), bOk);
		return Result;
	}

	TSharedPtr<FJsonObject> MakeBridgeError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Result = MakeResult(false);
		Result->SetStringField(TEXT("error"), Message);
		return Result;
	}

	TSharedPtr<FJsonObject> BuildEnvironmentInfo()
	{
		const FEngineVersion& EngineVersion = FEngineVersion::Current();
		TSharedPtr<FJsonObject> Result = MakeResult(true);
		Result->SetStringField(TEXT("name"), TEXT("codex-ue-bridge"));
		Result->SetStringField(TEXT("version"), TEXT("0.2.0"));
		Result->SetNumberField(TEXT("port"), Port);
		Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Result->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Result->SetStringField(TEXT("engine_version"), EngineVersion.ToString());
		Result->SetNumberField(TEXT("engine_major"), EngineVersion.GetMajor());
		Result->SetNumberField(TEXT("engine_minor"), EngineVersion.GetMinor());
		Result->SetNumberField(TEXT("engine_patch"), EngineVersion.GetPatch());
		Result->SetNumberField(TEXT("compiled_engine_major"), ENGINE_MAJOR_VERSION);
		Result->SetNumberField(TEXT("compiled_engine_minor"), ENGINE_MINOR_VERSION);
		Result->SetNumberField(TEXT("compiled_engine_patch"), ENGINE_PATCH_VERSION);
		Result->SetBoolField(TEXT("engine_matches_build"),
			EngineVersion.GetMajor() == ENGINE_MAJOR_VERSION && EngineVersion.GetMinor() == ENGINE_MINOR_VERSION);
		Result->SetBoolField(TEXT("ue54_compatible"),
			EngineVersion.GetMajor() == 5 && EngineVersion.GetMinor() == 4 &&
			ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4);
		return Result;
	}

	TSharedPtr<FJsonObject> ValidateExpectedContext(const TSharedPtr<FJsonObject>& Request)
	{
		const FEngineVersion& EngineVersion = FEngineVersion::Current();
		FString ExpectedProject;
		if (Request->TryGetStringField(TEXT("expected_project"), ExpectedProject) &&
			!ExpectedProject.Equals(FApp::GetProjectName(), ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> Error = MakeBridgeError(TEXT("Current project does not match expected_project"));
			Error->SetStringField(TEXT("expected_project"), ExpectedProject);
			Error->SetStringField(TEXT("actual_project"), FApp::GetProjectName());
			return Error;
		}

		double ExpectedMajor = 0;
		if (Request->TryGetNumberField(TEXT("expected_engine_major"), ExpectedMajor) &&
			EngineVersion.GetMajor() != static_cast<uint16>(ExpectedMajor))
		{
			TSharedPtr<FJsonObject> Error = MakeBridgeError(TEXT("Current engine major version does not match"));
			Error->SetNumberField(TEXT("expected_engine_major"), ExpectedMajor);
			Error->SetNumberField(TEXT("actual_engine_major"), EngineVersion.GetMajor());
			return Error;
		}

		double ExpectedMinor = 0;
		if (Request->TryGetNumberField(TEXT("expected_engine_minor"), ExpectedMinor) &&
			EngineVersion.GetMinor() != static_cast<uint16>(ExpectedMinor))
		{
			TSharedPtr<FJsonObject> Error = MakeBridgeError(TEXT("Current engine minor version does not match"));
			Error->SetNumberField(TEXT("expected_engine_minor"), ExpectedMinor);
			Error->SetNumberField(TEXT("actual_engine_minor"), EngineVersion.GetMinor());
			return Error;
		}
		return nullptr;
	}

	FString SeverityToString(EMessageSeverity::Type Severity)
	{
		switch (Severity)
		{
		case EMessageSeverity::Error:
			return TEXT("error");
		case EMessageSeverity::Warning:
			return TEXT("warning");
		case EMessageSeverity::PerformanceWarning:
			return TEXT("performance_warning");
		case EMessageSeverity::Info:
			return TEXT("info");
		default:
			return TEXT("other");
		}
	}

	void CompleteJson(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonObject>& Object, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		FString Json;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"));
		Response->Code = Code;
		OnComplete(MoveTemp(Response));
	}

	FString ReadBody(const FHttpServerRequest& Request)
	{
		if (Request.Body.IsEmpty())
		{
			return FString();
		}

		FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	FString NormalizePinName(const FName Name)
	{
		FString Value = Name.ToString();
		Value.ReplaceInline(TEXT(" "), TEXT(""));
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		return Value.ToLower();
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}

		FString Normalized = Name;
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized = Normalized.ToLower();

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && NormalizePinName(Pin->PinName) == Normalized)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		TArray<UEdGraph*> Graphs;
		Graphs.Append(Blueprint->UbergraphPages);
		Graphs.Append(Blueprint->FunctionGraphs);
		Graphs.Append(Blueprint->MacroGraphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> PinToJson(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Json->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		Json->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		if (!Pin->DefaultValue.IsEmpty())
		{
			Json->SetStringField(TEXT("default"), Pin->DefaultValue);
		}

		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode())
			{
				continue;
			}
			TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
			Link->SetStringField(TEXT("node_guid"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Link->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
			Links.Add(MakeShared<FJsonValueObject>(Link));
		}
		Json->SetArrayField(TEXT("links"), Links);
		return Json;
	}

	TSharedPtr<FJsonObject> InspectGraph(const TSharedPtr<FJsonObject>& Request)
	{
		FString AssetPath;
		if (!Request->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			return MakeBridgeError(TEXT("asset_path is required"));
		}

		FString GraphName = TEXT("EventGraph");
		Request->TryGetStringField(TEXT("graph_name"), GraphName);
		FString Search;
		Request->TryGetStringField(TEXT("search"), Search);

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			return MakeBridgeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		UEdGraph* Graph = FindGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return MakeBridgeError(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			const FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			if (!Search.IsEmpty() && !Title.Contains(Search, ESearchCase::IgnoreCase) && !Node->GetClass()->GetName().Contains(Search, ESearchCase::IgnoreCase))
			{
				continue;
			}

			TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			NodeJson->SetStringField(TEXT("title"), Title);
			NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
			NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin)
				{
					Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
				}
			}
			NodeJson->SetArrayField(TEXT("pins"), Pins);
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
		}

		TSharedPtr<FJsonObject> Result = MakeResult(true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("graph_name"), Graph->GetName());
		Result->SetArrayField(TEXT("nodes"), Nodes);
		return Result;
	}

	bool IsGetAllActorsNode(const UEdGraphNode* Node)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		return Call && Call->GetFunctionName() == FName(TEXT("GetAllActorsOfClass"));
	}

	UK2Node_MacroInstance* FindForEachNode(UK2Node_CallFunction* GetAllActors);

	UK2Node_CallFunction* FindThenTwoGetAllActors(UEdGraph* Graph, UClass* ActorClass)
	{
		UK2Node_CallFunction* Fallback = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UEdGraphPin* ThenTwo = FindPin(Node, TEXT("Then 2"), EGPD_Output);
			if (!ThenTwo)
			{
				continue;
			}
			for (UEdGraphPin* Linked : ThenTwo->LinkedTo)
			{
				UK2Node_CallFunction* GetAllActors = Linked ? Cast<UK2Node_CallFunction>(Linked->GetOwningNode()) : nullptr;
				if (!GetAllActors || !IsGetAllActorsNode(GetAllActors))
				{
					continue;
				}

				UEdGraphPin* ActorClassPin = FindPin(GetAllActors, TEXT("Actor Class"), EGPD_Input);
				if (!ActorClassPin || ActorClassPin->DefaultObject != ActorClass)
				{
					continue;
				}

				UK2Node_MacroInstance* ForEach = FindForEachNode(GetAllActors);
				if (ForEach)
				{
					UEdGraphPin* LoopBody = FindPin(ForEach, TEXT("Loop Body"), EGPD_Output);
					UEdGraphPin* ArrayElement = FindPin(ForEach, TEXT("Array Element"), EGPD_Output);
					if (LoopBody && LoopBody->LinkedTo.IsEmpty() && ArrayElement && ArrayElement->LinkedTo.IsEmpty())
					{
						return GetAllActors;
					}
				}
				Fallback = GetAllActors;
			}
		}
		return Fallback;
	}

	UK2Node_MacroInstance* FindForEachNode(UK2Node_CallFunction* GetAllActors)
	{
		UEdGraphPin* OutActors = FindPin(GetAllActors, TEXT("Out Actors"), EGPD_Output);
		if (!OutActors)
		{
			return nullptr;
		}

		for (UEdGraphPin* Linked : OutActors->LinkedTo)
		{
			UK2Node_MacroInstance* Macro = Linked ? Cast<UK2Node_MacroInstance>(Linked->GetOwningNode()) : nullptr;
			if (Macro && FindPin(Macro, TEXT("Loop Body"), EGPD_Output) && FindPin(Macro, TEXT("Array Element"), EGPD_Output))
			{
				return Macro;
			}
		}
		return nullptr;
	}

	bool IsGetterForComponent(const UK2Node_VariableGet* Getter, const FName ComponentName)
	{
		return Getter &&
			(Getter->VariableReference.GetMemberName() == ComponentName ||
				Getter->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(ComponentName.ToString(), ESearchCase::IgnoreCase));
	}

	bool IsExistingHideChain(UK2Node_MacroInstance* ForEach, const FName ComponentName)
	{
		UEdGraphPin* ArrayElement = FindPin(ForEach, TEXT("Array Element"), EGPD_Output);
		UEdGraphPin* LoopBody = FindPin(ForEach, TEXT("Loop Body"), EGPD_Output);
		if (!ArrayElement || !LoopBody)
		{
			return false;
		}

		for (UEdGraphPin* ElementLink : ArrayElement->LinkedTo)
		{
			UK2Node_VariableGet* Getter = ElementLink ? Cast<UK2Node_VariableGet>(ElementLink->GetOwningNode()) : nullptr;
			if (!IsGetterForComponent(Getter, ComponentName))
			{
				continue;
			}

			UEdGraphPin* ValuePin = Getter->GetValuePin();
			for (UEdGraphPin* ValueLink : ValuePin->LinkedTo)
			{
				UK2Node_CallFunction* Call = ValueLink ? Cast<UK2Node_CallFunction>(ValueLink->GetOwningNode()) : nullptr;
				if (!Call || Call->GetFunctionName() != FName(TEXT("SetVisibility")))
				{
					continue;
				}

				UEdGraphPin* Visibility = FindPin(Call, TEXT("bNewVisibility"), EGPD_Input);
				UEdGraphPin* Exec = Call->GetExecPin();
				if (Visibility && Visibility->DefaultValue.Equals(TEXT("false"), ESearchCase::IgnoreCase) && Exec && Exec->LinkedTo.Contains(LoopBody))
				{
					return true;
				}
			}
		}
		return false;
	}

	int32 RemoveStaleHideChains(UEdGraph* Graph, UK2Node_MacroInstance* ForEach, const FName ComponentName)
	{
		UEdGraphPin* ArrayElement = FindPin(ForEach, TEXT("Array Element"), EGPD_Output);
		UEdGraphPin* LoopBody = FindPin(ForEach, TEXT("Loop Body"), EGPD_Output);
		if (!ArrayElement || !LoopBody)
		{
			return 0;
		}

		int32 Removed = 0;
		const TArray<UEdGraphPin*> ElementLinks = ArrayElement->LinkedTo;
		for (UEdGraphPin* ElementLink : ElementLinks)
		{
			UK2Node_VariableGet* Getter = ElementLink ? Cast<UK2Node_VariableGet>(ElementLink->GetOwningNode()) : nullptr;
			if (!IsGetterForComponent(Getter, ComponentName))
			{
				continue;
			}

			bool bHasActiveCall = false;
			TArray<UK2Node_CallFunction*> StaleCalls;
			const TArray<UEdGraphPin*> ValueLinks = Getter->GetValuePin()->LinkedTo;
			for (UEdGraphPin* ValueLink : ValueLinks)
			{
				UK2Node_CallFunction* Call = ValueLink ? Cast<UK2Node_CallFunction>(ValueLink->GetOwningNode()) : nullptr;
				if (!Call || Call->GetFunctionName() != FName(TEXT("SetVisibility")))
				{
					continue;
				}

				UEdGraphPin* Exec = Call->GetExecPin();
				if (Exec && Exec->LinkedTo.Contains(LoopBody))
				{
					bHasActiveCall = true;
				}
				else
				{
					StaleCalls.Add(Call);
				}
			}

			for (UK2Node_CallFunction* StaleCall : StaleCalls)
			{
				Graph->RemoveNode(StaleCall);
				++Removed;
			}
			if (!bHasActiveCall && StaleCalls.Num() > 0)
			{
				Graph->RemoveNode(Getter);
				++Removed;
			}
		}
		return Removed;
	}

	TSharedPtr<FJsonObject> HideComponentForEach(const TSharedPtr<FJsonObject>& Request)
	{
		FString AssetPath;
		FString ActorBlueprintPath;
		FString ComponentNameString;
		if (!Request->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			return MakeBridgeError(TEXT("asset_path is required"));
		}
		if (!Request->TryGetStringField(TEXT("actor_blueprint_path"), ActorBlueprintPath) || ActorBlueprintPath.IsEmpty())
		{
			return MakeBridgeError(TEXT("actor_blueprint_path is required"));
		}
		if (!Request->TryGetStringField(TEXT("component_name"), ComponentNameString) || ComponentNameString.IsEmpty())
		{
			return MakeBridgeError(TEXT("component_name is required"));
		}

		FString GraphName = TEXT("EventGraph");
		Request->TryGetStringField(TEXT("graph_name"), GraphName);

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		UBlueprint* ActorBlueprint = LoadObject<UBlueprint>(nullptr, *ActorBlueprintPath);
		if (!Blueprint)
		{
			return MakeBridgeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}
		if (!ActorBlueprint || !ActorBlueprint->GeneratedClass)
		{
			return MakeBridgeError(FString::Printf(TEXT("Actor Blueprint not found: %s"), *ActorBlueprintPath));
		}

		UClass* ActorClass = ActorBlueprint->GeneratedClass;
		const FName ComponentName(*ComponentNameString);
		FObjectPropertyBase* ComponentProperty = FindFProperty<FObjectPropertyBase>(ActorClass, ComponentName);
		if (!ComponentProperty || !ComponentProperty->PropertyClass->IsChildOf<USceneComponent>())
		{
			return MakeBridgeError(FString::Printf(TEXT("Scene component property not found: %s.%s"), *ActorClass->GetName(), *ComponentNameString));
		}

		UEdGraph* Graph = FindGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return MakeBridgeError(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		UK2Node_CallFunction* GetAllActors = FindThenTwoGetAllActors(Graph, ActorClass);
		if (!GetAllActors)
		{
			return MakeBridgeError(TEXT("Get All Actors of Class connected to Then 2 was not found"));
		}

		UK2Node_MacroInstance* ForEach = FindForEachNode(GetAllActors);
		if (!ForEach)
		{
			return MakeBridgeError(TEXT("For Each Loop connected to Out Actors was not found"));
		}

		if (IsExistingHideChain(ForEach, ComponentName))
		{
			FScopedTransaction CleanupTransaction(NSLOCTEXT("CodexUEBridge", "CleanupHideComponentForEach", "Clean stale component visibility nodes"));
			Blueprint->Modify();
			Graph->Modify();
			const int32 RemovedNodes = RemoveStaleHideChains(Graph, ForEach, ComponentName);
			bool bSaved = true;
			if (RemovedNodes > 0)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				FCompilerResultsLog CleanupCompileLog;
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CleanupCompileLog);
				UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
				bSaved = CleanupCompileLog.NumErrors == 0 && AssetSubsystem && AssetSubsystem->SaveLoadedAsset(Blueprint, false);
			}
			else
			{
				CleanupTransaction.Cancel();
			}
			TSharedPtr<FJsonObject> Result = MakeResult(true);
			Result->SetBoolField(TEXT("already_present"), true);
			Result->SetNumberField(TEXT("removed_stale_nodes"), RemovedNodes);
			Result->SetBoolField(TEXT("saved"), bSaved);
			return Result;
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			return MakeBridgeError(TEXT("Graph schema is unavailable"));
		}

		UFunction* SetVisibilityFunction = USceneComponent::StaticClass()->FindFunctionByName(FName(TEXT("SetVisibility")));
		if (!SetVisibilityFunction)
		{
			return MakeBridgeError(TEXT("USceneComponent.SetVisibility was not found"));
		}

		FScopedTransaction Transaction(NSLOCTEXT("CodexUEBridge", "HideComponentForEach", "Add component visibility nodes"));
		Blueprint->Modify();
		Graph->Modify();

		FGraphNodeCreator<UK2Node_VariableGet> GetterCreator(*Graph);
		UK2Node_VariableGet* Getter = GetterCreator.CreateNode();
		Getter->VariableReference.SetExternalMember(ComponentName, ActorClass);
		Getter->NodePosX = ForEach->NodePosX + 420;
		Getter->NodePosY = ForEach->NodePosY + 80;
		GetterCreator.Finalize();

		FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Graph);
		UK2Node_CallFunction* SetVisibility = CallCreator.CreateNode(false);
		SetVisibility->SetFromFunction(SetVisibilityFunction);
		SetVisibility->NodePosX = ForEach->NodePosX + 760;
		SetVisibility->NodePosY = ForEach->NodePosY;
		CallCreator.Finalize();

		UEdGraphPin* LoopBody = FindPin(ForEach, TEXT("Loop Body"), EGPD_Output);
		UEdGraphPin* ArrayElement = FindPin(ForEach, TEXT("Array Element"), EGPD_Output);
		UEdGraphPin* GetterTarget = FindPin(Getter, TEXT("self"), EGPD_Input);
		UEdGraphPin* GetterValue = Getter->GetValuePin();
		UEdGraphPin* CallExec = SetVisibility->GetExecPin();
		UEdGraphPin* CallTarget = FindPin(SetVisibility, TEXT("self"), EGPD_Input);
		UEdGraphPin* NewVisibility = FindPin(SetVisibility, TEXT("bNewVisibility"), EGPD_Input);

		auto Connect = [Schema](UEdGraphPin* From, UEdGraphPin* To)
		{
			return From && To && Schema->TryCreateConnection(From, To);
		};

		const bool bConnected =
			Connect(LoopBody, CallExec) &&
			Connect(ArrayElement, GetterTarget) &&
			Connect(GetterValue, CallTarget);

		if (!bConnected || !NewVisibility)
		{
			Graph->RemoveNode(SetVisibility);
			Graph->RemoveNode(Getter);
			Transaction.Cancel();
			return MakeBridgeError(TEXT("Failed to connect the generated Blueprint nodes"));
		}

		Schema->TrySetDefaultValue(*NewVisibility, TEXT("false"));
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		FCompilerResultsLog CompileLog;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileLog);
		const bool bCompiled = Blueprint->Status != BS_Error && CompileLog.NumErrors == 0;
		bool bSaved = false;
		if (bCompiled && GEditor)
		{
			if (UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
			{
				bSaved = AssetSubsystem->SaveLoadedAsset(Blueprint, false);
			}
		}

		TSharedPtr<FJsonObject> Result = MakeResult(bCompiled && bSaved);
		Result->SetBoolField(TEXT("already_present"), false);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetNumberField(TEXT("compile_errors"), CompileLog.NumErrors);
		Result->SetNumberField(TEXT("compile_warnings"), CompileLog.NumWarnings);
		Result->SetStringField(TEXT("getter_guid"), Getter->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Result->SetStringField(TEXT("set_visibility_guid"), SetVisibility->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		if (!bCompiled)
		{
			Result->SetStringField(TEXT("error"), TEXT("Blueprint compilation failed; asset was not saved"));
		}
		else if (!bSaved)
		{
			Result->SetStringField(TEXT("error"), TEXT("Blueprint compiled but could not be saved"));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> ValidateProject(const TSharedPtr<FJsonObject>& Request)
	{
		TSharedPtr<FJsonObject> Result = BuildEnvironmentInfo();
		bool bValid = true;
		if (TSharedPtr<FJsonObject> ContextError = ValidateExpectedContext(Request))
		{
			bValid = false;
			Result->SetObjectField(TEXT("context_error"), ContextError);
		}

		const FString ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		const bool bProjectFileExists = IFileManager::Get().FileExists(*ProjectFile);
		Result->SetBoolField(TEXT("project_file_exists"), bProjectFileExists);
		bValid &= bProjectFileExists;

		TArray<TSharedPtr<FJsonValue>> AssetResults;
		const TArray<TSharedPtr<FJsonValue>>* RequiredAssets = nullptr;
		if (Request->TryGetArrayField(TEXT("required_assets"), RequiredAssets) && RequiredAssets)
		{
			for (const TSharedPtr<FJsonValue>& AssetValue : *RequiredAssets)
			{
				TSharedPtr<FJsonObject> AssetResult = MakeShared<FJsonObject>();
				const FString AssetPath = AssetValue.IsValid() ? AssetValue->AsString() : FString();
				AssetResult->SetStringField(TEXT("asset_path"), AssetPath);
				UObject* Asset = AssetPath.StartsWith(TEXT("/Game/"))
					? StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath)
					: nullptr;
				const bool bExists = Asset != nullptr;
				AssetResult->SetBoolField(TEXT("exists"), bExists);
				if (Asset)
				{
					AssetResult->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
				}
				else
				{
					bValid = false;
				}
				AssetResults.Add(MakeShared<FJsonValueObject>(AssetResult));
			}
		}
		Result->SetArrayField(TEXT("assets"), AssetResults);
		Result->SetBoolField(TEXT("ok"), bValid);
		Result->SetBoolField(TEXT("validated"), bValid);
		return Result;
	}

	TSharedPtr<FJsonObject> CompileBlueprintAsset(const TSharedPtr<FJsonObject>& Request)
	{
		FString AssetPath;
		if (!Request->TryGetStringField(TEXT("asset_path"), AssetPath) || !AssetPath.StartsWith(TEXT("/Game/")))
		{
			return MakeBridgeError(TEXT("asset_path must be a /Game Blueprint path"));
		}

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			return MakeBridgeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		bool bRefreshNodes = true;
		bool bSaveAfterCompile = true;
		Request->TryGetBoolField(TEXT("refresh_nodes"), bRefreshNodes);
		Request->TryGetBoolField(TEXT("save_after_compile"), bSaveAfterCompile);
		if (bRefreshNodes)
		{
			FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
		}

		FCompilerResultsLog CompileLog;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileLog);
		const bool bCompiled = Blueprint->Status != BS_Error && CompileLog.NumErrors == 0;
		bool bSaved = !bSaveAfterCompile;
		if (bCompiled && bSaveAfterCompile)
		{
			UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
			bSaved = AssetSubsystem && AssetSubsystem->SaveLoadedAsset(Blueprint, false);
		}

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		for (const TSharedRef<FTokenizedMessage>& Message : CompileLog.Messages)
		{
			TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
			Diagnostic->SetStringField(TEXT("severity"), SeverityToString(Message->GetSeverity()));
			Diagnostic->SetStringField(TEXT("message"), Message->ToText().ToString());
			Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
		}

		FString Status = TEXT("unknown");
		if (Blueprint->Status == BS_UpToDate)
		{
			Status = TEXT("up_to_date");
		}
		else if (Blueprint->Status == BS_UpToDateWithWarnings)
		{
			Status = TEXT("warnings");
		}
		else if (Blueprint->Status == BS_Error)
		{
			Status = TEXT("error");
		}

		TSharedPtr<FJsonObject> Result = MakeResult(bCompiled && bSaved);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("status"), Status);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("refreshed_nodes"), bRefreshNodes);
		Result->SetNumberField(TEXT("error_count"), CompileLog.NumErrors);
		Result->SetNumberField(TEXT("warning_count"), CompileLog.NumWarnings);
		Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
		if (!bCompiled)
		{
			Result->SetStringField(TEXT("error"), TEXT("Blueprint compilation failed"));
		}
		else if (!bSaved)
		{
			Result->SetStringField(TEXT("error"), TEXT("Blueprint compiled but could not be saved"));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> SetNodeComments(const TSharedPtr<FJsonObject>& Request)
	{
		FString AssetPath;
		if (!Request->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			return MakeBridgeError(TEXT("asset_path is required"));
		}

		FString GraphName = TEXT("EventGraph");
		Request->TryGetStringField(TEXT("graph_name"), GraphName);
		const TArray<TSharedPtr<FJsonValue>>* Comments = nullptr;
		if (!Request->TryGetArrayField(TEXT("comments"), Comments) || !Comments || Comments->IsEmpty())
		{
			return MakeBridgeError(TEXT("comments array is required"));
		}

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			return MakeBridgeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		UEdGraph* Graph = FindGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return MakeBridgeError(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		FScopedTransaction Transaction(NSLOCTEXT("CodexUEBridge", "SetNodeComments", "Set Blueprint node comments"));
		Blueprint->Modify();
		Graph->Modify();
		TArray<TSharedPtr<FJsonValue>> UpdatedGuids;

		for (const TSharedPtr<FJsonValue>& Value : *Comments)
		{
			const TSharedPtr<FJsonObject> Comment = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Comment.IsValid())
			{
				Transaction.Cancel();
				return MakeBridgeError(TEXT("Each comments entry must be an object"));
			}

			FString GuidString;
			FString Text;
			FGuid Guid;
			if (!Comment->TryGetStringField(TEXT("node_guid"), GuidString) || !FGuid::Parse(GuidString, Guid) ||
				!Comment->TryGetStringField(TEXT("text"), Text))
			{
				Transaction.Cancel();
				return MakeBridgeError(TEXT("Each comments entry requires a valid node_guid and text"));
			}

			UEdGraphNode* TargetNode = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == Guid)
				{
					TargetNode = Node;
					break;
				}
			}
			if (!TargetNode)
			{
				Transaction.Cancel();
				return MakeBridgeError(FString::Printf(TEXT("Node not found: %s"), *GuidString));
			}

			TargetNode->Modify();
			TargetNode->NodeComment = Text;
			TargetNode->bCommentBubbleVisible = true;
			TargetNode->bCommentBubblePinned = true;
			UpdatedGuids.Add(MakeShared<FJsonValueString>(GuidString));
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		const bool bSaved = AssetSubsystem && AssetSubsystem->SaveLoadedAsset(Blueprint, false);

		TSharedPtr<FJsonObject> Result = MakeResult(bSaved);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetArrayField(TEXT("updated_node_guids"), UpdatedGuids);
		if (!bSaved)
		{
			Result->SetStringField(TEXT("error"), TEXT("Node comments were updated but the Blueprint could not be saved"));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> CopyThenTwoToThenThree(const TSharedPtr<FJsonObject>& Request)
	{
		FString AssetPath;
		if (!Request->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			return MakeBridgeError(TEXT("asset_path is required"));
		}

		FString GraphName = TEXT("EventGraph");
		Request->TryGetStringField(TEXT("graph_name"), GraphName);

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			return MakeBridgeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		UEdGraph* Graph = FindGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return MakeBridgeError(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		FGuid SequenceGuid;
		FGuid::Parse(TEXT("9E51565F-4821-8AE1-941A-5686DDE126A8"), SequenceGuid);
		UK2Node_ExecutionSequence* Sequence = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == SequenceGuid)
			{
				Sequence = Cast<UK2Node_ExecutionSequence>(Node);
				break;
			}
		}
		if (!Sequence)
		{
			return MakeBridgeError(TEXT("ExitAllModel Sequence node was not found"));
		}

		UEdGraphPin* ThenTwo = Sequence->GetThenPinGivenIndex(2);
		if (!ThenTwo)
		{
			return MakeBridgeError(TEXT("Then 2 pin was not found"));
		}
		double RequestedThenIndex = 3;
		Request->TryGetNumberField(TEXT("target_then_index"), RequestedThenIndex);
		const int32 TargetThenIndex = FMath::RoundToInt(RequestedThenIndex);
		if (TargetThenIndex < 3)
		{
			return MakeBridgeError(TEXT("target_then_index must be 3 or greater"));
		}
		if (Sequence->GetThenPinGivenIndex(TargetThenIndex))
		{
			return MakeBridgeError(FString::Printf(TEXT("Then %d already exists; refusing to modify the Blueprint"), TargetThenIndex));
		}
		if (ThenTwo->LinkedTo.Num() != 1 || !ThenTwo->LinkedTo[0] || !ThenTwo->LinkedTo[0]->GetOwningNode())
		{
			return MakeBridgeError(TEXT("Then 2 must have exactly one connected root node"));
		}

		TArray<UEdGraphNode*> SourceNodes;
		TSet<UEdGraphNode*> Visited;
		TFunction<void(UEdGraphNode*)> CollectBranch = [&](UEdGraphNode* Node)
		{
			if (!Node || Visited.Contains(Node))
			{
				return;
			}
			Visited.Add(Node);
			SourceNodes.Add(Node);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					CollectBranch(Linked ? Linked->GetOwningNode() : nullptr);
				}
			}
		};
		CollectBranch(ThenTwo->LinkedTo[0]->GetOwningNode());
		if (SourceNodes.Num() != 4)
		{
			return MakeBridgeError(FString::Printf(TEXT("Then 2 branch contains %d nodes; expected exactly 4, refusing to modify"), SourceNodes.Num()));
		}

		FScopedTransaction Transaction(NSLOCTEXT("CodexUEBridge", "CopyThenTwoToTarget", "Copy Then 2 branch to target sequence output"));
		Blueprint->Modify();
		Graph->Modify();
		Sequence->Modify();
		while (!Sequence->GetThenPinGivenIndex(TargetThenIndex))
		{
			Sequence->AddInputPin();
		}
		Graph->GetSchema()->ReconstructNode(*Sequence);
		UEdGraphPin* TargetThen = Sequence->GetThenPinGivenIndex(TargetThenIndex);
		if (!TargetThen)
		{
			Transaction.Cancel();
			return MakeBridgeError(FString::Printf(TEXT("Failed to add Then %d pin"), TargetThenIndex));
		}

		TSet<UObject*> NodesToExport;
		for (UEdGraphNode* Node : SourceNodes)
		{
			NodesToExport.Add(Node);
		}
		FString ExportedText;
		FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);
		if (ExportedText.IsEmpty() || !FEdGraphUtilities::CanImportNodesFromText(Graph, ExportedText))
		{
			Transaction.Cancel();
			return MakeBridgeError(TEXT("Failed to export the Then 2 branch for duplication"));
		}

		TSet<UEdGraphNode*> ImportedNodes;
		FEdGraphUtilities::ImportNodesFromText(Graph, ExportedText, ImportedNodes);
		if (ImportedNodes.Num() != SourceNodes.Num())
		{
			Transaction.Cancel();
			return MakeBridgeError(FString::Printf(TEXT("Copied %d nodes, expected %d"), ImportedNodes.Num(), SourceNodes.Num()));
		}
		for (UEdGraphNode* Node : ImportedNodes)
		{
			if (Node)
			{
				Node->CreateNewGuid();
			}
		}

		UEdGraphNode* ImportedRoot = nullptr;
		for (UEdGraphNode* Node : ImportedNodes)
		{
			if (!Node)
			{
				continue;
			}
			bool bHasIncomingExec = false;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->LinkedTo.Num() > 0)
				{
					bHasIncomingExec = true;
					break;
				}
			}
			if (!bHasIncomingExec)
			{
				ImportedRoot = Node;
				break;
			}
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		UEdGraphPin* ImportedExec = FindPin(ImportedRoot, TEXT("execute"), EGPD_Input);
		if (!ImportedRoot || !ImportedExec || !Schema || !Schema->TryCreateConnection(TargetThen, ImportedExec))
		{
			Transaction.Cancel();
			return MakeBridgeError(FString::Printf(TEXT("Failed to connect Then %d to the copied branch"), TargetThenIndex));
		}

		const TCHAR* CommentText = TEXT("\u9000\u51fa\u80fd\u6e90\u53ef\u89c6\u5316\u6a21\u5f0f\uff1a\u590d\u5236 Then 2 \u5206\u652f\uff0c\u9690\u85cf\u6240\u6709\u673a\u67dc\u67f1\u5b50");
		for (UEdGraphNode* Node : ImportedNodes)
		{
			if (Node)
			{
				Node->Modify();
				Node->NodeComment = CommentText;
				Node->bCommentBubbleVisible = true;
				Node->bCommentBubblePinned = true;
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		TSharedPtr<FJsonObject> Result = MakeResult(true);
		Result->SetNumberField(TEXT("target_then_index"), TargetThenIndex);
		Result->SetNumberField(TEXT("copied_node_count"), ImportedNodes.Num());
		Result->SetStringField(TEXT("comment"), CommentText);
		return Result;
	}

	TSharedPtr<FJsonObject> RepairThenThreeCopy(const TSharedPtr<FJsonObject>& Request)
	{
		FString AssetPath;
		if (!Request->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			return MakeBridgeError(TEXT("asset_path is required"));
		}
		FString GraphName = TEXT("EventGraph");
		Request->TryGetStringField(TEXT("graph_name"), GraphName);
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		UEdGraph* Graph = Blueprint ? FindGraph(Blueprint, GraphName) : nullptr;
		if (!Blueprint || !Graph)
		{
			return MakeBridgeError(TEXT("ReceiveCommandBP EventGraph was not found"));
		}

		FGuid SequenceGuid;
		FGuid::Parse(TEXT("9E51565F-4821-8AE1-941A-5686DDE126A8"), SequenceGuid);
		UK2Node_ExecutionSequence* Sequence = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == SequenceGuid)
			{
				Sequence = Cast<UK2Node_ExecutionSequence>(Node);
				break;
			}
		}
		if (!Sequence || !Sequence->GetThenPinGivenIndex(3))
		{
			return MakeBridgeError(TEXT("The temporary Then 3 state was not found"));
		}

		const FGuid SourceGuids[] = {
			FGuid(TEXT("B08B6BC7-48B1-6442-7A30-7AA438FAA462")),
			FGuid(TEXT("53E651E0-4574-5515-7B8B-308FD995C865")),
			FGuid(TEXT("E57CB141-4CE4-AE3A-B484-8BBA64FDC2E7")),
			FGuid(TEXT("2D78C62C-4A19-68D6-4836-56A81A22200B"))
		};
		TSet<UEdGraphNode*> Duplicates;
		for (const FGuid& SourceGuid : SourceGuids)
		{
			bool bKeptOriginal = false;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || Node->NodeGuid != SourceGuid)
				{
					continue;
				}
				if (bKeptOriginal)
				{
					Duplicates.Add(Node);
				}
				else
				{
					bKeptOriginal = true;
				}
			}
		}
		if (Duplicates.Num() != 4)
		{
			return MakeBridgeError(FString::Printf(TEXT("Expected 4 temporary duplicate nodes, found %d"), Duplicates.Num()));
		}

		FScopedTransaction Transaction(NSLOCTEXT("CodexUEBridge", "RepairThenThreeCopy", "Repair Then 3 copied branch"));
		Blueprint->Modify();
		Graph->Modify();
		for (UEdGraphNode* Node : Duplicates)
		{
			Graph->RemoveNode(Node);
		}
		Sequence->Modify();
		Sequence->RemovePinFromExecutionNode(Sequence->GetThenPinGivenIndex(3));
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		TSharedPtr<FJsonObject> Result = MakeResult(true);
		Result->SetNumberField(TEXT("removed_duplicate_nodes"), Duplicates.Num());
		Result->SetBoolField(TEXT("removed_then_3"), true);
		return Result;
	}

	TSharedPtr<FJsonObject> ExecuteAction(const TSharedPtr<FJsonObject>& Request)
	{
		FString Action;
		if (!Request->TryGetStringField(TEXT("action"), Action))
		{
			return MakeBridgeError(TEXT("action is required"));
		}

		if (Action == TEXT("health"))
		{
			return BuildEnvironmentInfo();
		}
		if (Action == TEXT("validate_project"))
		{
			return ValidateProject(Request);
		}
		if (TSharedPtr<FJsonObject> ContextError = ValidateExpectedContext(Request))
		{
			return ContextError;
		}
		if (Action == TEXT("inspect_graph"))
		{
			return InspectGraph(Request);
		}
		if (Action == TEXT("hide_component_for_each"))
		{
			return HideComponentForEach(Request);
		}
		if (Action == TEXT("set_node_comments"))
		{
			return SetNodeComments(Request);
		}
		if (Action == TEXT("copy_then_two_to_then_three"))
		{
			return CopyThenTwoToThenThree(Request);
		}
		if (Action == TEXT("copy_then_two_to_then_four"))
		{
			Request->SetNumberField(TEXT("target_then_index"), 4);
			return CopyThenTwoToThenThree(Request);
		}
		if (Action == TEXT("repair_then_three_copy"))
		{
			return RepairThenThreeCopy(Request);
		}
		if (Action == TEXT("compile_blueprint"))
		{
			return CompileBlueprintAsset(Request);
		}
		return MakeBridgeError(FString::Printf(TEXT("Unknown action: %s"), *Action));
	}

	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
		{
			CompleteJson(OnComplete, BuildEnvironmentInfo());
			return true;
		}

		if (Request.Verb != EHttpServerRequestVerbs::VERB_POST)
		{
			CompleteJson(OnComplete, MakeBridgeError(TEXT("Method not allowed")), EHttpServerResponseCodes::BadMethod);
			return true;
		}

		TSharedPtr<FJsonObject> Body;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReadBody(Request));
		if (!FJsonSerializer::Deserialize(Reader, Body) || !Body.IsValid())
		{
			CompleteJson(OnComplete, MakeBridgeError(TEXT("Invalid JSON body")), EHttpServerResponseCodes::BadRequest);
			return true;
		}

		AsyncTask(ENamedThreads::GameThread, [Body, OnComplete]()
		{
			CompleteJson(OnComplete, ExecuteAction(Body));
		});
		return true;
	}
}

void FCodexUEBridgeModule::StartupModule()
{
	FHttpServerModule& HttpServer = FHttpServerModule::Get();
	CodexUEBridge::Router = HttpServer.GetHttpRouter(CodexUEBridge::Port);
	if (!CodexUEBridge::Router.IsValid())
	{
		UE_LOG(LogCodexUEBridge, Error, TEXT("Unable to create HTTP router on port %d"), CodexUEBridge::Port);
		return;
	}

	CodexUEBridge::RouteHandle = CodexUEBridge::Router->BindRoute(
		FHttpPath(CodexUEBridge::Route),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateStatic(&CodexUEBridge::HandleRequest));

	if (!CodexUEBridge::RouteHandle.IsValid())
	{
		UE_LOG(LogCodexUEBridge, Error, TEXT("Unable to bind route %s"), CodexUEBridge::Route);
		return;
	}

	HttpServer.StartAllListeners();
	UE_LOG(LogCodexUEBridge, Display, TEXT("Codex UE Bridge listening at http://127.0.0.1:%d%s"), CodexUEBridge::Port, CodexUEBridge::Route);
}

void FCodexUEBridgeModule::ShutdownModule()
{
	if (CodexUEBridge::Router.IsValid() && CodexUEBridge::RouteHandle.IsValid())
	{
		CodexUEBridge::Router->UnbindRoute(CodexUEBridge::RouteHandle);
	}
	CodexUEBridge::RouteHandle.Reset();
	CodexUEBridge::Router.Reset();
}

IMPLEMENT_MODULE(FCodexUEBridgeModule, CodexUEBridge)
