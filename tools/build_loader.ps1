# Build the AxiomOS UEFI loader on a Windows host with mingw-w64.
#
# Produces build\BOOTX64.EFI.  The loader runs in the firmware environment, so
# every compilation unit must be compiled without SSE/MMX: some firmware
# revisions leave CR0.EM set, which turns any SSE instruction into #UD.

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$gcc     = 'D:\mingw64\bin\x86_64-w64-mingw32-gcc.exe'
$out     = Join-Path $root 'build\uefi-win'
$sources = Get-ChildItem (Join-Path $root 'boot\uefi') -Filter *.c
$asm     = Join-Path $root 'boot\uefi\jump.S'

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$flags = @(
    '-ffreestanding', '-fno-stack-protector', '-fno-stack-check',
    '-mno-red-zone', '-mno-stack-arg-probe', '-O2', '-Wall', '-Wextra',
    '-Wno-unused-parameter', '-mno-sse', '-mno-mmx', '-mno-sse2',
    '-mgeneral-regs-only',
    '-Iboot/uefi', '-Iboot'
)

$objDir = Join-Path $out 'obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

Push-Location $root
try {
    foreach ($src in $sources) {
        $obj = Join-Path $objDir (($src.BaseName) + '.o')
        & $gcc @flags -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $($src.Name)" }
    }

    $jumpObj = Join-Path $objDir 'jump.o'
    & $gcc @flags -c $asm -o $jumpObj
    if ($LASTEXITCODE -ne 0) { throw 'compile failed: jump.S' }

    $objs = @(Get-ChildItem $objDir -Filter *.o | ForEach-Object { $_.FullName })
    & $gcc -nostdlib '-Wl,--subsystem,10' '-Wl,-e,efi_main' '-Wl,--no-insert-timestamp' `
        -o (Join-Path $out 'BOOTX64.EFI') @objs
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }
}
finally {
    Pop-Location
}

Get-Item (Join-Path $out 'BOOTX64.EFI') | Select-Object FullName, Length, LastWriteTime
