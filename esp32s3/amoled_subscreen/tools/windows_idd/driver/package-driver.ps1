param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64")]
    [string]$Platform = "x64",

    [switch]$SkipBuild,
    [switch]$SkipSign,
    [switch]$TrustLocalMachine
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectFile = Join-Path $ProjectRoot "idd_xfz1986_usb_graphic.vcxproj"
$InfFile = Join-Path $ProjectRoot "amoled_subscreen_idd.inf"
$DiagnosticScriptFile = Join-Path $ProjectRoot "diagnose-driver.ps1"
$OutputDll = Join-Path $ProjectRoot "$Platform\$Configuration\xfz1986_usb_graphic.dll"
$PackageDir = Join-Path $ProjectRoot "dist\driver\$Platform\$Configuration"
$PackageInf = Join-Path $PackageDir "amoled_subscreen_idd.inf"
$PackageCat = Join-Path $PackageDir "amoled_subscreen_idd.cat"
$PackageDll = Join-Path $PackageDir "xfz1986_usb_graphic.dll"
$InstallerDir = Join-Path $ProjectRoot "dist\installer\$Platform\$Configuration"
$InstallerDriverDir = Join-Path $InstallerDir "driver"
$DistDir = Join-Path $ProjectRoot "dist"
$ExeStagingDir = Join-Path $DistDir "build\bootstrapper\$Platform\$Configuration"
$BootstrapperSource = Join-Path $ExeStagingDir "InstallerBootstrapper.cs"
$BootstrapperManifest = Join-Path $ExeStagingDir "InstallerBootstrapper.manifest"
$BootstrapperPayloadZip = Join-Path $ExeStagingDir "driver_payload.zip"
$InstallerExe = Join-Path $DistDir "amoled_subscreen_idd_driver_installer_${Platform}_${Configuration}.exe"
$RawZip = Join-Path $DistDir "amoled_subscreen_idd_driver_raw_${Platform}_${Configuration}.zip"
$InstallerZip = Join-Path $DistDir "amoled_subscreen_idd_driver_installer_${Platform}_${Configuration}.zip"
$CertSubject = "CN=AMOLED Subscreen IDD Test Certificate"
$DefaultUdispConfig = "esp32s3udisp0_R368x448_Ejpg8_Fps30_Bl200"

function Find-MsBuild {
    if ($env:MSBUILD_EXE -and (Test-Path $env:MSBUILD_EXE)) {
        return $env:MSBUILD_EXE
    }

    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )

    foreach ($path in $candidates) {
        if ($path -and (Test-Path $path)) {
            return $path
        }
    }

    throw "MSBuild.exe was not found. Open a Developer PowerShell or set MSBUILD_EXE."
}

function Find-WdkTool([string]$Name, [string]$Arch) {
    $binRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (!(Test-Path $binRoot)) {
        throw "Windows Kits bin directory was not found: $binRoot"
    }

    $tools = Get-ChildItem $binRoot -Recurse -Filter $Name -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\$Arch\\$Name$" } |
        Sort-Object FullName -Descending

    if (!$tools) {
        $tools = Get-ChildItem $binRoot -Recurse -Filter $Name -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending
    }

    if (!$tools) {
        throw "$Name was not found under $binRoot"
    }

    return $tools[0].FullName
}

function Find-CSharpCompiler {
    if ($env:CSC_EXE -and (Test-Path $env:CSC_EXE)) {
        return $env:CSC_EXE
    }

    $cmd = Get-Command csc.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\Roslyn\csc.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe",
        "$env:windir\Microsoft.NET\Framework64\v4.0.30319\csc.exe",
        "$env:windir\Microsoft.NET\Framework\v4.0.30319\csc.exe"
    )

    foreach ($path in $candidates) {
        if ($path -and (Test-Path $path)) {
            return $path
        }
    }

    throw "csc.exe was not found. Install Visual Studio/.NET Framework build tools or set CSC_EXE."
}

