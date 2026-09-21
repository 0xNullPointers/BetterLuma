<#
.SYNOPSIS
    Installs BetterLuma into the local Steam installation directory.
.DESCRIPTION
    Automates downloading the latest BetterLuma release, detecting the Steam
    installation directory, backing up existing conflicting DLLs, opensteamtool.dll,
    LumaCore.dll, LumaCorePayload.dll, and LumaCorePayload32.dll to .bak, deploying
    BetterLuma.toml, and generating a dynamic uninstaller script.
.PARAMETER SteamPath
    Optional custom path to the Steam installation folder. If omitted, the script
    automatically detects Steam exclusively from the Windows Registry.
.PARAMETER CloseSteam
    If specified, automatically closes any running steam.exe process without prompting.
.PARAMETER BuildType
    Optional build version to install: 'Release' or 'Debug'.
.PARAMETER DownloadUrl
    Optional direct URL to the BetterLuma zip package. If omitted, the script
    automatically queries GitHub for the latest release.
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$SteamPath,

    [Parameter()]
    [switch]$CloseSteam,

    [Parameter(ValueFromPipeline = $true)]
    [ValidateSet('Release', 'Debug', 'r', 'd', 'R', 'D', '')]
    [string]$BuildType = '',

    [Parameter()]
    [string]$DownloadUrl
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Ensure TLS 1.2 is enabled and optimize connection pooling for high-speed downloads
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12 -bor [System.Net.SecurityProtocolType]::Tls11 -bor [System.Net.SecurityProtocolType]::Tls
[System.Net.ServicePointManager]::DefaultConnectionLimit = 64
$ProgressPreference = 'SilentlyContinue'

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "          BetterLuma Automated Installer           " -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Detect Steam Installation Directory
function Find-SteamDirectory {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        $normalized = [System.IO.Path]::GetFullPath($ExplicitPath.TrimEnd('\', '/'))
        if (Test-Path (Join-Path $normalized 'steam.exe')) {
            return $normalized
        }
        Write-Warning "Specified path '$ExplicitPath' does not contain steam.exe."
    }

    # Query Steam installation directory ONLY from Windows Registry
    # 1. Current User Registry (HKCU:\Software\Valve\Steam)
    try {
        $hkcu = Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue
        if ($hkcu.SteamPath) {
            $regPath = [System.IO.Path]::GetFullPath($hkcu.SteamPath.Replace('/', '\'))
            if (Test-Path (Join-Path $regPath 'steam.exe')) {
                return $regPath
            }
        }
        if ($hkcu.SteamExe) {
            $regExe = [System.IO.Path]::GetFullPath($hkcu.SteamExe.Replace('/', '\'))
            $regPath = Split-Path -Parent $regExe
            if (Test-Path (Join-Path $regPath 'steam.exe')) {
                return $regPath
            }
        }
    } catch {}

    # 2. Local Machine Registry (HKLM 64-bit and 32-bit)
    $hklmKeys = @(
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam'
    )
    foreach ($key in $hklmKeys) {
        try {
            $hklm = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
            if ($hklm.InstallPath) {
                $regPath = [System.IO.Path]::GetFullPath($hklm.InstallPath.Replace('/', '\'))
                if (Test-Path (Join-Path $regPath 'steam.exe')) {
                    return $regPath
                }
            }
        } catch {}
    }

    return $null
}

$detectedSteam = Find-SteamDirectory -ExplicitPath $SteamPath
if (-not $detectedSteam) {
    Write-Error "Could not detect the Steam installation directory. Please ensure Steam is installed or specify -SteamPath '<Path-To-Steam>'."
    return
}

Write-Host "[+] Detected Steam Directory: " -NoNewline
Write-Host "$detectedSteam" -ForegroundColor Green

# Step 2: Handle Running Steam Process
$allSteam = @(Get-Process -Name "steam" -ErrorAction SilentlyContinue)
$steamProcesses = @($allSteam | Where-Object {
    try {
        if ($_.Path) {
            return (Split-Path -Parent $_.Path) -eq $detectedSteam
        }
    } catch {}
    return $true
})
if ($steamProcesses.Count -gt 0) {
    Write-Host ""
    Write-Warning "Steam is currently running (Process IDs: $(($steamProcesses.Id) -join ', '))."
    Write-Warning "Loaded DLL files will be locked while Steam is active."

    $shouldClose = $CloseSteam
    if (-not $shouldClose) {
        $response = Read-Host "Would you like to terminate Steam now to proceed with installation? (Y/N)"
        if ($response -match '^[Yy]') {
            $shouldClose = $true
        }
    }

    if ($shouldClose) {
        Write-Host "[*] Closing Steam processes..." -ForegroundColor Yellow
        $steamProcesses | Stop-Process -Force
        Start-Sleep -Seconds 2
        Write-Host "[+] Steam closed successfully." -ForegroundColor Green
    } else {
        Write-Error "Installation cancelled. Please exit Steam completely before running the installer."
        return
    }
}

# Step 3: Select Build Version & Download BetterLuma
function Get-LatestBetterLumaUrl {
    param(
        [string]$Repo = "0xNullPointers/BetterLuma",
        [string]$Build = "Release"
    )

    $assetPattern = "*$Build*.zip"
    $fallbackUrl  = "https://github.com/$Repo/releases/latest/download/$Build.zip"

    # 1. Query GitHub API for the latest release asset
    try {
        $headers = @{
            'User-Agent' = 'BetterLuma-Installer'
            'Accept'     = 'application/vnd.github.v3+json'
        }
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers $headers -TimeoutSec 10 -ErrorAction Stop
        if ($release -and $release.assets) {
            $asset = $release.assets | Where-Object { $_.name -like $assetPattern } | Select-Object -First 1
            if ($asset -and $asset.browser_download_url) {
                Write-Host "  [+] Latest release ($($release.tag_name)): $($asset.name)" -ForegroundColor Green
                return $asset.browser_download_url
            }
        }
    } catch {
        Write-Warning "Could not query GitHub API ($($_.Exception.Message)). Falling back to latest redirect endpoint."
    }

    # 2. Fallback to GitHub's direct latest download URL
    Write-Host "  [*] Using latest release: $fallbackUrl" -ForegroundColor Cyan
    return $fallbackUrl
}

if (-not $DownloadUrl) {
    if ([string]::IsNullOrWhiteSpace($BuildType)) {
        Write-Host ""
        Write-Host "Select BetterLuma build version to install:" -ForegroundColor Cyan
        Write-Host "  [R] Release (Recommended)" -ForegroundColor Green
        Write-Host "  [D] Debug" -ForegroundColor Yellow
        Write-Host ""
        $userChoice = Read-Host "Choose build [R/d] (Default is Release)"
        if ([string]::IsNullOrWhiteSpace($userChoice) -or $userChoice -match '^[Rr]') {
            $BuildType = "Release"
        } elseif ($userChoice -match '^[Dd]') {
            $BuildType = "Debug"
        } else {
            Write-Host "[*] Unrecognized option '$userChoice'. Defaulting to Release." -ForegroundColor Yellow
            $BuildType = "Release"
        }
    } else {
        if ($BuildType -match '^[Dd]') {
            $BuildType = "Debug"
        } else {
            $BuildType = "Release"
        }
    }

    Write-Host ""
    Write-Host "[+] Selected Build: " -NoNewline
    Write-Host "$BuildType" -ForegroundColor Green

    Write-Host "[*] Resolving latest BetterLuma $BuildType from GitHub..." -ForegroundColor Cyan
    $DownloadUrl = Get-LatestBetterLumaUrl -Build $BuildType
}

$tempRoot = Join-Path $env:TEMP ("BetterLuma_Setup_" + [System.Guid]::NewGuid().ToString("N"))
$tempZip  = Join-Path $tempRoot "BetterLuma.zip"
$tempExt  = Join-Path $tempRoot "Extracted"

New-Item -ItemType Directory -Path $tempExt -Force | Out-Null

# Define multi-connection segmented fast downloader
if (-not ([System.Management.Automation.PSTypeName]'BetterLumaFastDownloader').Type) {
    $downloaderSource = @"
using System;
using System.IO;
using System.Net;
using System.Threading;
using System.Threading.Tasks;

public class BetterLumaFastDownloader
{
    static BetterLumaFastDownloader()
    {
        ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12 | SecurityProtocolType.Tls11 | SecurityProtocolType.Tls;
        ServicePointManager.DefaultConnectionLimit = 64;
    }

    public static void Download(string url, string outputPath, int threadCount = 8)
    {
        var headReq = (HttpWebRequest)WebRequest.Create(url);
        headReq.Method = "HEAD";
        headReq.AllowAutoRedirect = true;
        headReq.UserAgent = "Mozilla/5.0";
        headReq.Timeout = 15000;
        
        long totalBytes = 0;
        string finalUrl = url;
        bool supportsRange = false;

        using (var headResp = (HttpWebResponse)headReq.GetResponse())
        {
            finalUrl = headResp.ResponseUri.AbsoluteUri;
            totalBytes = headResp.ContentLength;
            supportsRange = "bytes".Equals(headResp.Headers["Accept-Ranges"], StringComparison.OrdinalIgnoreCase);
        }

        using (var fs = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite))
        {
            if (totalBytes > 0) fs.SetLength(totalBytes);
        }

        if (!supportsRange || totalBytes <= 0 || threadCount <= 1)
        {
            using (var wc = new WebClient())
            {
                wc.DownloadFile(finalUrl, outputPath);
            }
            return;
        }

        long totalDownloaded = 0;
        var startTime = DateTime.UtcNow;
        var timer = new Timer(_ =>
        {
            long current = Interlocked.Read(ref totalDownloaded);
            double elapsed = (DateTime.UtcNow - startTime).TotalSeconds;
            if (elapsed <= 0) return;
            double speed = (current / 1024.0 / 1024.0) / elapsed;
            int pct = totalBytes > 0 ? (int)((current * 100) / totalBytes) : 0;
            int barWidth = 28;
            int filled = Math.Min(barWidth, (pct * barWidth) / 100);
            string bar = new string('=', filled) + (filled < barWidth ? ">" : "") + new string(' ', Math.Max(0, barWidth - filled - 1));
            Console.Write("\r  [{0}] {1,3}% ({2:0.0}/{3:0.0} MB) at {4:0.00} MB/s   ",
                bar, pct, current / 1024.0 / 1024.0, totalBytes / 1024.0 / 1024.0, speed);
        }, null, 200, 200);

        try
        {
            long chunkSize = (long)Math.Ceiling((double)totalBytes / threadCount);
            Parallel.For(0, threadCount, new ParallelOptions { MaxDegreeOfParallelism = threadCount }, i =>
            {
                long start = i * chunkSize;
                long end = Math.Min(start + chunkSize - 1, totalBytes - 1);
                if (start > end) return;

                var req = (HttpWebRequest)WebRequest.Create(finalUrl);
                req.AddRange(start, end);
                req.Timeout = 30000;
                req.ReadWriteTimeout = 30000;
                req.UserAgent = "Mozilla/5.0";

                using (var resp = req.GetResponse())
                using (var stream = resp.GetResponseStream())
                using (var fs = new FileStream(outputPath, FileMode.Open, FileAccess.Write, FileShare.ReadWrite))
                {
                    fs.Seek(start, SeekOrigin.Begin);
                    byte[] buffer = new byte[65536];
                    int bytesRead;
                    while ((bytesRead = stream.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        fs.Write(buffer, 0, bytesRead);
                        Interlocked.Add(ref totalDownloaded, bytesRead);
                    }
                }
            });
        }
        finally
        {
            timer.Dispose();
        }

        Console.WriteLine("\r  [============================] 100% ({0:0.0}/{0:0.0} MB) Download complete!            ", totalBytes / 1024.0 / 1024.0);
    }
}
"@
    Add-Type -TypeDefinition $downloaderSource -Language CSharp
}

try {
    Write-Host ""
    Write-Host "[*] Downloading BetterLuma from: " -ForegroundColor Cyan
    Write-Host "    $DownloadUrl"

    $downloadSuccess = $false

    if (Test-Path -LiteralPath $DownloadUrl) {
        Copy-Item -LiteralPath $DownloadUrl -Destination $tempZip -Force
        $downloadSuccess = (Test-Path -LiteralPath $tempZip) -and ((Get-Item -LiteralPath $tempZip).Length -gt 0)
    } else {
        try {
            [BetterLumaFastDownloader]::Download($DownloadUrl, $tempZip, 8)
            $downloadSuccess = (Test-Path -LiteralPath $tempZip) -and ((Get-Item -LiteralPath $tempZip).Length -gt 1024)
        } catch {
            Write-Warning "Fast multi-threaded downloader encountered an error: $_"
        }

        if (-not $downloadSuccess) {
            if (Get-Command 'curl.exe' -ErrorAction SilentlyContinue) {
                Write-Host "[*] Falling back to native curl.exe..." -ForegroundColor Cyan
                & curl.exe -L --fail --retry 3 --connect-timeout 10 -o $tempZip $DownloadUrl
            } else {
                Write-Host "[*] Falling back to WebClient..." -ForegroundColor Cyan
                $wc = New-Object System.Net.WebClient
                $wc.DownloadFile($DownloadUrl, $tempZip)
            }
        }
    }

    if (-not (Test-Path $tempZip) -or ((Get-Item $tempZip).Length -lt 1024)) {
        Write-Error "Failed to download a valid BetterLuma archive package."
        return
    }

    Write-Host "[+] Download verified ($((Get-Item $tempZip).Length) bytes)." -ForegroundColor Green
    Write-Host "[*] Extracting archive..."

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($tempZip, $tempExt)

    # Locate extracted DLLs (search top level and immediate subdirectories if any)
    $extractedDlls = @(Get-ChildItem -Path $tempExt -Filter "*.dll" -Recurse | Where-Object { -not $_.PSIsContainer })
    if ($extractedDlls.Count -eq 0) {
        Write-Error "No DLL files found inside the downloaded archive."
        return
    }

    Write-Host "[+] Extracted $($extractedDlls.Count) DLL file(s):" -ForegroundColor Green
    foreach ($d in $extractedDlls) {
        Write-Host "    - $($d.Name)" -ForegroundColor DarkGray
    }

    # Step 4: Scan for Conflicts & Back up (.bak)
    Write-Host ""
    Write-Host "[*] Scanning Steam directory for existing files and conflicts..." -ForegroundColor Cyan

    $backedUpFiles  = [System.Collections.Generic.List[psobject]]::new()
    $registeredBaks = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    # BetterLuma files to directly remove and replace
    $replaceWithoutBackup = @(
        'BetterLuma.dll',
        'BetterLumaPayload.dll',
        'BetterLumaPayload32.dll'
    )

    # Clean up existing BetterLuma files directly without creating .bak backups
    foreach ($blFile in $replaceWithoutBackup) {
        $blPath = Join-Path $detectedSteam $blFile
        if (Test-Path -LiteralPath $blPath) {
            Remove-Item -LiteralPath $blPath -Force
            Write-Host "  [-] Found existing $blFile -> Removed" -ForegroundColor DarkGray
        }
        $bakPath = Join-Path $detectedSteam "$blFile.bak"
        if (Test-Path -LiteralPath $bakPath) {
            Remove-Item -LiteralPath $bakPath -Force
            Write-Host "  [-] Found obsolete $blFile.bak -> Removed" -ForegroundColor DarkGray
        }
    }

    # Explicit priority files that must be backed up to .bak if present
    $priorityFiles = @(
        'opensteamtool.dll',
        'LumaCore.dll',
        'LumaCorePayload.dll',
        'LumaCorePayload32.dll'
    )

    # Helper: Detect if a target proxy DLL in Steam is already a BetterLuma proxy
    function Test-IsBetterLumaProxy {
        param([string]$FilePath)
        if (-not (Test-Path -LiteralPath $FilePath)) { return $false }
        try {
            return [bool](Select-String -Path $FilePath -Pattern "BetterLuma\.dll" -Quiet -Encoding ascii)
        } catch {
            return $false
        }
    }

    # Collect all targets to scan: priority targets + all extracted incoming DLLs
    $targetsToScan = [System.Collections.Generic.List[string]]::new()
    foreach ($f in $priorityFiles) {
        if (-not $targetsToScan.Contains($f)) {
            $targetsToScan.Add($f)
        }
    }
    foreach ($dll in $extractedDlls) {
        if (-not $replaceWithoutBackup.Contains($dll.Name) -and -not $targetsToScan.Contains($dll.Name)) {
            $targetsToScan.Add($dll.Name)
        }
    }

    foreach ($fileName in $targetsToScan) {
        $targetFile = Join-Path $detectedSteam $fileName
        $bakName    = "$fileName.bak"
        $bakPath    = Join-Path $detectedSteam $bakName

        if (Test-Path -LiteralPath $targetFile) {
            # Check if this file is our own proxy from an earlier installation (priority backup files are never proxies)
            $isOwnProxy = (-not $priorityFiles.Contains($fileName)) -and (Test-IsBetterLumaProxy -FilePath $targetFile)

            if ($isOwnProxy) {
                # This is BetterLuma's proxy. Do not back it up as a conflicting 3rd-party file.
                Write-Host "  [*] Existing BetterLuma $fileName detected, replacing with updated version" -ForegroundColor DarkGray
                Remove-Item -LiteralPath $targetFile -Force
            } elseif (-not (Test-Path -LiteralPath $bakPath)) {
                # Genuine 3rd-party/conflicting file: safely back up to .bak
                Move-Item -LiteralPath $targetFile -Destination $bakPath -Force
                Write-Host "  [!] Found $fileName -> Backed up to $bakName" -ForegroundColor Yellow
            } else {
                # Preserve the original backup if already existing, replace active file
                Write-Host "  [*] $bakName already exists, replacing $fileName" -ForegroundColor DarkGray
                Remove-Item -LiteralPath $targetFile -Force
            }

            # Register genuine .bak file for restoration if present
            if (Test-Path -LiteralPath $bakPath) {
                if ($registeredBaks.Add($bakName)) {
                    $backedUpFiles.Add([PSCustomObject]@{
                        Original = $fileName
                        Backup   = $bakName
                    })
                }
            }
        } elseif (Test-Path -LiteralPath $bakPath) {
            # Active file is not present, but its .bak already exists from previous installation
            if ($registeredBaks.Add($bakName)) {
                $backedUpFiles.Add([PSCustomObject]@{
                    Original = $fileName
                    Backup   = $bakName
                })
            }
        }
    }

    # Step 5: Install DLLs into Steam Directory
    Write-Host ""
    Write-Host "[*] Deploying DLLs into Steam root folder..." -ForegroundColor Cyan
    $installedDllNames = [System.Collections.Generic.List[string]]::new()

    foreach ($dll in $extractedDlls) {
        $dest = Join-Path $detectedSteam $dll.Name
        Copy-Item -Path $dll.FullName -Destination $dest -Force
        $installedDllNames.Add($dll.Name)
        Write-Host "  [+] Installed: $($dll.Name)" -ForegroundColor Green
    }

    # Step 6: Create or Update BetterLuma.toml Configuration
    Write-Host ""
    Write-Host "[*] Configuring BetterLuma.toml..." -ForegroundColor Cyan

    function Parse-TomlContent([string]$content) {
        $lines = $content -split '\r?\n'
        $currentSection = ""
        $inMultiline = $false
        $multilineKey = ""
        $multilineBuffer = [System.Collections.Generic.List[string]]::new()
        $config = [System.Collections.Generic.Dictionary[string, System.Collections.Generic.Dictionary[string, string]]]::new([System.StringComparer]::OrdinalIgnoreCase)

        foreach ($line in $lines) {
            $trimmed = $line.Trim()
            if (-not $inMultiline) {
                if ($trimmed.StartsWith("#") -or [string]::IsNullOrWhiteSpace($trimmed)) { continue }
                if ($trimmed -match '^\[([a-zA-Z0-9_\.\-]+)\]$') {
                    $currentSection = $matches[1]
                    if (-not $config.ContainsKey($currentSection)) {
                        $config[$currentSection] = [System.Collections.Generic.Dictionary[string, string]]::new([System.StringComparer]::OrdinalIgnoreCase)
                    }
                    continue
                }
                if ($trimmed -match '^([a-zA-Z0-9_\-]+)\s*=\s*(.*)$') {
                    $key = $matches[1]
                    $val = $matches[2].Trim()
                    if ($val.StartsWith("[") -and -not (($val -replace '#.*$', '').Trim().EndsWith("]"))) {
                        $inMultiline = $true
                        $multilineKey = $key
                        $multilineBuffer.Clear()
                        $firstVal = ($line -replace "^[^=]+=\s*", "")
                        $multilineBuffer.Add($firstVal)
                    } else {
                        if ($currentSection) {
                            $config[$currentSection][$key] = $val
                        }
                    }
                }
            } else {
                $multilineBuffer.Add($line)
                $cleanLine = ($line -replace '#.*$', '').Trim()
                if ($cleanLine.EndsWith("]")) {
                    $inMultiline = $false
                    if ($currentSection) {
                        $config[$currentSection][$multilineKey] = ($multilineBuffer -join "`r`n")
                    }
                }
            }
        }
        return $config
    }

    function Get-BetterLumaTomlTemplate([string]$ExtractedDir) {
        # 1. Check if extracted archive contains BetterLuma.toml.example
        if ($ExtractedDir -and (Test-Path -LiteralPath $ExtractedDir)) {
            $localExample = Get-ChildItem -Path $ExtractedDir -Filter "BetterLuma.toml.example" -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($localExample) {
                Write-Host "  [+] Found BetterLuma.toml.example in downloaded archive." -ForegroundColor DarkGray
                return (Get-Content -LiteralPath $localExample.FullName -Raw)
            }
        }

        # 2. Download directly from GitHub
        $rawUrls = @(
            "https://raw.githubusercontent.com/0xNullPointers/BetterLuma/main/BetterLuma.toml.example",
            "https://github.com/0xNullPointers/BetterLuma/raw/main/BetterLuma.toml.example"
        )
        Write-Host "  [*] Downloading BetterLuma.toml.example from GitHub..." -ForegroundColor Cyan

        foreach ($url in $rawUrls) {
            try {
                $webClient = New-Object System.Net.WebClient
                $webClient.Headers.Add("User-Agent", "BetterLuma-Installer")
                $content = $webClient.DownloadString($url)
                if (-not [string]::IsNullOrWhiteSpace($content) -and $content.Length -gt 100) {
                    Write-Host "  [+] Successfully fetched BetterLuma.toml.example." -ForegroundColor Green
                    return $content
                }
            } catch {}
        }

        if (Get-Command 'curl.exe' -ErrorAction SilentlyContinue) {
            foreach ($url in $rawUrls) {
                try {
                    $curlOut = & curl.exe -sSL --fail --connect-timeout 10 $url
                    if (-not [string]::IsNullOrWhiteSpace($curlOut) -and $curlOut.Length -gt 100) {
                        Write-Host "  [+] Successfully fetched BetterLuma.toml.example via curl." -ForegroundColor Green
                        return $curlOut
                    }
                } catch {}
            }
        }

        throw "Failed to download BetterLuma.toml.example from GitHub."
    }

    function Merge-BetterLumaToml {
        param(
            [string]$TemplateContent,
            [string]$ExistingContent,
            [string]$DefaultPatternUrl = "https://gitlab.com/0xBadCod3/Steam-Auto-PT/-/raw/{channel}/{component}/{sha256}.toml"
        )

        $existingConfig = if ([string]::IsNullOrWhiteSpace($ExistingContent)) {
            [System.Collections.Generic.Dictionary[string, System.Collections.Generic.Dictionary[string, string]]]::new([System.StringComparer]::OrdinalIgnoreCase)
        } else {
            Parse-TomlContent $ExistingContent
        }

        $templateLines = $TemplateContent -split '\r?\n'
        $outLines = [System.Collections.Generic.List[string]]::new()
        $currentSection = ""
        $inSkipMultiline = $false
        $appliedKeys = [System.Collections.Generic.Dictionary[string, System.Collections.Generic.HashSet[string]]]::new([System.StringComparer]::OrdinalIgnoreCase)

        # Pre-scan template to identify all uncommented keys per section
        $templateKeysPerSection = [System.Collections.Generic.Dictionary[string, System.Collections.Generic.HashSet[string]]]::new([System.StringComparer]::OrdinalIgnoreCase)
        $tmpSec = ""
        foreach ($l in $templateLines) {
            $t = $l.Trim()
            if ($t -match '^\[([a-zA-Z0-9_\.\-]+)\]$') {
                $tmpSec = $matches[1]
                if (-not $templateKeysPerSection.ContainsKey($tmpSec)) {
                    $templateKeysPerSection[$tmpSec] = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
                }
            } elseif ($tmpSec -and ($t -match '^([a-zA-Z0-9_\-]+)\s*=\s*(.*)$')) {
                $templateKeysPerSection[$tmpSec].Add($matches[1]) | Out-Null
            }
        }

        foreach ($line in $templateLines) {
            $trimmed = $line.Trim()

            if ($inSkipMultiline) {
                $cleanLine = ($line -replace '#.*$', '').Trim()
                if ($cleanLine.EndsWith("]")) {
                    $inSkipMultiline = $false
                }
                continue
            }

            if ($trimmed -match '^\[([a-zA-Z0-9_\.\-]+)\]$') {
                $currentSection = $matches[1]
                if (-not $appliedKeys.ContainsKey($currentSection)) {
                    $appliedKeys[$currentSection] = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
                }
                $outLines.Add($line)
                continue
            }

            # Check commented optional key in template (e.g. # hubcap_key = "xxxxxx") that user has set,
            # but only if this key is NOT already present uncommented in this section of the template
            if ($currentSection -and ($trimmed -match '^#\s*([a-zA-Z0-9_\-]+)\s*=\s*(.*)$')) {
                $key = $matches[1]
                $hasUncommented = $templateKeysPerSection.ContainsKey($currentSection) -and $templateKeysPerSection[$currentSection].Contains($key)
                if (-not $hasUncommented) {
                    if ($existingConfig.ContainsKey($currentSection) -and $existingConfig[$currentSection].ContainsKey($key)) {
                        $userVal = $existingConfig[$currentSection][$key]
                        $indent = ($line -replace '^(\s*).*$', '$1')
                        $outLines.Add("${indent}${key} = ${userVal}")
                        $appliedKeys[$currentSection].Add($key) | Out-Null
                        continue
                    }
                }
            }

            if ($currentSection -and ($trimmed -match '^([a-zA-Z0-9_\-]+)\s*=\s*(.*)$')) {
                $key = $matches[1]
                $val = $matches[2].Trim()
                $appliedKeys[$currentSection].Add($key) | Out-Null

                $isTemplateMultiline = ($val.StartsWith("[") -and -not (($val -replace '#.*$', '').Trim().EndsWith("]")))

                $userVal = $null
                if ($existingConfig.ContainsKey($currentSection) -and $existingConfig[$currentSection].ContainsKey($key)) {
                    $userVal = $existingConfig[$currentSection][$key]
                }

                # Ensure [pattern_fetch] url_template gets user's value, migrated legacy format, or default pattern url
                if ($currentSection -eq "pattern_fetch" -and $key -eq "url_template") {
                    if ([string]::IsNullOrWhiteSpace($userVal) -or $userVal -eq '""' -or $userVal -eq "''") {
                        if ($existingConfig.ContainsKey("pattern_fetch") -and $existingConfig["pattern_fetch"].ContainsKey("mirror") -and -not [string]::IsNullOrWhiteSpace($existingConfig["pattern_fetch"]["mirror"])) {
                            $userVal = $existingConfig["pattern_fetch"]["mirror"]
                            $appliedKeys["pattern_fetch"].Add("mirror") | Out-Null
                        } elseif ($existingConfig.ContainsKey("remote") -and $existingConfig["remote"].ContainsKey("url_template") -and -not [string]::IsNullOrWhiteSpace($existingConfig["remote"]["url_template"])) {
                            $userVal = $existingConfig["remote"]["url_template"]
                            if (-not $appliedKeys.ContainsKey("remote")) {
                                $appliedKeys["remote"] = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
                            }
                            $appliedKeys["remote"].Add("url_template") | Out-Null
                        }
                    }
                    if ([string]::IsNullOrWhiteSpace($userVal) -or $userVal -eq '""' -or $userVal -eq "''") {
                        $userVal = "`"$DefaultPatternUrl`""
                    }
                }

                if ($userVal -ne $null) {
                    $indent = ($line -replace '^(\s*).*$', '$1')
                    $outLines.Add("${indent}${key} = ${userVal}")
                    if ($isTemplateMultiline) {
                        $inSkipMultiline = $true
                    }
                } else {
                    if ($currentSection -eq "pattern_fetch" -and $key -eq "url_template" -and ($val -eq '""' -or $val -eq "''")) {
                        $indent = ($line -replace '^(\s*).*$', '$1')
                        $outLines.Add("${indent}${key} = `"$DefaultPatternUrl`"")
                    } else {
                        $outLines.Add($line)
                    }
                }
                continue
            }

            $outLines.Add($line)
        }

        # Append any extra sections/keys from user's existing config that were not in template
        $extraLines = [System.Collections.Generic.List[string]]::new()
        foreach ($sec in $existingConfig.Keys) {
            $missingKeys = [System.Collections.Generic.List[string]]::new()
            foreach ($k in $existingConfig[$sec].Keys) {
                if (-not $appliedKeys.ContainsKey($sec) -or -not $appliedKeys[$sec].Contains($k)) {
                    $missingKeys.Add($k)
                }
            }
            if ($missingKeys.Count -gt 0) {
                $extraLines.Add("")
                $extraLines.Add("[$sec]")
                foreach ($k in $missingKeys) {
                    $v = $existingConfig[$sec][$k]
                    $extraLines.Add("${k} = ${v}")
                }
            }
        }

        if ($extraLines.Count -gt 0) {
            $outLines.AddRange($extraLines)
        }

        return ($outLines -join "`r`n")
    }

    $tomlPath = Join-Path $detectedSteam "BetterLuma.toml"
    $hasExistingToml = (Test-Path -LiteralPath $tomlPath)
    $existingToml = if ($hasExistingToml) {
        Get-Content -LiteralPath $tomlPath -Raw
    } else {
        ""
    }

    try {
        $templateToml = Get-BetterLumaTomlTemplate -ExtractedDir $tempExt
        $mergedToml   = Merge-BetterLumaToml -TemplateContent $templateToml -ExistingContent $existingToml

        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($tomlPath, $mergedToml, $utf8NoBom)

        if (-not $hasExistingToml) {
            Write-Host "  [+] Created BetterLuma.toml from official example with Pattern URL" -ForegroundColor Green
        } else {
            Write-Host "  [+] Updated BetterLuma.toml (preserved settings and applied new defaults)" -ForegroundColor Green
        }
    } catch {
        if ($hasExistingToml) {
            Write-Warning "Could not update BetterLuma.toml template ($($_.Exception.Message)). Existing configuration was preserved."
        } else {
            throw
        }
    }

    # Step 7: Generate uninstall-BetterLuma.bat
    Write-Host ""
    Write-Host "[*] Generating uninstall-BetterLuma.bat..." -ForegroundColor Cyan

    $batPath = Join-Path $detectedSteam "uninstall-BetterLuma.bat"
    $batLines = [System.Collections.Generic.List[string]]::new()

    $batLines.Add("@echo off")
    $batLines.Add("setlocal enabledelayedexpansion")
    $batLines.Add("title BetterLuma Uninstaller")
    $batLines.Add("pushd `"%~dp0`"")
    $batLines.Add("")
    $batLines.Add("echo ===================================================")
    $batLines.Add("echo             BetterLuma Uninstaller")
    $batLines.Add("echo ===================================================")
    $batLines.Add("echo.")
    $batLines.Add("")
    $batLines.Add(":: Check if Steam is running")
    $batLines.Add("tasklist /fi `"imagename eq steam.exe`" 2>NUL | find /i `"steam.exe`" >NUL")
    $batLines.Add("if `"%ERRORLEVEL%`"==`"0`" (")
    $batLines.Add("    echo [!] Steam is currently running.")
    $batLines.Add("    echo     Steam must be closed to safely remove and restore DLLs.")
    $batLines.Add("    echo.")
    $batLines.Add("    set /p `"CLOSE_STEAM=Close Steam now automatically? (Y/N): `"")
    $batLines.Add("    if /i `"!CLOSE_STEAM!`"==`"Y`" (")
    $batLines.Add("        echo [*] Closing Steam...")
    $batLines.Add("        taskkill /F /IM steam.exe >nul 2>&1")
    $batLines.Add("        timeout /t 2 /nobreak >nul")
    $batLines.Add("    ) else (")
    $batLines.Add("        echo [X] Uninstallation aborted. Please exit Steam and run this again.")
    $batLines.Add("        pause")
    $batLines.Add("        popd")
    $batLines.Add("        exit /b 1")
    $batLines.Add("    )")
    $batLines.Add(")")
    $batLines.Add("")
    $batLines.Add("echo [*] Removing BetterLuma installed files...")

    # Dynamic deletion of installed DLLs
    foreach ($name in $installedDllNames) {
        $batLines.Add("if exist `"%~dp0$name`" (")
        $batLines.Add("    del /f /q `"%~dp0$name`" >nul 2>&1")
        $batLines.Add("    if not exist `"%~dp0$name`" (")
        $batLines.Add("        echo   [-] Removed: $name")
        $batLines.Add("    ) else (")
        $batLines.Add("        echo   [*] Warning: Could not remove $name")
        $batLines.Add("    )")
        $batLines.Add(")")
    }

    # Deletion of BetterLuma.toml
    $batLines.Add("if exist `"%~dp0BetterLuma.toml`" (")
    $batLines.Add("    del /f /q `"%~dp0BetterLuma.toml`" >nul 2>&1")
    $batLines.Add("    if not exist `"%~dp0BetterLuma.toml`" (")
    $batLines.Add("        echo   [-] Removed: BetterLuma.toml")
    $batLines.Add("    )")
    $batLines.Add(")")

    # Deletion of betterluma runtime directory (logs and cache)
    $batLines.Add("if exist `"%~dp0betterluma`" (")
    $batLines.Add("    rmdir /s /q `"%~dp0betterluma`" >nul 2>&1")
    $batLines.Add("    if not exist `"%~dp0betterluma`" (")
    $batLines.Add("        echo   [-] Removed: betterluma folder")
    $batLines.Add("    )")
    $batLines.Add(")")

    $batLines.Add("")
    $batLines.Add("echo [*] Restoring backed up [.bak] files...")

    # Dynamic restoration of backed up files
    $restoredInBat = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    if ($backedUpFiles.Count -gt 0) {
        foreach ($b in $backedUpFiles) {
            $orig = $b.Original
            $bak  = $b.Backup
            if ($restoredInBat.Add($orig)) {
                $batLines.Add("if exist `"%~dp0$bak`" (")
                $batLines.Add("    move /y `"%~dp0$bak`" `"%~dp0$orig`" >nul 2>&1")
                $batLines.Add("    if exist `"%~dp0$orig`" (")
                $batLines.Add("        echo   [+] Restored: $orig [from $bak]")
                $batLines.Add("    ) else (")
                $batLines.Add("        echo   [*] Warning: Failed to restore $bak to $orig")
                $batLines.Add("    )")
                $batLines.Add(")")
            }
        }
    }

    # Priority target safety check: always restore opensteamtool.dll, LumaCore.dll, LumaCorePayload.dll, LumaCorePayload32.dll if .bak exists
    foreach ($pf in $priorityFiles) {
        if ($restoredInBat.Add($pf)) {
            $pfBak = "$pf.bak"
            $batLines.Add("if exist `"%~dp0$pfBak`" (")
            $batLines.Add("    move /y `"%~dp0$pfBak`" `"%~dp0$pf`" >nul 2>&1")
            $batLines.Add("    if exist `"%~dp0$pf`" (")
            $batLines.Add("        echo   [+] Restored: $pf [from $pfBak]")
            $batLines.Add("    ) else (")
            $batLines.Add("        echo   [*] Warning: Failed to restore $pfBak to $pf")
            $batLines.Add("    )")
            $batLines.Add(")")
        }
    }

    $batLines.Add("")
    $batLines.Add("echo.")
    $batLines.Add("echo [V] BetterLuma has been successfully uninstalled.")
    $batLines.Add("echo.")
    $batLines.Add("pause")
    $batLines.Add("popd")
    $batLines.Add(":: Self-delete uninstaller batch file")
    $batLines.Add("(goto) 2>nul & del /f /q `"%~f0`"")

    [System.IO.File]::WriteAllLines($batPath, $batLines, [System.Text.Encoding]::ASCII)
    Write-Host "  [+] Generated: uninstall-BetterLuma.bat" -ForegroundColor Green

    # Step 8: Final Summary
    Write-Host ""
    Write-Host "Installation Complete!" -ForegroundColor Green
    Write-Host "Target Directory: $detectedSteam"
    Write-Host "Build Installed: $BuildType"
    Write-Host "Files Installed: $(($installedDllNames + @('BetterLuma.toml', 'uninstall-BetterLuma.bat')) -join ', ')"
    Write-Host "Uninstaller: $(Join-Path $detectedSteam 'uninstall-BetterLuma.bat')" -ForegroundColor Cyan
    Write-Host ""
}
finally {
    # Clean up temporary download and extraction directories
    if (Test-Path $tempRoot) {
        Remove-Item -Path $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
