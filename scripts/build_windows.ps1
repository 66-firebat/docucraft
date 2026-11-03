param(
    [string]$W64Devkit = "D:\w64devkit",
    [string]$CMake = "cmake",
    [int]$Jobs = 4,
    [switch]$RebuildMuPDF,
    [switch]$NoGui
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$mupdf = Join-Path $root "cpp\third_party\mupdf-src"
$build = Join-Path $root "build_windows"
$sdk = Join-Path $build "mupdf-sdk"

$mupdfBuild = Join-Path $mupdf "docucraft\build.ps1"
if (-not (Test-Path $mupdfBuild)) {
    throw "MuPDF submodule missing: git submodule update --init --recursive"
}
$make = Join-Path $W64Devkit "bin\make.exe"
if (-not (Test-Path $make)) { throw "w64devkit make not found: $make" }

if ($RebuildMuPDF -or -not (Test-Path (Join-Path $sdk "lib\x86_64\libmupdf.a"))) {
    & $mupdfBuild -Target windows -Sdk $sdk -W64Devkit $W64Devkit -Jobs $Jobs
}

$gui = if ($NoGui) { "OFF" } else { "ON" }
$savedPath = $env:PATH
$env:PATH = "$(Join-Path $W64Devkit 'bin');$env:PATH"
try {
    & $CMake -S (Join-Path $root "cpp") -B $build -G "MinGW Makefiles" `
        "-DCMAKE_MAKE_PROGRAM=$make" `
        "-DCMAKE_C_COMPILER=gcc" `
        "-DCMAKE_CXX_COMPILER=g++" `
        "-DCMAKE_BUILD_TYPE=Release" `
        "-DHARDCOPY_BUILD_CLI=ON" `
        "-DHARDCOPY_BUILD_GUI=$gui" `
        "-DHARDCOPY_TEXT_MODE=ON" `
        "-DHARDCOPY_VERTICAL_LAYOUT=ON" `
        "-DHARDCOPY_EPUB=ON" `
        "-DHARDCOPY_COMIC=ON" `
        "-DHARDCOPY_DOCX=OFF" `
        "-DHARDCOPY_PDFIN=ON" `
        "-DHARDCOPY_PARALLEL=ON" `
        "-DMUPDF_ROOT=$sdk"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    & $CMake --build $build -j $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
}
finally {
    $env:PATH = $savedPath
}

$cli = Join-Path $build "cli\hardcopy-cli.exe"
Write-Host "hardcopy-cli: $cli"
if (-not $NoGui) {
    $guiDir = Join-Path $build "gui"
    Copy-Item -Force $cli $guiDir
    Write-Host "hardcopy-gui: $(Join-Path $guiDir 'hardcopy-gui.exe')"
}