function Find-DotNetReferenceAssembly([string]$Name) {
    $referenceRoot = Join-Path ${env:ProgramFiles(x86)} "Reference Assemblies\Microsoft\Framework\.NETFramework"
    $versions = @("v4.8", "v4.7.2", "v4.6.1", "v4.6", "v4.5.2", "v4.5.1", "v4.5")

    foreach ($version in $versions) {
        $path = Join-Path (Join-Path $referenceRoot $version) $Name
        if (Test-Path $path) {
            return $path
        }
    }

    return $Name
}

function Get-OrCreate-TestCertificate {
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

    if (!$cert) {
        $cert = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject $CertSubject `
            -CertStoreLocation Cert:\CurrentUser\My `
            -KeyAlgorithm RSA `
            -KeyLength 2048 `
            -HashAlgorithm SHA256 `
            -KeyUsage DigitalSignature `
            -NotAfter (Get-Date).AddYears(5)
    }

    return $cert
}

function Wait-FileWritable([string]$Path, [int]$TimeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Path) {
            try {
                $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
                $stream.Close()
                return
            } catch {
                Start-Sleep -Milliseconds 500
            }
        } else {
            Start-Sleep -Milliseconds 500
        }
    }

    throw "Timed out waiting for file to become writable: $Path"
}

function Trust-TestCertificate($cert) {
    $certPath = Join-Path $PackageDir "amoled_subscreen_idd_test.cer"
    Export-Certificate -Cert $cert -FilePath $certPath | Out-Null

    Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\CurrentUser\Root | Out-Null
    Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\CurrentUser\TrustedPublisher | Out-Null

    if ($TrustLocalMachine) {
        Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    }

    return $certPath
}

if (!(Test-Path $ProjectFile)) {
    throw "Project file was not found: $ProjectFile"
}

if (!(Test-Path $InfFile)) {
    throw "INF file was not found: $InfFile"
}

if (!$SkipBuild) {
    $msbuild = Find-MsBuild
    & $msbuild $ProjectFile /p:Configuration=$Configuration /p:Platform=$Platform /m:1 /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE"
    }
}

if (!(Test-Path $OutputDll)) {
    throw "Driver DLL was not found: $OutputDll"
}

if (Test-Path $PackageDir) {
    Remove-Item -LiteralPath $PackageDir -Recurse -Force
}

New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null
Copy-Item -LiteralPath $InfFile -Destination $PackageInf -Force
Copy-Item -LiteralPath $OutputDll -Destination $PackageDll -Force

$inf2cat = Find-WdkTool "Inf2Cat.exe" "x86"
$packageDirForTool = (Resolve-Path -LiteralPath $PackageDir).ProviderPath
& $inf2cat /driver:$packageDirForTool /os:10_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE"
}

if (!$SkipSign) {
    if (!(Test-Path $PackageCat)) {
        throw "Catalog file was not generated: $PackageCat"
    }

    $signtool = Find-WdkTool "signtool.exe" $Platform
    $cert = Get-OrCreate-TestCertificate
    $certPath = Trust-TestCertificate $cert

    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My $PackageCat
    if ($LASTEXITCODE -ne 0) {
        throw "signtool sign failed with exit code $LASTEXITCODE"
    }

    & $signtool verify /v /pa $PackageCat
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "signtool verify returned exit code $LASTEXITCODE. The package may still require test mode or LocalMachine trust for installation."
    }

    Write-Host "Test certificate exported to: $certPath"
}

if (Test-Path $InstallerDir) {
    Remove-Item -LiteralPath $InstallerDir -Recurse -Force
}

New-Item -ItemType Directory -Path $InstallerDriverDir -Force | Out-Null
Get-ChildItem -LiteralPath $PackageDir -Force |
    Copy-Item -Destination $InstallerDriverDir -Recurse -Force

$installScript = @'
param(
    [switch]$ForceInstall
)

$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-TestSigningState {
    $output = & bcdedit /enum 2>$null
    if ($LASTEXITCODE -ne 0) {
        return "Unknown"
    }

    if ($output -match "(?im)^\s*testsigning\s+Yes\s*$") {
        return "On"
    }

    return "Off"
}

function Get-PresentPnpDevices {
    try {
        return @(Get-PnpDevice -PresentOnly -ErrorAction Stop)
    } catch {
        Write-Warning "Could not enumerate present PnP devices: $($_.Exception.Message)"
        return @()
    }
}

