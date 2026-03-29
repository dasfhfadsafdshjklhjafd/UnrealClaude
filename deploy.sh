#!/bin/bash
# Deploy built plugin to MyFPS_UDEMY project
# Run after every build. UE5 must be closed first.

PLUGIN_DST="G:/UE5 projects/MyFPS_UDEMY/Plugins/UnrealClaude"

cp "G:/tools/UnrealClaude-fork/PluginBuild/Binaries/Win64/UnrealEditor-UnrealClaude.dll" "$PLUGIN_DST/Binaries/Win64/UnrealEditor-UnrealClaude.dll"
cp "G:/tools/UnrealClaude-fork/PluginBuild/Binaries/Win64/UnrealEditor.modules" "$PLUGIN_DST/Binaries/Win64/UnrealEditor.modules"
cp "G:/tools/UnrealClaude-fork/PluginBuild/Resources/RolePrompts/"*.txt "$PLUGIN_DST/Resources/RolePrompts/"

echo "Deployed: $(date)"
