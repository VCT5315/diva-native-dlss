<#
.SYNOPSIS
    Obtains the AMD FSR 4 upscaler runtime used by the FSR 4.1 backend.

.DESCRIPTION
    The mod does not ship amd_fidelityfx_upscaler_dx12.dll. Many FSR 4 games
    already install a copy next to their executable and the mod will find those
    on its own, so only run this if discovery fails.

    This script downloads the pinned FidelityFX SDK release and extracts
    Kits\FidelityFX\signedbin\amd_fidelityfx_upscaler_dx12.dll into the mod
    folder. Per the SDK's docs/license.md the upscaler DLL is covered by the MIT
    exception list in that file; keep AMD's notice alongside it.

    The download is large (~125 MiB) but only the single DLL is kept.
#>
$ErrorActionPreference = 'Stop'

$sdkVersion = 'v2.3.0'   # AMD FSR SDK 2.3.0 "Redstone" -> FSR Upscaling 4.1.1
$sdkUrl     = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/$sdkVersion/FidelityFX-Samples-$sdkVersion-prebuilt.zip"
$assetInZip = 'Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll'
$destination = Join-Path $PSScriptRoot 'mods\Diva Native DLSS\amd_fidelityfx_upscaler_dx12.dll'

Write-Output "Pinned FidelityFX SDK $sdkVersion (FSR Upscaling 4.1.1)."

if ((Test-Path -LiteralPath $destination) -and ((Get-Item $destination).VersionInfo.FileVersion -like '4.*'))
{
    Write-Output "Already present: $destination"
    exit 0
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("diva-fsr-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$zip = Join-Path $work 'sdk.zip'

try
{
    Write-Output "Downloading $sdkUrl"
    Invoke-WebRequest -Uri $sdkUrl -OutFile $zip

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try
    {
        $entry = $archive.Entries | Where-Object { $_.FullName -ieq $assetInZip } | Select-Object -First 1
        if (-not $entry) { throw "Release archive does not contain $assetInZip" }

        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
    }
    finally { $archive.Dispose() }
}
finally
{
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

$info = (Get-Item -LiteralPath $destination).VersionInfo
Write-Output "Installed $destination (file version $($info.FileVersion))."
if ($info.FileVersion -notlike '4.*')
{
    Write-Warning 'Unexpected upscaler version; FSR 4.x was expected.'
}
Write-Output 'FSR 4.1 backend is ready. The mod also finds copies installed by FSR 4 games.'
