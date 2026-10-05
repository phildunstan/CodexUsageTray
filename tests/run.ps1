$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
[xml]$project = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\CodexUsageTray.vcxproj') -Raw
$namespace = [System.Xml.XmlNamespaceManager]::new($project.NameTable)
$namespace.AddNamespace('msb', $project.DocumentElement.NamespaceURI)
$project.SelectSingleNode('//msb:ClCompile[@Include]', $namespace).SetAttribute('Include', '$(TestSource)')
foreach ($subsystem in $project.SelectNodes('//msb:SubSystem', $namespace)) { $subsystem.InnerText = 'Console' }
$projectPath = Join-Path $PSScriptRoot 'bin\GrokTests.vcxproj'
New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'bin') -Force | Out-Null
$project.Save($projectPath)
foreach ($name in @('fake_grok', 'grok_refresh_tests', 'fake_codex', 'codex_refresh_tests')) {
    & $msbuild $projectPath /nologo /verbosity:quiet /p:Configuration=Release /p:Platform=x64 "/p:TestSource=$PSScriptRoot\$name.cpp" "/p:TargetName=$name" "/p:OutDir=$PSScriptRoot\bin\" "/p:IntDir=$PSScriptRoot\bin\$name\"
    if ($LASTEXITCODE -ne 0) { throw "Building $name failed" }
}
& (Join-Path $PSScriptRoot 'bin\grok_refresh_tests.exe') (Join-Path $PSScriptRoot 'bin\fake_grok.exe') (Join-Path $PSScriptRoot 'bin\events.txt')
if ($LASTEXITCODE -ne 0) { throw 'Grok refresh tests failed' }
& (Join-Path $PSScriptRoot 'bin\codex_refresh_tests.exe') (Join-Path $PSScriptRoot 'bin\fake_codex.exe') (Join-Path $PSScriptRoot 'bin\codex-events.txt')
if ($LASTEXITCODE -ne 0) { throw 'Codex refresh tests failed' }
