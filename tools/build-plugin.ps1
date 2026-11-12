[CmdletBinding()]
param(
    [string]$KenshiRoot = 'E:\SteamLibrary\steamapps\common\Kenshi',
    [string]$KenshiProjects = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $KenshiProjects 'KenshiLib_Examples_deps'
$toolchain = Join-Path $KenshiProjects 'toolchain'
$vc = Join-Path $toolchain 'vc100-extract\Program Files(64)\Microsoft Visual Studio 10.0\VC'
$headers = Join-Path $toolchain 'vc100-x86-extract\Program Files\Microsoft Visual Studio 10.0\VC\include'
$sdk = Join-Path $toolchain 'sdk71-build-extract\Program Files\Microsoft SDKs\Windows\v7.1'
$bin = Join-Path $vc 'bin\amd64'
$reference = Join-Path $KenshiRoot 'RE_Kenshi\kenshi_x64.exe'
$expectedEngine = '504B362CDE850D56AFB1CEA6F5B7B0EE014D9DD7B47E188599D91C804502CD3E'
if ((Get-FileHash -LiteralPath $reference).Hash -ne $expectedEngine) {
    throw 'Expected the verified Steam 1.0.65 image in the RE_Kenshi directory.'
}

$stamp = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$build = Join-Path $project "build\releases\$stamp"
$package = Join-Path $build 'package'
New-Item -ItemType Directory -Path $package -Force | Out-Null
$oldPath, $oldInclude, $oldLib = $env:PATH, $env:INCLUDE, $env:LIB

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE." }
}

Push-Location $build
try {
    $env:PATH = "$bin;$oldPath"
    $env:INCLUDE = @("$deps\KenshiLib\Include","$deps\KenshiLib\Include\ogre",
        "$deps\KenshiLib\Include\mygui","$deps\boost_1_60_0",$headers,"$sdk\Include") -join ';'
    $env:LIB = @("$deps\KenshiLib\Libraries","$vc\lib\amd64","$sdk\Lib\x64") -join ';'
    $compile = @('/nologo','/O2','/MD','/EHsc','/W3','/Zi','/DNDEBUG')
    Invoke-Checked "$bin\cl.exe" ($compile + @('/c','/GL','/Gy','/Oi','/DWIN32','/D_WIN64','/D_WINDOWS','/DUNICODE','/D_UNICODE',
        '/D_CRT_SECURE_NO_WARNINGS','/DBOOST_ALL_NO_LIB','/DBOOST_ERROR_CODE_HEADER_ONLY',
        '/DBOOST_SYSTEM_NO_DEPRECATED','/Fdfreeloader-compile.pdb',"$project\plugin\Freeloader.cpp"))
    Invoke-Checked "$bin\link.exe" @('/nologo','/DLL','/MACHINE:X64','/SUBSYSTEM:WINDOWS',
        '/LTCG','/DEBUG','/INCREMENTAL:NO','/OUT:Freeloader.dll','/PDB:Freeloader.pdb',
        'Freeloader.obj','KenshiLib.lib','kernel32.lib')
    $exports = Invoke-Checked "$bin\dumpbin.exe" @('/exports','Freeloader.dll')
    $imports = Invoke-Checked "$bin\dumpbin.exe" @('/imports','Freeloader.dll')
    if (($exports -join "`n") -notmatch '\?startPlugin@@YAXXZ' -or
        ($imports -join "`n") -notmatch '(?im)^\s+KenshiLib\.dll\s*$' -or
        (($exports + $imports) -join "`n") -match '(?i)TPL_Start|TPL\.dll|TPLLib|Freeloader\.Native') {
        throw 'Unexpected loader exports or dependencies.'
    }
    $exports | Set-Content exports.log
    $imports | Set-Content imports.log
    Copy-Item -LiteralPath 'Freeloader.dll','Freeloader.pdb' -Destination $package
    foreach ($name in 'Freeloader.ini','RE_Kenshi.json') {
        Copy-Item -LiteralPath (Join-Path "$project\packaging" $name) -Destination $package
    }
    [ordered]@{
        Status = 'verified'
        EngineSHA256 = $expectedEngine
        Files = @(Get-ChildItem -LiteralPath $package -File | Get-FileHash | ForEach-Object {
            [ordered]@{ Name = Split-Path -Leaf $_.Path; SHA256 = $_.Hash }
        })
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $build 'build-record.json')
} finally {
    Pop-Location
    $env:PATH, $env:INCLUDE, $env:LIB = $oldPath, $oldInclude, $oldLib
}
Write-Output "Verified build: $build"


