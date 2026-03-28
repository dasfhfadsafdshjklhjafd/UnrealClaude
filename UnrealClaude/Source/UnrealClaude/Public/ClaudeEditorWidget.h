// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IClaudeRunner.h"
#include "LLM/ILLMBackend.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SMultiLineEditableText;
class SMultiLineEditableTextBox;
class SScrollBox;
class SVerticalBox;
class SClaudeInputArea;
class SClaudeToolbar;
class SExpandableArea;

/**
 * Chat message display widget
 */
class SChatMessage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SChatMessage)
		: _IsUser(true)
	{}
		SLATE_ARGUMENT(FString, Message)
		SLATE_ARGUMENT(bool, IsUser)
		SLATE_ARGUMENT(FString, SenderName)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

/**
 * Main Claude chat widget for the editor
 */
class UNREALCLAUDE_API SClaudeEditorWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClaudeEditorWidget)
	{}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SClaudeEditorWidget();

private:
	/** UI Construction */
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildChatArea();
	TSharedRef<SWidget> BuildInputArea();
	TSharedRef<SWidget> BuildStatusBar();
	
	/** Add a message to the chat display (SenderName is stored per-message for history) */
	void AddMessage(const FString& Message, bool bIsUser, const FString& SenderName = FString());
	
	/** Add streaming response (appends to last assistant message) */
	void AppendToLastResponse(const FString& Text);
	
	/** Send the current input to Claude */
	void SendMessage();
	
	/** Clear chat history */
	void ClearChat();

	/** Cancel current request */
	void CancelRequest();

	/** Copy selected text or last response */
	void CopyToClipboard();

	/** Copy entire chat history to clipboard */
	void CopyWholeChat();

	/** Send a compact/summarize request and replace history with summary */
	void CompactSession();

	/** Restore previous session context */
	void RestoreSession();

	/** Start a new session (clear history and saved session) */
	void NewSession();
	
	/** Handle response from Claude */
	void OnClaudeResponse(const FString& Response, bool bSuccess);
	
	/** Check if Claude CLI is available */
	bool IsClaudeAvailable() const;
	
	/** Get status text */
	FText GetStatusText() const;
	
	/** Get status color */
	FSlateColor GetStatusColor() const;
	
