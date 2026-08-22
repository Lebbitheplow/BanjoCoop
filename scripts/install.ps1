<#
    Install BanjoCoop into BanjoRecompiled's mods folder.

    Ships inside the release archives next to the two files it installs.
    It never touches your ROM or your save files.

    Usage:
        .\install.ps1
        .\install.ps1 -CheckRom "C:\path\to\rom.z64"

    If PowerShell refuses to run this, it is the execution policy, not the script:
        powershell -ExecutionPolicy Bypass -File .\install.ps1
#>

param(
    [string]$CheckRom
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Keep in step with scripts/versions.sh - that file is the source of truth.
$BanjoRecompVersion = 'v1.0.1'
$BanjoRecompRepo    = 'BanjoRecomp/BanjoRecomp'
$BanjoRecompAsset   = "BanjoRecompiled-$BanjoRecompVersion-Windows.zip"

function Write-Head($msg) { Write-Host $msg -ForegroundColor Cyan }

# ---- -CheckRom ---------------------------------------------------------------------------------
#
# BanjoRecompiled accepts only NTSC-U v1.0 and rejects anything else without explaining, so
# "my ROM doesn't work" is the likeliest reason somebody is stuck.

if ($CheckRom) {
    if (-not (Test-Path $CheckRom)) { Write-Error "no such file: $CheckRom"; exit 1 }

    $want = '1fe1632098865f639e22c11b9a81ee8f29c75d7a'
    $got  = (Get-FileHash -Algorithm SHA1 -Path $CheckRom).Hash.ToLower()
    Write-Host "sha1: $got"

    if ($got -eq $want) {
        Write-Head 'this is the correct ROM (NTSC-U v1.0)'
        exit 0
    }

    Write-Head 'this is NOT the ROM BanjoRecompiled accepts'
    Write-Host "expected $want (NTSC-U v1.0)."
    Write-Host 'rev A, PAL, and the Xbox Live Arcade version will not work.'
    Write-Host 'if yours is a .v64 or .n64 the byte order may just need converting -'
    Write-Host 'scripts/check_rom.py in the source repo does that with -o out.z64.'
    exit 1
}

# ---- locate the two files to install -------------------------------------------------------------

function Find-Artifact($name) {
    $candidates = @(
        (Join-Path $Here $name),
        (Join-Path $Here "build\$name"),
        (Join-Path $Here "build-native\$name"),
        (Join-Path (Split-Path -Parent $Here) "build\$name"),
        (Join-Path (Split-Path -Parent $Here) "build-native\$name")
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

$Nrm = Find-Artifact 'banjocoop.nrm'
$Lib = Find-Artifact 'banjocoop_net.dll'
if (-not $Nrm) { Write-Error 'banjocoop.nrm not found next to this script'; exit 1 }
if (-not $Lib) { Write-Error 'banjocoop_net.dll not found next to this script'; exit 1 }

# ---- locate BanjoRecompiled ----------------------------------------------------------------------
#
# What matters is the config directory, not where the executable was unpacked: the runtime loads
# mods from %LOCALAPPDATA%\BanjoRecompiled\mods wherever it lives.

$ConfigDir = Join-Path $env:LOCALAPPDATA 'BanjoRecompiled'
$ModsDir   = Join-Path $ConfigDir 'mods'

$runtimePresent = (Test-Path $ConfigDir) -or
                  (Test-Path (Join-Path $Here 'BanjoRecompiled.exe')) -or
                  (Test-Path (Join-Path (Split-Path -Parent $Here) 'BanjoRecompiled.exe'))

if (-not $runtimePresent) {
    Write-Head 'no BanjoRecompiled install found.'
    Write-Host ''
    Write-Host 'BanjoCoop is a mod - it needs BanjoRecompiled to run in.'
    Write-Host "download it now? ($BanjoRecompVersion, about 19 MB)"
    $reply = Read-Host '  [y/N]'

    if ($reply -match '^[yY]') {
        $url = "https://github.com/$BanjoRecompRepo/releases/download/$BanjoRecompVersion/$BanjoRecompAsset"
        $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString())
        New-Item -ItemType Directory -Path $tmp -Force | Out-Null
        try {
            $zip = Join-Path $tmp 'rt.zip'
            Write-Host "  fetching $BanjoRecompAsset"
            # Far faster than the default, which renders a progress bar per byte.
            $prev = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $url -OutFile $zip
            $ProgressPreference = $prev

            Expand-Archive -Path $zip -DestinationPath (Join-Path $tmp 'rt') -Force
            $exe = Get-ChildItem -Path (Join-Path $tmp 'rt') -Filter 'BanjoRecompiled.exe' -Recurse |
                   Select-Object -First 1
            if (-not $exe) { Write-Error 'unexpected archive layout'; exit 1 }

            $dest = Join-Path $Here "BanjoRecompiled-$BanjoRecompVersion"
            New-Item -ItemType Directory -Path $dest -Force | Out-Null
            Copy-Item -Path (Join-Path $exe.DirectoryName '*') -Destination $dest -Recurse -Force
            Write-Host "  unpacked -> BanjoRecompiled-$BanjoRecompVersion\"
        } finally {
            Remove-Item -Path $tmp -Recurse -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host ''
        Write-Host 'get it from:'
        Write-Host "  https://github.com/$BanjoRecompRepo/releases"
        Write-Host ''
        Write-Host 'run it once - it will ask where your Banjo-Kazooie ROM is - then re-run this script.'
        exit 1
    }
}

# ---- install ---------------------------------------------------------------------------------------
#
# Both files, side by side. The runtime loads the native library from next to the .nrm rather than
# from inside it, so one without the other is a mod that loads and then cannot find its networking.

New-Item -ItemType Directory -Path $ModsDir -Force | Out-Null
Copy-Item -Path $Nrm -Destination $ModsDir -Force
Copy-Item -Path $Lib -Destination $ModsDir -Force

# cloudflared, if it shipped in this archive, goes beside the library so the Cloudflare-tunnel
# connection mode finds it. Optional: Direct (UDP) play does not need it.
$Cf = Find-Artifact 'cloudflared.exe'
if ($Cf) { Copy-Item -Path $Cf -Destination $ModsDir -Force }

Write-Head 'BanjoCoop installed'
Write-Host "  $ModsDir\banjocoop.nrm"
Write-Host "  $ModsDir\banjocoop_net.dll"
if ($Cf) { Write-Host "  $ModsDir\cloudflared.exe  (for the Cloudflare Tunnel connection mode)" }
Write-Host ''
Write-Host 'next:'
Write-Host '  1. run BanjoRecompiled (it asks for your own Banjo-Kazooie ROM, NTSC-U v1.0,'
Write-Host "     the first time, and copies it into its own folder)"
Write-Host '  2. open the mod menu and enable BanjoCoop'
Write-Host '  3. set Network Mode to Host, or to Join with the host address'
Write-Host '     default port is 34567/UDP'
Write-Host ''
Write-Host 'stuck on the ROM?  .\install.ps1 -CheckRom <your-rom>'
