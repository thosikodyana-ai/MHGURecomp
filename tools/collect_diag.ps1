# MHGURecomp Diagnostic Bundle Collector (PowerShell)
# Collects rotated logs (log.txt, log.*.txt), recent minidumps (crash/*.dmp),
# mhgurecomp.ini, SHA-256 binary hashes, and OS/CPU/GPU system info into diag/MHGURecomp_Diag_<timestamp>.zip.
# Strictly excludes game ROM files (data/, exefs/, romfs/) and player save files (save/).

$ErrorActionPreference = "SilentlyContinue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = if (Test-Path (Join-Path $ScriptDir "mhgurecomp.exe")) { $ScriptDir } else { Split-Path -Parent $ScriptDir }
Set-Location $RootDir

$Timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$DiagBase = Join-Path $RootDir "diag"
$StagingDir = Join-Path $DiagBase "staging_$Timestamp"
$ZipPath = Join-Path $DiagBase "MHGURecomp_Diag_$Timestamp.zip"

New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StagingDir "logs_game_dir") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StagingDir "logs_localappdata") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StagingDir "crash_dumps") | Out-Null

Write-Host "[collect_diag] Collecting logs and configuration from: $RootDir"

# 1. Copy log.txt and rotated log.*.txt from game directory (and run/ if present)
Get-ChildItem -Path $RootDir -Filter "log*.txt" -File | ForEach-Object {
    Copy-Item $_.FullName -Destination (Join-Path $StagingDir "logs_game_dir") -Force
}
if (Test-Path (Join-Path $RootDir "run")) {
    Get-ChildItem -Path (Join-Path $RootDir "run") -Filter "log*.txt" -File | ForEach-Object {
        Copy-Item $_.FullName -Destination (Join-Path $StagingDir ("logs_game_dir\run_" + $_.Name)) -Force
    }
}

# 2. Copy fallback logs and crash dumps from %LOCALAPPDATA%\MHGURecomp if present
$FallbackDir = Join-Path $env:LOCALAPPDATA "MHGURecomp"
if (Test-Path $FallbackDir) {
    Get-ChildItem -Path $FallbackDir -Filter "log*.txt" -File | ForEach-Object {
        Copy-Item $_.FullName -Destination (Join-Path $StagingDir "logs_localappdata") -Force
    }
    $FallbackCrash = Join-Path $FallbackDir "crash"
    if (Test-Path $FallbackCrash) {
        Get-ChildItem -Path $FallbackCrash -Filter "*.dmp" -File | Sort-Object LastWriteTime -Descending | Select-Object -First 5 | ForEach-Object {
            Copy-Item $_.FullName -Destination (Join-Path $StagingDir ("crash_dumps\localappdata_" + $_.Name)) -Force
        }
    }
}

# 3. Copy up to 5 most recent minidumps from crash/
$CrashDir = Join-Path $RootDir "crash"
if (Test-Path $CrashDir) {
    Get-ChildItem -Path $CrashDir -Filter "*.dmp" -File | Sort-Object LastWriteTime -Descending | Select-Object -First 5 | ForEach-Object {
        Copy-Item $_.FullName -Destination (Join-Path $StagingDir "crash_dumps") -Force
    }
}

# 4. Copy mhgurecomp.ini (contains no sensitive data or saves)
if (Test-Path (Join-Path $RootDir "mhgurecomp.ini")) {
    Copy-Item (Join-Path $RootDir "mhgurecomp.ini") -Destination $StagingDir -Force
}

# 5. Generate system_summary.txt (OS, CPU, RAM, GPU, SHA-256 of mhgurecomp.exe & mhgugame.dll, non-ROM file tree)
$SysInfoFile = Join-Path $StagingDir "system_summary.txt"
$Report = @()
$Report += "================================================================================"
$Report += "MHGURecomp Diagnostic System Summary ($Timestamp)"
$Report += "================================================================================"
$Os = Get-CimInstance Win32_OperatingSystem
$Cpu = Get-CimInstance Win32_Processor
$Gpus = Get-CimInstance Win32_VideoController
$Report += "OS      : $($Os.Caption) ($($Os.Version), Build $($Os.BuildNumber), $($Os.OSArchitecture))"
$Report += "RAM     : Total $([math]::Round($Os.TotalVisibleMemorySize / 1024, 0)) MB | Free $([math]::Round($Os.FreePhysicalMemory / 1024, 0)) MB"
foreach ($c in $Cpu) {
    $Report += "CPU     : $($c.Name) (Cores=$($c.NumberOfCores), Threads=$($c.NumberOfLogicalProcessors), MaxClock=$($c.MaxClockSpeed) MHz)"
}
foreach ($g in $Gpus) {
    $Report += "GPU     : $($g.Name) | DriverVersion=$($g.DriverVersion) | AdapterRAM=$([math]::Round($g.AdapterRAM / 1MB, 0)) MB"
}
$Report += ""
$Report += "--- Binary SHA-256 Hashes ---"
foreach ($bin in @("mhgurecomp.exe", "mhgugame.dll", "build\mhgurecomp.exe", "build\mhgugame.dll")) {
    $bp = Join-Path $RootDir $bin
    if (Test-Path $bp) {
        $h = Get-FileHash -Path $bp -Algorithm SHA256
        $sz = (Get-Item $bp).Length
        $Report += "$bin ($sz bytes): $($h.Hash)"
    }
}
$Report += ""
$Report += "--- Top-Level Directory Listing (Excluding data/exefs/romfs/save) ---"
Get-ChildItem -Path $RootDir -Force | Where-Object { $_.Name -notin @("data", "exefs", "romfs", "save") } | ForEach-Object {
    $Report += ("{0,-10} {1,12} {2}" -f $_.Mode, $_.Length, $_.Name)
}
$Report | Out-File -FilePath $SysInfoFile -Encoding UTF8

# 6. Create ZIP archive
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path "$StagingDir\*" -DestinationPath $ZipPath -Force
Remove-Item -Path $StagingDir -Recurse -Force

Write-Host "[collect_diag] Diagnostic archive created successfully:"
Write-Host "  -> $ZipPath"