function Write-DeviceBindingHint {
    $devices = Get-PresentPnpDevices
    if ($devices.Count -eq 0) {
        return
    }

    $targetDevices = @($devices | Where-Object { $_.InstanceId -like "USB\VID_303A&PID_2987*" })
    $espUsbDevices = @($devices | Where-Object {
        $_.InstanceId -like "USB\VID_303A&PID_1001*" -or
        $_.InstanceId -like "USB\VID_303A&PID_4001*"
    })

    if ($targetDevices.Count -gt 0) {
        Write-Host "Target USB device is currently present:"
        foreach ($device in $targetDevices) {
            Write-Host "  $($device.Status)  $($device.FriendlyName)  $($device.InstanceId)"
        }
        return
    }

    Write-Warning "Driver package was added, but no currently connected USB\VID_303A&PID_2987 device was found."
    Write-Warning "Windows will not bind this display driver until the hardware enumerates with that target VID/PID."

    if ($espUsbDevices.Count -gt 0) {
        Write-Warning "Detected ESP32 USB serial/JTAG style device IDs instead:"
        foreach ($device in $espUsbDevices) {
            Write-Warning "  $($device.Status)  $($device.FriendlyName)  $($device.InstanceId)"
        }
        Write-Warning "Do not bind the IDD display driver to PID_1001 or PID_4001 unless firmware intentionally exposes that as the display interface."
    }
}

if (!(Test-IsAdministrator)) {
    throw "Please run this script from an elevated PowerShell."
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$driverDir = Join-Path $root "driver"
if (!(Test-Path $driverDir)) {
    $driverDir = $root
}
$inf = Join-Path $driverDir "amoled_subscreen_idd.inf"
$cert = Join-Path $driverDir "amoled_subscreen_idd_test.cer"

if (!(Test-Path $inf)) {
    throw "Driver INF was not found: $inf"
}

$defaultUdispConfig = "__DEFAULT_UDISP_CONFIG__"
$driverParameterKey = "HKLM:\SYSTEM\CurrentControlSet\Services\xfz1986_usb_graphic\Parameters"

function Set-DefaultDriverRuntimeConfig {
    New-Item -Path $driverParameterKey -Force | Out-Null
    New-ItemProperty -Path $driverParameterKey -Name "udisp" -PropertyType String -Value $defaultUdispConfig -Force | Out-Null
    Write-Host "Default driver runtime config written:"
    Write-Host "  $driverParameterKey"
    Write-Host "  udisp=$defaultUdispConfig"
}

if (Test-Path $cert) {
    Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Host "Imported test certificate into LocalMachine trust stores."
} else {
    Write-Warning "Test certificate was not found: $cert"
}

$testSigning = Get-TestSigningState
if ($testSigning -eq "Off" -and !$ForceInstall) {
    Write-Warning "Windows test signing mode is not enabled."
    Write-Warning "If installation fails because of test signing, run as administrator:"
    Write-Warning "  bcdedit /set testsigning on"
    Write-Warning "Then reboot Windows and run this installer again."
}

pnputil /add-driver "$inf" /install
if ($LASTEXITCODE -eq 259) {
    Write-Warning "pnputil returned 259. The driver package may already be installed or no connected device needed an update."
} elseif ($LASTEXITCODE -ne 0) {
    throw "pnputil failed with exit code $LASTEXITCODE"
}

Write-Host "Driver installation command completed."
Set-DefaultDriverRuntimeConfig
Write-DeviceBindingHint
Write-Host "If the device was already connected, unplug and replug it so the driver reloads the runtime config."
'@

$uninstallScript = @'
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (!(Test-IsAdministrator)) {
    throw "Please run this script from an elevated PowerShell."
}

$driverParameterKey = "HKLM:\SYSTEM\CurrentControlSet\Services\xfz1986_usb_graphic\Parameters"

$drivers = pnputil /enum-drivers
$matchedPackages = @()
$current = @()

foreach ($line in $drivers) {
    if ($line -match "^\s*$") {
        if (($current -join "`n") -match "amoled_subscreen_idd\.inf|xfz1986_usb_graphic|amoled_subscreen development|AMOLED Subscreen USB Display") {
            $matchedPackages += ,($current -join "`n")
        }
        $current = @()
    } else {
        $current += $line
    }
}

if ($current.Count -gt 0 -and (($current -join "`n") -match "amoled_subscreen_idd\.inf|xfz1986_usb_graphic|amoled_subscreen development|AMOLED Subscreen USB Display")) {
    $matchedPackages += ,($current -join "`n")
}

if ($matchedPackages.Count -eq 0) {
    Write-Host "No matching installed driver package was found."
    exit 0
}

foreach ($packageBlock in $matchedPackages) {
    $publishedName = $null
    foreach ($line in ($packageBlock -split "`n")) {
        if ($line -match "Published Name\s*:\s*(\S+)") {
            $publishedName = $Matches[1]
            break
        }
        if ($line -match "\b(oem\d+\.inf)\b") {
            $publishedName = $Matches[1]
            break
        }
    }

    if (!$publishedName) {
        Write-Warning "Could not parse Published Name from:"
        Write-Warning $packageBlock
        continue
    }

    if (!$Force) {
        Write-Host "Found driver package: $publishedName"
        Write-Host "Run with -Force to delete it:"
        Write-Host "  .\uninstall-driver.ps1 -Force"
        continue
    }

    pnputil /delete-driver $publishedName /uninstall /force
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil failed to delete $publishedName with exit code $LASTEXITCODE"
    }
}