private:
	/** Chat message container */
	TSharedPtr<SVerticalBox> ChatMessagesBox;

	/** Scroll box for chat */
	TSharedPtr<SScrollBox> ChatScrollBox;

	/** Input area widget */
	TSharedPtr<SClaudeInputArea> InputArea;

	/** Current input text */
	FString CurrentInputText;
	
	/** Is currently waiting for response */
	bool bIsWaitingForResponse = false;

	/** Timestamp when the current streaming request started (FPlatformTime::Seconds) */
	double StreamingStartTime = 0.0;

	/** Number of tool calls observed during current streaming response */
	int32 StreamingToolCallCount = 0;

	/** Final stats from the Result event (persists after streaming ends until next request) */
	FString LastResultStats;

	/** Last response for copying */
	FString LastResponse;

	/** Display name of the model that is currently responding (captured at send time) */
	FString CurrentResponseModelName;

	/** Accumulated streaming response */
	FString StreamingResponse;

	/** Current streaming message widget (for updating in place) */
	TSharedPtr<SMultiLineEditableText> StreamingTextBlock;

	/** Inner content box for streaming bubble (holds text segments + tool indicators) */
	TSharedPtr<SVerticalBox> StreamingContentBox;

	/** Text accumulated for the current segment only (reset on each tool use) */
	FString CurrentSegmentText;

	/** Tool call status labels by call ID */
	TMap<FString, TSharedPtr<STextBlock>> ToolCallStatusLabels;

	/** Tool call result text blocks by call ID */
	TMap<FString, TSharedPtr<STextBlock>> ToolCallResultTexts;

	/** Tool call expandable areas by call ID */
	TMap<FString, TSharedPtr<SExpandableArea>> ToolCallExpandables;

	/** Tool names by call ID */
	TMap<FString, FString> ToolCallNames;

	/** All text segments in order (frozen when tool events arrive) */
	TArray<FString> AllTextSegments;

	/** Text block widgets for each segment (for code block post-processing) */
	TArray<TSharedPtr<SMultiLineEditableText>> TextSegmentBlocks;

	/** Container vertical boxes wrapping each text segment (for code block replacement) */
	TArray<TSharedPtr<SVerticalBox>> TextSegmentContainers;

	/** Current tool group expandable area */
	TSharedPtr<SExpandableArea> ToolGroupExpandArea;

	/** Inner box holding tool entries in current group */
	TSharedPtr<SVerticalBox> ToolGroupInnerBox;

	/** Summary text for current tool group header */
	TSharedPtr<STextBlock> ToolGroupSummaryText;

	/** Number of tools in current group */
	int32 ToolGroupCount = 0;

	/** Number of completed tools in current group */
	int32 ToolGroupDoneCount = 0;

	/** Tool call IDs in current group (for showing/hiding labels on transition) */
	TArray<FString> ToolGroupCallIds;

	/** Toolbar widget (kept to call RefreshModelOptions on backend change) */
	TSharedPtr<SClaudeToolbar> ToolbarWidget;

	/** Include UE5.7 context in prompts */
	bool bIncludeUE57Context = true;

	/** Include project context in prompts */
	bool bIncludeProjectContext = true;

	/** Selected Claude model */
	FString SelectedModel = TEXT("claude-sonnet-4-6");

	/** Whether Claude models use CLI (true) or direct API (false) */
	bool bAnthropicUseCLI = true;

	/** Sync active model and backend from Worker role assignment */
	void SyncModelFromWorkerRole();

	/** Handle Anthropic mode toggle (CLI vs API) */
	void OnAnthropicModeChanged(bool bUseCLI);

	/** Handle model change — auto-resolve backend */
	void OnModelChangedWithBackendResolve(const FString& NewModelId);

	/** Open role config UI */
	void OpenRoleConfig();

	/** Selected role for next send (defaults to Worker) */
	EModelRole SelectedSendRole = EModelRole::Worker;

	/** Accumulated input token count for the current session */
	int32 SessionInputTokens = 0;

	/** Accumulated output token count for the current session */
	int32 SessionOutputTokens = 0;

	/** Handle streaming progress from Claude (legacy, still used for accumulation) */
	void OnClaudeProgress(const FString& PartialOutput);

	/** Handle structured NDJSON stream events from Claude */
	void OnClaudeStreamEvent(const FClaudeStreamEvent& Event);

	/** Reset all streaming and tool tracking state to defaults */
	void ResetStreamingState();

	/** Start a new streaming response message */
	void StartStreamingResponse();

	/** Finalize streaming response */
	void FinalizeStreamingResponse();

	/** Handle a ToolUse stream event (insert tool indicator, start new text segment) */
	void HandleToolUseEvent(const FClaudeStreamEvent& Event);

	/** Handle a ToolResult stream event (update tool indicator with completion + result) */
	void HandleToolResultEvent(const FClaudeStreamEvent& Event);

	/** Handle a Result stream event (append stats footer) */
	void HandleResultEvent(const FClaudeStreamEvent& Event);

	/** Update tool group summary text based on pending/completed state */
	void UpdateToolGroupSummary();

	/** Get display-friendly tool name (strips MCP server prefix) */
	static FString GetDisplayToolName(const FString& FullToolName);

	/** Post-process text segments to render code blocks with monospace styling */
	void ParseAndRenderCodeBlocks();

	/** Parse text into alternating plain/code sections split on triple-backtick fences */
	static void ParseCodeFences(const FString& Input, TArray<TPair<FString, bool>>& OutSections);

	/** Refresh project context */
	void RefreshProjectContext();

	/** Get project context summary for status bar */
	FText GetProjectContextSummary() const;

	/** Generate MCP tool status message for greeting */
	FString GenerateMCPStatusMessage() const;
};
