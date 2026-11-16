param(
    [string]$Compiler = 'i686-w64-mingw32-clang++.exe',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build')
)

$ErrorActionPreference = 'Stop'
$compilerPath = (Get-Command $Compiler -CommandType Application).Source
$stripPath = Join-Path (Split-Path $compilerPath -Parent) 'llvm-strip.exe'
$output = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$commonFlags = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                 '-Wl,--no-insert-timestamp')
$modSource = Join-Path $PSScriptRoot 'split.cpp'
$modBinary = Join-Path $output 'TCS-Split-Prototype.dll'
$loaderSource = Join-Path $PSScriptRoot 'attach.cpp'
$loaderBinary = Join-Path $output 'Enable-Split.exe'

& $compilerPath @commonFlags -shared '-Wl,--kill-at' $modSource -o $modBinary
if ($LASTEXITCODE -ne 0) { throw 'The mod DLL failed to build.' }

& $stripPath --strip-all $modBinary
if ($LASTEXITCODE -ne 0) { throw 'Could not strip the mod DLL debug information.' }

& $compilerPath @commonFlags -s -municode $loaderSource -lbcrypt -o $loaderBinary
if ($LASTEXITCODE -ne 0) { throw 'The mod launcher failed to build.' }