if ($Force -and (Test-Path $driverParameterKey)) {
    Remove-ItemProperty -Path $driverParameterKey -Name "udisp" -ErrorAction SilentlyContinue
    Write-Host "Removed driver runtime config:"
    Write-Host "  $driverParameterKey\udisp"
}
'@

$readmeText = @"
AMOLED Subscreen IDD Driver Installer Package

Install:
  1. Open PowerShell as Administrator.
  2. Run:
       powershell -ExecutionPolicy Bypass -File .\install-driver.ps1
  3. The installer writes the default runtime config:
       esp32s3udisp0_R368x448_Ejpg8_Fps30_Bl200
  4. If Windows rejects the test-signed driver, run:
       bcdedit /set testsigning on
     Then reboot and run install-driver.ps1 again.

Uninstall:
  1. Open PowerShell as Administrator.
  2. Preview matching package:
       powershell -ExecutionPolicy Bypass -File .\uninstall-driver.ps1
  3. Delete matching package:
       powershell -ExecutionPolicy Bypass -File .\uninstall-driver.ps1 -Force

Diagnostics:
  Run this command from the extracted installer directory:
       powershell -ExecutionPolicy Bypass -File .\diagnose-driver.ps1

Raw driver files are under:
  .\driver

If this package is delivered as an EXE, double-click it and accept the UAC prompt.
"@

$installScript = $installScript.Replace("__DEFAULT_UDISP_CONFIG__", $DefaultUdispConfig)

Set-Content -LiteralPath (Join-Path $InstallerDir "install-driver.ps1") -Value $installScript -Encoding UTF8
Set-Content -LiteralPath (Join-Path $InstallerDir "uninstall-driver.ps1") -Value $uninstallScript -Encoding UTF8
Set-Content -LiteralPath (Join-Path $InstallerDir "README.txt") -Value $readmeText -Encoding UTF8
if (Test-Path $DiagnosticScriptFile) {
    Copy-Item -LiteralPath $DiagnosticScriptFile -Destination (Join-Path $InstallerDir "diagnose-driver.ps1") -Force
}

$installCmd = @'
@echo off
net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Please right-click this installer and choose "Run as administrator".
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-driver.ps1"
if not "%errorlevel%"=="0" (
    echo.
    echo Driver installation failed.
    pause
    exit /b %errorlevel%
)

echo.
echo Driver installation command completed.
pause
'@

Set-Content -LiteralPath (Join-Path $InstallerDir "install-driver.cmd") -Value $installCmd -Encoding ASCII

if (Test-Path $ExeStagingDir) {
    Remove-Item -LiteralPath $ExeStagingDir -Recurse -Force
}

New-Item -ItemType Directory -Path $ExeStagingDir -Force | Out-Null
Compress-Archive -Path (Join-Path $InstallerDir "*") -DestinationPath $BootstrapperPayloadZip -Force

