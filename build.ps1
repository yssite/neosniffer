param(
    [ValidateSet("x64", "arm64")]
    [string]$Target = "x64",
    [switch]$Test
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir = Join-Path $root "src"
$testDir = Join-Path $root "test"
$outDir = if ($Target -eq "x64") { Join-Path $root "out" } else { Join-Path (Join-Path $root "out") $Target }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Resolve toolchain for the selected target
if ($Target -eq "arm64") {
    $clang = (Get-Command aarch64-w64-mingw32-clang++ -ErrorAction SilentlyContinue).Source
    if (-not $clang) {
        Write-Host "ERROR: ARM64 cross-compiler 'aarch64-w64-mingw32-clang++' was not found in PATH." -ForegroundColor Red
        Write-Host "Install an llvm-mingw 20.x release with aarch64 support (https://github.com/mstorsjo/llvm-mingw/releases) or build with -Target x64." -ForegroundColor Yellow
        exit 1
    }
    $windres = (Get-Command aarch64-w64-mingw32-windres -ErrorAction SilentlyContinue).Source
    $windresTarget = "aarch64-w64-mingw32"
} else {
    $clang = (Get-Command x86_64-w64-mingw32-clang++ -ErrorAction SilentlyContinue).Source
    if (-not $clang) { $clang = (Get-Command clang++ -ErrorAction SilentlyContinue).Source }
    if (-not $clang) {
        Write-Host "ERROR: 'clang++' (llvm-mingw 20.x) was not found in PATH." -ForegroundColor Red
        Write-Host "Install llvm-mingw from https://github.com/mstorsjo/llvm-mingw/releases and add its bin/ directory to PATH." -ForegroundColor Yellow
        exit 1
    }
    $windres = (Get-Command x86_64-w64-mingw32-windres -ErrorAction SilentlyContinue).Source
    if (-not $windres) { $windres = (Get-Command llvm-windres -ErrorAction SilentlyContinue).Source }
    if (-not $windres) { $windres = (Get-Command windres -ErrorAction SilentlyContinue).Source }
    $windresTarget = "x86_64-w64-mingw32"
}

# Stop running copies so the output binaries can be replaced
foreach ($proc in @("NeoSniffer", "NeoSniffer.Mcp")) {
    Get-Process $proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 200

$common = @(
    "-std=c++20", "-O2", "-municode", "-static",
    "-finput-charset=UTF-8", "-fexec-charset=UTF-8",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
    "-DUNICODE", "-D_UNICODE", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN"
)

$guiLibs = @(
    "-lgdi32", "-luser32", "-lcomctl32", "-ldwmapi", "-lshell32", "-lole32",
    "-luxtheme", "-lshlwapi", "-ladvapi32", "-lmsimg32", "-lversion", "-lcomdlg32",
    "-lgdiplus", "-lwinmm", "-lpathcch", "-luuid"
)

$consoleLibs = @(
    "-ladvapi32"
)

function Build-Binary {
    param(
        [string]$Name,
        [string]$Subsystem,
        [string[]]$Sources,
        [string[]]$Libs,
        [string[]]$ExtraFlags = @(),
        [string[]]$ExtraObjs = @()
    )

    $objDir = Join-Path $outDir "obj_$Name"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $objs = @()
    $i = 0
    foreach ($s in $Sources) {
        if (-not (Test-Path $s)) { throw "missing source file: $s" }
        $obj = Join-Path $objDir ("{0:D3}_{1}.o" -f $i, [IO.Path]::GetFileNameWithoutExtension($s))
        $i++
        & $clang @common @ExtraFlags -I $srcDir -c $s -o $obj
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
        $objs += $obj
    }
    $objs += $ExtraObjs
    $exe = Join-Path $outDir "$Name.exe"
    if (Test-Path $exe) { Remove-Item $exe -Force -ErrorAction SilentlyContinue }
    & $clang @common "-Wl,--subsystem,$Subsystem" -o $exe @objs @Libs
    if ($LASTEXITCODE -ne 0) { throw "link failed: $Name" }
    $len = (Get-Item $exe).Length
    Write-Host ("  built {0}  ({1:N0} bytes)" -f $exe, $len)
    return $exe
}

$guiSources = @(
    (Join-Path $srcDir "main.cpp"),
    (Join-Path $srcDir "core_scan.cpp"),
    (Join-Path $srcDir "core_features.cpp"),
    (Join-Path $srcDir "ui_treemap.cpp"),
    (Join-Path $srcDir "ui_config.cpp"),
    (Join-Path $srcDir "i18n.cpp")
)

$mcpSources = @(
    (Join-Path $srcDir "core_scan.cpp"),
    (Join-Path $srcDir "core_features.cpp"),
    (Join-Path $srcDir "mcp_main.cpp")
)

Write-Host "Building NeoSniffer ($Target)..."
$resObjs = @()
$rcFile = Join-Path $root "app.rc"
$icoFile = Join-Path $srcDir "app.ico"
if ((Test-Path $rcFile) -and (Test-Path $icoFile)) {
    $rcObjDir = Join-Path $outDir "obj_NeoSniffer"
    New-Item -ItemType Directory -Force -Path $rcObjDir | Out-Null
    $llvmRc = (Get-Command llvm-rc -ErrorAction SilentlyContinue).Source
    if ($windres) {
        $resObj = Join-Path $rcObjDir "app.res.o"
        & $windres -I $root -i $rcFile -O coff -o $resObj
        if ($LASTEXITCODE -ne 0) { throw "windres failed: $rcFile" }
        $resObjs += $resObj
    } elseif ($llvmRc) {
        $ppRc = Join-Path $rcObjDir "app.pp.rc"
        & $clang -E -x c -P -DRC_INVOKED -I $root $rcFile -o $ppRc
        if ($LASTEXITCODE -ne 0) { throw "rc preprocess failed: $rcFile" }
        $resFile = Join-Path $rcObjDir "app.res"
        & $llvmRc /I $root /FO $resFile $ppRc
        if ($LASTEXITCODE -ne 0) { throw "llvm-rc failed: $rcFile" }
        $resObjs += $resFile
    } else {
        Write-Warning "Neither windres ($windresTarget) nor llvm-rc found in PATH; skipping app.rc embedding."
    }
} elseif (Test-Path $rcFile) {
    Write-Warning "src/app.ico not found; skipping app.rc resource embedding."
}

$null = Build-Binary -Name "NeoSniffer" -Subsystem "windows" -Sources $guiSources -Libs $guiLibs -ExtraObjs $resObjs
$null = Build-Binary -Name "NeoSniffer.Mcp" -Subsystem "console" -Sources $mcpSources -Libs $consoleLibs

if ($Test) {
    $mftTestSrc = Join-Path $testDir "mfttest.cpp"
    if (-not (Test-Path $mftTestSrc)) { throw "missing test file: $mftTestSrc" }
    Write-Host "Building unit tests ($Target)..."
    $testSources = @(
        (Join-Path $srcDir "core_scan.cpp"),
        (Join-Path $srcDir "core_features.cpp"),
        $mftTestSrc
    )
    $testExe = Build-Binary -Name "mfttest" -Subsystem "console" -Sources $testSources -Libs $consoleLibs -ExtraFlags @("-DSS_TEST_HOOKS")
    if ($Target -eq "x64" -or $env:PROCESSOR_ARCHITECTURE -eq "ARM64") {
        Write-Host "Running mfttest..."
        & $testExe
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Unit tests FAILED (exit code $LASTEXITCODE)." -ForegroundColor Red
            exit $LASTEXITCODE
        }
    } else {
        Write-Host "Skipping execution of $Target test binary on $($env:PROCESSOR_ARCHITECTURE) host."
    }
}

Write-Host "Done. Output: $outDir"
