$ErrorActionPreference = 'Stop'
$sdkRevision = '374959484e79a640feaba44c93ac8cfb0a03f5b5'
$sdkBase = "https://raw.githubusercontent.com/NVIDIA/DLSS/$sdkRevision/"
$sdkRoot = Join-Path $PSScriptRoot 'third_party\ngx'
$sdkFiles = @(
    @{ Remote='include/nvsdk_ngx.h'; Local='include\nvsdk_ngx.h'; Hash='DC38E7467CF415379C9D12AE1B6E4A494C453ED92720FB53E92AECB523E7B848' },
    @{ Remote='include/nvsdk_ngx_defs.h'; Local='include\nvsdk_ngx_defs.h'; Hash='EA23F33497CD274860D1C25A97644FCE807DCB0037C594547203343103FAD03E' },
    @{ Remote='include/nvsdk_ngx_params.h'; Local='include\nvsdk_ngx_params.h'; Hash='943BC8CC5CDAE03B6303016FBAD3183636F2335AE27A2D18776798C3B4EFABBC' },
    @{ Remote='lib/Windows_x86_64/x64/nvsdk_ngx_s.lib'; Local='lib\nvsdk_ngx_s.lib'; Hash='4E5D355086D2BC11E1A0842457D2519EA528EE1F3E112C45679A84960C07DFF3' },
    @{ Remote='LICENSE.txt'; Local='LICENSE.txt'; Hash='D4216E39EBEF5F9B50A6712EBB37BEEB5379862A67733A9999C651F21592AAF0' }
)
foreach ($sdkFile in $sdkFiles) {
    $sdkPath = Join-Path $sdkRoot $sdkFile.Local
    if ((Test-Path -LiteralPath $sdkPath) -and (Get-FileHash -LiteralPath $sdkPath -Algorithm SHA256).Hash -eq $sdkFile.Hash) { continue }
    New-Item -ItemType Directory -Path (Split-Path -Parent $sdkPath) -Force | Out-Null
    Invoke-WebRequest -Uri ($sdkBase + $sdkFile.Remote) -OutFile $sdkPath
    if ((Get-FileHash -LiteralPath $sdkPath -Algorithm SHA256).Hash -ne $sdkFile.Hash) { throw "SDK hash mismatch: $sdkPath" }
}
Write-Output "Verified official NVIDIA SDK revision $sdkRevision. Its license is in third_party\ngx\LICENSE.txt."
