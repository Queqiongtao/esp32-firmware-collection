param(
    [string]$OutputDir
)

$ErrorActionPreference = "Continue"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (!$OutputDir) {
    $distDir = Join-Path $ProjectRoot "dist"
    if (Test-Path $distDir) {
        $OutputDir = Join-Path $distDir "diagnostics"
    } else {
        $OutputDir = Join-Path $ProjectRoot "diagnostics"
    }
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$reportPath = Join-Path $OutputDir "diagnose_$timestamp.txt"

function Add-Section([string]$Title) {
    Add-Content -LiteralPath $reportPath -Value ""
    Add-Content -LiteralPath $reportPath -Value "==== $Title ===="
}

function Add-CommandOutput([string]$Title, [scriptblock]$Command) {
    Add-Section $Title
    try {
        $output = & $Command 2>&1 | Out-String -Width 240
        if ([string]::IsNullOrWhiteSpace($output)) {
            $output = "<no output>"
        }
        Add-Content -LiteralPath $reportPath -Value $output
    } catch {
        Add-Content -LiteralPath $reportPath -Value ("ERROR: " + $_.Exception.Message)
    }
}

"AMOLED Subscreen IDD Driver Diagnostic Report" | Set-Content -LiteralPath $reportPath -Encoding UTF8
Add-Content -LiteralPath $reportPath -Value ("Generated: " + (Get-Date).ToString("yyyy-MM-dd HH:mm:ss zzz"))
Add-Content -LiteralPath $reportPath -Value ("Computer: " + $env:COMPUTERNAME)
Add-Content -LiteralPath $reportPath -Value ("User: " + $env:USERNAME)

Add-CommandOutput "OS" {
    Get-CimInstance Win32_OperatingSystem |
        Select-Object Caption,Version,BuildNumber,OSArchitecture |
        Format-List
}

Add-CommandOutput "Current Test Signing State" {
    bcdedit /enum
}

Add-CommandOutput "Connected USB VID/PID Devices" {
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like "USB\VID_*" } |
        Select-Object Status,Class,FriendlyName,InstanceId,Problem |
        Sort-Object FriendlyName |
        Format-Table -AutoSize -Wrap
}

Add-CommandOutput "Target Or ESP32 Historical Devices" {
    Get-PnpDevice |
        Where-Object {
            $_.InstanceId -like "*VID_303A*" -or
            $_.InstanceId -like "*PID_2987*" -or
            $_.FriendlyName -like "*AMOLED*" -or
            $_.FriendlyName -like "*Subscreen*" -or
            $_.FriendlyName -like "*USB Display*"
        } |
        Select-Object Status,Class,FriendlyName,InstanceId,Problem |
        Format-List
}

Add-CommandOutput "Connected Problem Devices" {
    pnputil /enum-devices /connected /problem
}

Add-CommandOutput "AMOLED Driver Packages" {
    pnputil /enum-drivers |
        Select-String -Pattern "Published Name|Original Name|Provider Name|Class Name|Driver Version|Signer Name|amoled|subscreen|xfz1986|303A|2987" -Context 0,2
}

Add-CommandOutput "Project UMDF And IddCx Settings" {
    $inf = Join-Path $ProjectRoot "amoled_subscreen_idd.inf"
    $vcxproj = Join-Path $ProjectRoot "idd_xfz1986_usb_graphic.vcxproj"

    if (Test-Path $inf) {
        "--- INF ---"
        Select-String -LiteralPath $inf -Pattern "UmdfLibraryVersion|UmdfExtensions|UmdfDispatcher|UmdfService|UpperFilters"
    }

    if (Test-Path $vcxproj) {
        "--- VCXPROJ ---"
        Select-String -LiteralPath $vcxproj -Pattern "UMDF_VERSION_MAJOR|UMDF_VERSION_MINOR|IDDCX_VERSION_MAJOR|IDDCX_VERSION_MINOR|TimeStamp"
    }
}

Add-CommandOutput "Recent SetupAPI WDF And AMOLED Lines" {
    $setupApiLog = Join-Path $env:windir "inf\setupapi.dev.log"
    if (Test-Path $setupApiLog) {
        Get-Content -LiteralPath $setupApiLog -Tail 1200 |
            Select-String -Pattern "VID_303A&PID_2987|amoled_subscreen|xfz1986|CM_PROB|FAILED_ADD|problem status|WDF schema|UmdfLibraryVersion|UmdfExtensions|CoInstaller|0x00000057|0xc0000001" -Context 2,3
    } else {
        "setupapi.dev.log not found: $setupApiLog"
    }
}

Add-CommandOutput "Latest Installer Logs" {
    $dirs = Get-ChildItem -Path $env:TEMP -Directory -Filter "amoled_subscreen_idd_installer_*" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 5

    foreach ($dir in $dirs) {
        "DIR: $($dir.FullName)"
        Get-ChildItem -LiteralPath $dir.FullName -Force |
            Select-Object Name,Length,LastWriteTime |
            Format-Table -AutoSize |
            Out-String -Width 240

        $log = Join-Path $dir.FullName "install.log"
        if (Test-Path $log) {
            "--- install.log ---"
            Get-Content -LiteralPath $log -Tail 200
        }
        ""
    }
}

Add-CommandOutput "Recent WUDF And Kernel-PnP System Events" {
    Get-WinEvent -LogName System -MaxEvents 300 -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ProviderName -match "Kernel-PnP|DriverFrameworks|WUDF" -or
            $_.Message -match "VID_303A|PID_2987|amoled|xfz1986|WUDF|UMDF|IddCx|Indirect"
        } |
        Select-Object -First 80 TimeCreated,Id,ProviderName,LevelDisplayName,Message |
        Format-List
}

Add-CommandOutput "DriverFrameworks UserMode Log State" {
    Get-WinEvent -ListLog "Microsoft-Windows-DriverFrameworks-UserMode/Operational" -ErrorAction SilentlyContinue |
        Select-Object LogName,RecordCount,IsEnabled |
        Format-List
}

Write-Host "Diagnostic report generated:"
Write-Host "  $reportPath"