$bootstrapperManifestText = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="1.0.0.0" processorArchitecture="amd64" name="AmoledSubscreen.Idd.DriverInstaller" type="win32" />
  <description>AMOLED Subscreen IDD Driver Installer</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="requireAdministrator" uiAccess="false" />
      </requestedPrivileges>
    </security>
  </trustInfo>
</assembly>
'@

$bootstrapperSourceText = @'
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Windows.Forms;

internal static class InstallerBootstrapper
{
    private const string Title = "AMOLED Subscreen IDD Driver Installer";
    private const string PayloadResourceName = "driver_payload.zip";

    [STAThread]
    private static int Main()
    {
        Console.Title = Title;
        int exitCode = 1;

        try
        {
            exitCode = Run();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine();
            Console.Error.WriteLine("Fatal error:");
            Console.Error.WriteLine(ex.ToString());
            MessageBox.Show(ex.Message, Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
            exitCode = 1;
        }

        Console.WriteLine();
        Console.WriteLine(exitCode == 0 ? "Install command completed." : "Install command failed.");
        Console.WriteLine("Press any key to close this window.");
        try
        {
            Console.ReadKey(true);
        }
        catch
        {
        }

        return exitCode;
    }

    private static int Run()
    {
        if (!IsAdministrator())
        {
            throw new InvalidOperationException("This installer must run as administrator. Please accept the UAC prompt and run it again.");
        }

        Console.WriteLine(Title);
        Console.WriteLine("This installer will extract the driver package and run pnputil.");
        Console.WriteLine();

        string extractDir = Path.Combine(Path.GetTempPath(), "amoled_subscreen_idd_installer_" + Process.GetCurrentProcess().Id.ToString());
        if (Directory.Exists(extractDir))
        {
            Directory.Delete(extractDir, true);
        }
        Directory.CreateDirectory(extractDir);

        string payloadZip = Path.Combine(extractDir, "driver_payload.zip");
        ExtractPayload(payloadZip);
        ZipFile.ExtractToDirectory(payloadZip, extractDir);

        string installScript = Path.Combine(extractDir, "install-driver.ps1");
        if (!File.Exists(installScript))
        {
            throw new FileNotFoundException("Install script was not found after extraction.", installScript);
        }

        string logPath = Path.Combine(extractDir, "install.log");
        Console.WriteLine("Extracted package:");
        Console.WriteLine("  " + extractDir);
        Console.WriteLine("Log file:");
        Console.WriteLine("  " + logPath);
        Console.WriteLine();

        int code = RunPowerShellInstall(installScript, extractDir, logPath);
        if (code == 0)
        {
            MessageBox.Show("Driver installation command completed. If the device is already connected, unplug and replug it if Windows does not bind the driver immediately.", Title, MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        else
        {
            MessageBox.Show("Driver installation failed. See the console window and log file:\r\n" + logPath, Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
        }

        return code;
    }

    private static bool IsAdministrator()
    {
        WindowsIdentity identity = WindowsIdentity.GetCurrent();
        WindowsPrincipal principal = new WindowsPrincipal(identity);
        return principal.IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static void ExtractPayload(string payloadZip)
    {
        Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(PayloadResourceName);
        if (stream == null)
        {
            throw new InvalidOperationException("Embedded driver payload was not found.");
        }

        using (stream)
        using (FileStream output = File.Create(payloadZip))
        {
            stream.CopyTo(output);
        }
    }

    private static int RunPowerShellInstall(string installScript, string workingDirectory, string logPath)
    {
        object sync = new object();
        using (StreamWriter log = new StreamWriter(logPath, false, Encoding.UTF8))
        using (Process process = new Process())
        {
            process.StartInfo.FileName = "powershell.exe";
            process.StartInfo.Arguments = "-NoProfile -ExecutionPolicy Bypass -File " + Quote(installScript);
            process.StartInfo.WorkingDirectory = workingDirectory;
            process.StartInfo.UseShellExecute = false;
            process.StartInfo.RedirectStandardOutput = true;
            process.StartInfo.RedirectStandardError = true;
            process.StartInfo.CreateNoWindow = false;

            DataReceivedEventHandler outputHandler = delegate(object sender, DataReceivedEventArgs args)
            {
                WriteLine(sync, log, args.Data, false);
            };
            DataReceivedEventHandler errorHandler = delegate(object sender, DataReceivedEventArgs args)
            {
                WriteLine(sync, log, args.Data, true);
            };

            process.OutputDataReceived += outputHandler;
            process.ErrorDataReceived += errorHandler;

            Console.WriteLine("Running:");
            Console.WriteLine("  powershell.exe " + process.StartInfo.Arguments);
            Console.WriteLine();

            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();

            return process.ExitCode;
        }
    }

    private static void WriteLine(object sync, StreamWriter log, string line, bool isError)
    {
        if (line == null)
        {
            return;
        }

        lock (sync)
        {
            if (isError)
            {
                Console.Error.WriteLine(line);
            }
            else
            {
                Console.WriteLine(line);
            }

            log.WriteLine(line);
            log.Flush();
        }
    }

    private static string Quote(string value)
    {
        return "\"" + value.Replace("\"", "\\\"") + "\"";
    }
}
'@

Set-Content -LiteralPath $BootstrapperManifest -Value $bootstrapperManifestText -Encoding UTF8
Set-Content -LiteralPath $BootstrapperSource -Value $bootstrapperSourceText -Encoding UTF8

if (Test-Path $InstallerExe) {
    Remove-Item -LiteralPath $InstallerExe -Force
}

$csc = Find-CSharpCompiler
$compressionRef = Find-DotNetReferenceAssembly "System.IO.Compression.dll"
$compressionFileSystemRef = Find-DotNetReferenceAssembly "System.IO.Compression.FileSystem.dll"
$formsRef = Find-DotNetReferenceAssembly "System.Windows.Forms.dll"

$cscArgs = @(
    "/nologo",
    "/target:exe",
    "/platform:$Platform",
    "/out:$InstallerExe",
    "/win32manifest:$BootstrapperManifest",
    "/resource:$BootstrapperPayloadZip,driver_payload.zip",
    "/reference:$compressionRef",
    "/reference:$compressionFileSystemRef",
    "/reference:$formsRef",
    $BootstrapperSource
)

& $csc @cscArgs
if ($LASTEXITCODE -ne 0) {
    throw "csc.exe failed to build installer EXE with exit code $LASTEXITCODE"
}

for ($i = 0; $i -lt 30 -and !(Test-Path $InstallerExe); $i++) {
    Start-Sleep -Milliseconds 500
}

if (!(Test-Path $InstallerExe)) {
    throw "C# bootstrapper did not generate the expected EXE: $InstallerExe"
}

Wait-FileWritable $InstallerExe 30

if (!$SkipSign) {
    $signtool = Find-WdkTool "signtool.exe" $Platform
    $cert = Get-OrCreate-TestCertificate

    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My $InstallerExe
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed to sign installer EXE with exit code $LASTEXITCODE"
    }
}

foreach ($zip in @($RawZip, $InstallerZip)) {
    if (Test-Path $zip) {
        Remove-Item -LiteralPath $zip -Force
    }
}

Compress-Archive -Path (Join-Path $PackageDir "*") -DestinationPath $RawZip -Force
Compress-Archive -Path (Join-Path $InstallerDir "*") -DestinationPath $InstallerZip -Force

Write-Host ""
Write-Host "Raw driver package generated:"
Write-Host "  $PackageDir"
Write-Host "  $RawZip"
Write-Host ""
Write-Host "Installer package generated:"
Write-Host "  $InstallerDir"
Write-Host "  $InstallerZip"
Write-Host "  $InstallerExe"
Write-Host ""
Write-Host "Install from an elevated PowerShell:"
Write-Host "  powershell -ExecutionPolicy Bypass -File `"$InstallerDir\install-driver.ps1`""
Write-Host ""
Write-Host "Or double-click the EXE installer and accept the UAC prompt:"
Write-Host "  $InstallerExe"
Write-Host ""
Write-Host "Or install the raw driver package manually:"
Write-Host "  pnputil /add-driver `"$PackageInf`" /install"
