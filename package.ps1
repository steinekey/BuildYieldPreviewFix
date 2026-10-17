# Builds the Workshop package of BuildYieldPreviewFix: a GameCore Mod Loader plugin. The loader DLL
# and the protocol SQL come from the vendored GCML release in gcml\ (see the GCML repository's README).
# Run after building:  powershell -File package.ps1 [-Install]
#
#   bin\workshop\BuildYieldPreviewFix\   the folder to upload to the Workshop
#   -Install                             also copies it into Documents\My Games\Sid Meier's Civilization VI\Mods,
#                                        to test it as a local mod (enable it under Additional Content)
#
# The build command (PowerShell, from the repo root):
#   & "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" BuildYieldPreviewFix.vcxproj -p:Configuration=Release -p:Platform=x64 -m -v:m -nologo
param([switch]$Install)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$name = "BuildYieldPreviewFix"
$package = Join-Path $root "bin\workshop\$name"
$modinfo = Join-Path $root "$name.modinfo"
$gcml = Join-Path $root "gcml"
$loader = Join-Path $gcml "GameCore_XP2_GCML_FinalRelease.dll"
$plugin = Join-Path $root "bin\$name.dll"
foreach ($file in $modinfo, $loader, $plugin) {
	if (-not (Test-Path $file)) {
		throw "missing: $file (build $name.vcxproj first; gcml\ must hold a GCML release)"
	}
}

# The protocol SQL is the release's template with this mod's row, so the two never drift apart.
$modId = ([xml](Get-Content $modinfo -Raw)).Mod.id
$loaderVersion = (Get-Content (Join-Path $gcml "loader-version.txt") -Raw).Trim()
$template = Get-Content (Join-Path $gcml "GameCoreModLoader.sql") -Raw
$placeholder = "VALUES('EDIT-your-mod-id', 'Binaries/Win64/EDIT-your-plugin.dll', 0, 1);"
if (-not $template.Contains($placeholder)) {
	throw "gcml\GameCoreModLoader.sql no longer has the line: $placeholder"
}
$sql = $template.Replace($placeholder, "VALUES('$modId', 'Binaries/Win64/$name.dll', 0, $loaderVersion);")

if (Test-Path $package) {
	Remove-Item $package -Recurse -Force
}
New-Item -ItemType Directory -Force (Join-Path $package "Data"), (Join-Path $package "Binaries\Win64") | Out-Null
Copy-Item $modinfo $package
Copy-Item (Join-Path $root "Data\InGame.sql") (Join-Path $package "Data")
[System.IO.File]::WriteAllText((Join-Path $package "Data\GameCoreModLoader.sql"), $sql)
Copy-Item $loader (Join-Path $package "Binaries\Win64")
# The loader is MIT licensed: its notice travels with it.
Copy-Item (Join-Path $gcml "LICENSE.txt") (Join-Path $package "Binaries\Win64\GCML-LICENSE.txt")
Copy-Item $plugin (Join-Path $package "Binaries\Win64\$name.dll")
Write-Host "packaged $package (mod $modId, loader $loaderVersion)"

if ($Install) {
	$documents = [Environment]::GetFolderPath("MyDocuments")
	$target = Join-Path $documents "My Games\Sid Meier's Civilization VI\Mods\$name"
	$game = Get-Process -Name "CivilizationVI*" -ErrorAction SilentlyContinue
	if ($game) {
		throw "the game is running (pid $($game.Id)); close it first, a loaded DLL cannot be overwritten"
	}
	if (Test-Path $target) {
		Remove-Item $target -Recurse -Force
	}
	Copy-Item $package $target -Recurse
	Write-Host "installed as a local mod in $target"
}
