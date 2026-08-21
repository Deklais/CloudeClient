param(
	[string]$BuildDir = "out\build\x64-Release-vulkan",
	[string]$OutputDir = "out\portable\AstralClient"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Resolve-Path (Join-Path $Root $BuildDir)
$OutputParent = Join-Path $Root "out\portable"
$OutputPath = Join-Path $Root $OutputDir
$ResolvedOutputParent = [System.IO.Path]::GetFullPath($OutputParent)
$ResolvedOutputPath = [System.IO.Path]::GetFullPath($OutputPath)

if(-not $ResolvedOutputPath.StartsWith($ResolvedOutputParent, [System.StringComparison]::OrdinalIgnoreCase))
{
	throw "Refusing to write outside out\portable: $ResolvedOutputPath"
}

if(-not (Test-Path (Join-Path $BuildPath "DDNet.exe")))
{
	throw "DDNet.exe was not found in $BuildPath. Build the client first."
}

if(Test-Path $ResolvedOutputPath)
{
	Remove-Item -LiteralPath $ResolvedOutputPath -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $ResolvedOutputPath | Out-Null

Copy-Item -LiteralPath (Join-Path $BuildPath "DDNet.exe") -Destination $ResolvedOutputPath -Force
Copy-Item -LiteralPath (Join-Path $BuildPath "data") -Destination $ResolvedOutputPath -Recurse -Force
Copy-Item -LiteralPath (Join-Path $Root "data") -Destination $ResolvedOutputPath -Recurse -Force

Get-ChildItem -LiteralPath $BuildPath -Filter "*.dll" -File | ForEach-Object {
	Copy-Item -LiteralPath $_.FullName -Destination $ResolvedOutputPath -Force
}

$OptionalFiles = @(
	"steam_appid.txt",
	"storage.cfg"
)

foreach($File in $OptionalFiles)
{
	$BuildFile = Join-Path $BuildPath $File
	$RootFile = Join-Path $Root $File
	if(Test-Path $BuildFile)
	{
		Copy-Item -LiteralPath $BuildFile -Destination $ResolvedOutputPath -Force
	}
	elseif(Test-Path $RootFile)
	{
		Copy-Item -LiteralPath $RootFile -Destination $ResolvedOutputPath -Force
	}
}

Write-Host "AstralClient portable build is ready:"
Write-Host $ResolvedOutputPath
