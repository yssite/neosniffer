$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir = Join-Path $root "src"
$mcpDir = Join-Path $root "mcp"
$outDir = Join-Path $root "out"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# stop running copies so the output files can be replaced
Get-Process SpaceSniffer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$clang = (Get-Command clang++ -ErrorAction SilentlyContinue).Source
if (-not $clang) { throw "clang++ not found in PATH (install llvm-mingw)" }

$common = @(
    "-std=c++20", "-O2", "-municode", "-static",
    "-finput-charset=UTF-8", "-fexec-charset=UTF-8",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
    "-DUNICODE", "-D_UNICODE", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN"
)

$libs = @(
    "-lgdi32", "-luser32", "-lcomctl32", "-ldwmapi", "-lshell32", "-lole32",
    "-luxtheme", "-lshlwapi", "-ladvapi32", "-lmsimg32", "-lversion", "-lcomdlg32",
    "-lgdiplus", "-lwinmm", "-lpathcch", "-luuid", "-luxtheme"
)

function Build-Target {
    param([string]$Name, [string]$Subsystem, [string[]]$Sources, [string[]]$ExtraObjs = @())

    $objDir = Join-Path $outDir "obj_$Name"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $objs = @()
    $i = 0
    foreach ($s in $Sources) {
        $obj = Join-Path $objDir ("{0:D3}_{1}.o" -f $i, [IO.Path]::GetFileNameWithoutExtension($s))
        $i++
        & $clang @common -c $s -o $obj
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
        $objs += $obj
    }
    $objs += $ExtraObjs
    $exe = Join-Path $outDir "$Name.exe"
    if (Test-Path $exe) { Remove-Item $exe -Force -ErrorAction SilentlyContinue }
    & $clang @common "-Wl,--subsystem,$Subsystem" -o $exe @objs @libs
    if ($LASTEXITCODE -ne 0) { throw "link failed: $Name" }
    $len = (Get-Item $exe).Length
    Write-Host ("  built {0}  ({1:N0} bytes)" -f $exe, $len)
}

$srcSources = @(Get-ChildItem -Path $srcDir -Filter *.cpp -File | ForEach-Object { $_.FullName })
$coreSources = @(Get-ChildItem -Path (Join-Path $srcDir "core") -Filter *.cpp -File -ErrorAction SilentlyContinue |
    ForEach-Object { $_.FullName })
$coreImpl = @()
foreach ($f in @("core_scan.cpp", "core_features.cpp")) {
    $full = Join-Path $srcDir $f
    if (Test-Path $full) { $coreImpl += $full }
}
$mcpOwn = @()
if (Test-Path $mcpDir) {
    $mcpOwn = @(Get-ChildItem -Path $mcpDir -Filter *.cpp -File | ForEach-Object { $_.FullName })
}
$mcpSources = @()
if ($mcpOwn.Count -gt 0) { $mcpSources = @($coreImpl + $mcpOwn) }

Write-Host "Building native SpaceSniffer..."
$resObjs = @()
$rcFile = Join-Path $root "app.rc"
if (Test-Path $rcFile) {
    $icoFile = Join-Path $root "assets\app.ico"
    $iconScript = Join-Path (Split-Path -Parent $root) "tools\make_icon.py"
    if ((-not (Test-Path $icoFile)) -and (Test-Path $iconScript)) {
        $py = (Get-Command python -ErrorAction SilentlyContinue).Source
        if (-not $py) { $py = (Get-Command py -ErrorAction SilentlyContinue).Source }
        if ($py) {
            & $py $iconScript
            if ($LASTEXITCODE -ne 0) { throw "icon generation failed: $iconScript" }
        }
    }
    $rcObjDir = Join-Path $outDir "obj_SpaceSniffer"
    New-Item -ItemType Directory -Force -Path $rcObjDir | Out-Null
    $windres = (Get-Command windres -ErrorAction SilentlyContinue).Source
    if (-not $windres) { $windres = (Get-Command llvm-windres -ErrorAction SilentlyContinue).Source }
    if (-not $windres) { $windres = (Get-Command x86_64-w64-mingw32-windres -ErrorAction SilentlyContinue).Source }
    $llvmRc = (Get-Command llvm-rc -ErrorAction SilentlyContinue).Source
    if ($windres) {
        $resObj = Join-Path $rcObjDir "app.res.o"
        & $windres -I $root -i $rcFile -O coff -o $resObj
        if ($LASTEXITCODE -ne 0) { throw "windres failed: $rcFile" }
        $resObjs += $resObj
    } elseif ($llvmRc) {
        $ppRc = Join-Path $rcObjDir "app.pp.rc"
        & $clang -E -x c -P -DRC_INVOKED $rcFile -o $ppRc
        if ($LASTEXITCODE -ne 0) { throw "rc preprocess failed: $rcFile" }
        $resFile = Join-Path $rcObjDir "app.res"
        & $llvmRc /I $root /FO $resFile $ppRc
        if ($LASTEXITCODE -ne 0) { throw "llvm-rc failed: $rcFile" }
        $resObjs += $resFile
    } else {
        throw "Neither windres nor llvm-rc found in PATH"
    }
}
Build-Target -Name "SpaceSniffer" -Subsystem "windows" -Sources $srcSources -ExtraObjs $resObjs

if ($mcpSources.Count -gt 0) {
    Build-Target -Name "SpaceSniffer.Mcp" -Subsystem "console" -Sources $mcpSources
}

Write-Host "Done. Output: $outDir"

