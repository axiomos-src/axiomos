# Boot AxiomOS in the VMware EFI VM with a given BOOTX64.EFI and dump COM1.
#
#   .\vmtest.ps1                      # boot build\uefi-win\BOOTX64.EFI
#   .\vmtest.ps1 -BootFile foo.efi -WaitSeconds 40 -Tag run7
#
# The image under test is D:\Axiom-OS\vm\axiom-flat.img.  It is refreshed from
# D:\Axiom-OS\vm\esp.vhd (a Windows-formatted FAT32 volume) so the FAT layout
# is always one the firmware is known to accept.

param(
    [string]$BootFile   = 'D:\Axiom-OS\build\uefi-win\BOOTX64.EFI',
    [string]$KernelFile = '',
    [int]$WaitSeconds   = 25,
    [string]$Tag        = 'run'
)

$ErrorActionPreference = 'Stop'
$vmDir  = 'D:\Axiom-OS\vm'
$vmx    = Join-Path $vmDir 'axiomos.vmx'
$img    = Join-Path $vmDir 'axiom-flat.img'
$vhd    = Join-Path $vmDir 'esp.vhd'
$diskpart = "$env:SystemRoot\System32\diskpart.exe"
$vmrun    = 'D:\vmare\vmrun.exe'
$shot     = 'D:\Axiom-OS\tools\vncshot.py'

function Invoke-Diskpart([string]$commands) {
    $tmp = Join-Path $vmDir 'dp-tmp.txt'
    $commands | Set-Content $tmp -Encoding ASCII
    & $diskpart /s $tmp | Out-Null
}

# 1. stop the VM if it is up ------------------------------------------------
# vmrun's stop/start handshake can block behind VMware's modal dialogs, so the
# worker processes are terminated directly; they flush their logs on exit.
Get-Process -Name 'vmware-vmx', 'vmware' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 3

# 2. refresh the ESP volume -------------------------------------------------
Invoke-Diskpart "select vdisk file=$vhd`nattach vdisk`nexit`n"
Copy-Item $BootFile 'Z:\EFI\BOOT\BOOTX64.EFI' -Force
if ($KernelFile) {
    New-Item -ItemType Directory -Force -Path 'Z:\axiomos' | Out-Null
    Copy-Item $KernelFile 'Z:\axiomos\kernel.elf' -Force
}
Remove-Item 'Z:\axiomos\bootlog.txt' -Force -ErrorAction SilentlyContinue
Invoke-Diskpart "select vdisk file=$vhd`ndetach vdisk`nexit`n"

# 3. materialise the flat disk ---------------------------------------------
$fs = [System.IO.File]::Open($vhd, 'Open', 'Read')
try {
    $buf = New-Object byte[] 67108864
    $fs.Read($buf, 0, 67108864) | Out-Null
}
finally { $fs.Close() }
[System.IO.File]::WriteAllBytes($img, $buf)

# 4. fresh firmware state + fresh serial file ------------------------------
Remove-Item (Join-Path $vmDir 'nvram') -Force -ErrorAction SilentlyContinue
$serialName = "serial-$Tag.log"
(Get-Content $vmx) -replace 'serial0\.fileName = ".*"', "serial0.fileName = `"$serialName`"" |
    Set-Content $vmx

# 5. boot and watch --------------------------------------------------------
Start-Process -FilePath $vmrun -ArgumentList @('-T', 'ws', 'start', $vmx) -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds $WaitSeconds

$alive = (Get-Process -Name 'vmware-vmx' -ErrorAction SilentlyContinue) -ne $null
$serial = Join-Path $vmDir $serialName
$text = if (Test-Path $serial) { Get-Content $serial -Raw } else { '' }

"===== VM alive: $alive ====="
"===== COM1 ($serialName) ====="
if ($text) { $text } else { '(no serial output)' }

# 6. screenshot (best effort) ----------------------------------------------
try { & python $shot 127.0.0.1 5900 (Join-Path $vmDir "shot-$Tag.png") | Out-Null } catch {}
