[CmdletBinding()]
param(
    [ValidateRange(1024,65535)][int]$Port=8765,
    [switch]$NoBrowser,
    [switch]$LocalOnly,
    [uint32]$LanIfIndex=0,
    [string]$LanIp,
    [string]$OutputDirectory,
    [switch]$AsAdministrator,
    [switch]$Check
)
$ErrorActionPreference='Stop'
try {
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Radaer requires 64-bit Windows.' }
    if ($OutputDirectory) { $OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory) }
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=New-Object Security.Principal.WindowsPrincipal($identity)
    $elevated=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($AsAdministrator -and -not $elevated) {
        # Encode literal PowerShell strings so spaces, quotes and non-ASCII paths survive UAC.
        $scriptLiteral="'"+$PSCommandPath.Replace("'","''")+"'"
        $command="& $scriptLiteral -Port $Port"
        if ($NoBrowser) { $command+=' -NoBrowser' }
        if ($LocalOnly) { $command+=' -LocalOnly' }
        if ($LanIfIndex) { $command+=" -LanIfIndex $LanIfIndex" }
        if ($LanIp) { $command+=" -LanIp '"+$LanIp.Replace("'","''")+"'" }
        if ($Check) { $command+=' -Check' }
        if ($OutputDirectory) { $command+=" -OutputDirectory '"+$OutputDirectory.Replace("'","''")+"'" }
        $command+='; exit $LASTEXITCODE'
        $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
        $child=Start-Process -FilePath "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -Verb RunAs -ArgumentList @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded) -PassThru -Wait
        exit $child.ExitCode
    }
    if (-not $elevated -and -not $Check) { Write-Host 'Device discovery is available. For packet capture, use start-admin.cmd.' -ForegroundColor Yellow }
    & (Join-Path $PSScriptRoot 'native/tools/start-webui.ps1') -Port $Port -NoBrowser:$NoBrowser -LocalOnly:$LocalOnly -LanIfIndex $LanIfIndex -LanIp $LanIp -OutputDirectory $OutputDirectory -Check:$Check
    exit $LASTEXITCODE
} catch {
    [Console]::Error.WriteLine('Radaer: '+$_.Exception.Message)
    exit 1
}
