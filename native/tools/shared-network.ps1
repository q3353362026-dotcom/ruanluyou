[CmdletBinding(SupportsShouldProcess=$true)]
param(
    [ValidateSet('Info','EnableSharing','Preflight','Capture')][string]$Mode='Info',
    [uint32]$PublicIfIndex=0,
    [uint32]$PrivateIfIndex=0,
    [string]$ClientIp,
    [string]$HostIp,
    [switch]$RedirectTcp,
    [ValidateRange(0,65535)][int]$TcpPort=0,
    [string]$TcpServer,
    [ValidateRange(0,65535)][int]$UdpPort=0,
    [ValidateRange(0,65535)][int]$ListenPort=0,
    [ValidateRange(1,604800)][int]$Seconds=60,
    [string]$Device='shared-client',
    [string]$Out,
    [string]$SaveBanks,
    [string[]]$BankFile=@(),
    [string]$WinDivertDll,
    [string]$Exe
)
$ErrorActionPreference='Stop'
$nativeRoot=Split-Path -Parent $PSScriptRoot
$repoRoot=Split-Path -Parent $nativeRoot
. (Join-Path $PSScriptRoot 'runtime-paths.ps1')
if(-not $Exe) {$Exe=Get-RadaerExecutable}
if(-not $WinDivertDll) {$WinDivertDll=Join-Path $repoRoot 'runtime/release_resources/WinDivert.dll'}
Initialize-RadaerRuntime $Exe
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false)
function Assert-Admin {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=New-Object Security.Principal.WindowsPrincipal($identity)
    if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Open an Administrator PowerShell for sharing setup or capture.'
    }
}
function Get-IcsEntries {
    $manager=New-Object -ComObject HNetCfg.HNetShare
    foreach($connection in @($manager.EnumEveryConnection)) {
        $properties=$manager.NetConnectionProps.Invoke($connection)
        $configuration=$manager.INetSharingConfigurationForINetConnection.Invoke($connection)
        [pscustomobject]@{Name=$properties.Name;Guid=$properties.Guid;Configuration=$configuration;
            Enabled=[bool]$configuration.SharingEnabled;
            Role=if($configuration.SharingEnabled){[int]$configuration.SharingConnectionType}else{$null}}
    }
}
function Invoke-Native([string[]]$NativeArguments) {
    & $Exe @NativeArguments
    if($LASTEXITCODE -ne 0) {throw "Native command failed with exit code $LASTEXITCODE"}
}
if($Mode -eq 'Info') {
    $adapters=@(Get-NetAdapter -IncludeHidden | Select-Object Name,InterfaceDescription,ifIndex,Status,InterfaceGuid)
    $addresses=@(Get-NetIPAddress -AddressFamily IPv4 | Select-Object InterfaceIndex,IPAddress,PrefixLength)
    $neighbors=@(Get-NetNeighbor -AddressFamily IPv4 | Where-Object {$_.State -notin @('Unreachable','Incomplete')} |
        Select-Object InterfaceIndex,IPAddress,LinkLayerAddress,State)
    $ics=@();$icsError=$null
    try {$ics=@(Get-IcsEntries | Select-Object Name,Guid,Enabled,Role)} catch {$icsError=$_.Exception.Message}
    [pscustomobject]@{Adapters=$adapters;IPv4=$addresses;Neighbors=$neighbors;ICS=$ics;IcsReadError=$icsError;
        RoleMeaning='0=public Internet uplink; 1=private client network';
        Note='For Mobile Hotspot, enable Settings > Network & Internet > Mobile hotspot, then inspect its private adapter.'} |
        ConvertTo-Json -Depth 6
    return
}
if(-not $PrivateIfIndex) {throw '-PrivateIfIndex is required; inspect -Mode Info first.'}
$private=Get-NetAdapter -IncludeHidden | Where-Object {$_.ifIndex -eq $PrivateIfIndex}
if(@($private).Count -ne 1) {throw 'Private interface index does not identify exactly one adapter.'}
if($Mode -eq 'EnableSharing') {
    Assert-Admin
    if(-not $PublicIfIndex -or $PublicIfIndex -eq $PrivateIfIndex) {throw 'Select different public uplink and private client adapters.'}
    $public=Get-NetAdapter -IncludeHidden | Where-Object {$_.ifIndex -eq $PublicIfIndex}
    if(@($public).Count -ne 1) {throw 'Public adapter not found.'}
    $entries=@(Get-IcsEntries)
    $pub=@($entries | Where-Object {([guid]$_.Guid) -eq ([guid]$public.InterfaceGuid)})
    $pri=@($entries | Where-Object {([guid]$_.Guid) -eq ([guid]$private.InterfaceGuid)})
    if($pub.Count -ne 1 -or $pri.Count -ne 1) {throw 'Selected adapters are not exposed by Windows ICS.'}
    foreach($entry in $entries) {
        if(-not $entry.Enabled) {continue}
        $matches=(([guid]$entry.Guid -eq [guid]$public.InterfaceGuid) -and $entry.Role -eq 0) -or
                 (([guid]$entry.Guid -eq [guid]$private.InterfaceGuid) -and $entry.Role -eq 1)
        if(-not $matches) {throw "Existing ICS configuration on '$($entry.Name)' conflicts. Adjust Windows sharing settings explicitly first."}
    }
    if($PSCmdlet.ShouldProcess("$($public.Name) -> $($private.Name)",'Enable Windows Internet Connection Sharing (may assign a new private IP)')) {
        $publicChanged=$false
        try {
            if(-not $pub[0].Enabled) {$pub[0].Configuration.EnableSharing(0);$publicChanged=$true}
            if(-not $pri[0].Enabled) {$pri[0].Configuration.EnableSharing(1)}
        } catch {
            if($publicChanged) {$pub[0].Configuration.DisableSharing()}
            throw
        }
        Get-IcsEntries | Select-Object Name,Guid,Enabled,Role | ConvertTo-Json
        Write-Host 'ICS enabled. Re-run -Mode Info; connect the client and verify its gateway and Internet connectivity.'
    }
    return
}
if(-not $ClientIp) {throw '-ClientIp is required; use the client IPv4 shown on its device.'}
if(-not $HostIp) {
    $privateIps=@(Get-NetIPAddress -InterfaceIndex $PrivateIfIndex -AddressFamily IPv4 | Where-Object {$_.IPAddress -notlike '169.254.*'})
    if($privateIps.Count -ne 1) {throw 'Private adapter has multiple/no IPv4 addresses; pass -HostIp explicitly.'}
    $HostIp=$privateIps[0].IPAddress
}
$Exe=(Resolve-Path -LiteralPath $Exe).Path
$WinDivertDll=(Resolve-Path -LiteralPath $WinDivertDll).Path
$captureArguments=@('--shared-windows','true','--shared-if-index',"$PrivateIfIndex",'--shared-host-ip',$HostIp,
    '--client-ip',$ClientIp,'--windivert-dll',$WinDivertDll)
if($RedirectTcp) {
    if(-not $PSBoundParameters.ContainsKey('TcpPort')) {$TcpPort=65010}
    $captureArguments+=@('--shared-redirect','true','--tcp-port',"$TcpPort",'--listen',($HostIp+':'+$ListenPort))
    if($TcpServer) {$captureArguments+=@('--tcp-server',$TcpServer)}
} elseif($TcpPort -or $TcpServer) {throw 'TCP filters require -RedirectTcp.'}
if($UdpPort) {$captureArguments+=@('--udp-port',"$UdpPort")}
Invoke-Native (@('shared-preflight')+$captureArguments)
if($Mode -eq 'Preflight') {return}
Assert-Admin
if(-not $Out) {throw 'Capture requires -Out with a new JSONL output path.'}
if(Test-Path -LiteralPath $Out) {throw 'Output exists; choose a new capture path.'}
$captureArguments+=@('--out',$Out,'--device',$Device,'--seconds',"$Seconds")
if($SaveBanks) {$captureArguments+=@('--save-banks',$SaveBanks)}
$captureArguments+=$BankFile
if(-not $PSCmdlet.ShouldProcess("$ClientIp via $($private.Name)",'Start bounded shared-network capture')) {return}
# The native process binds its listener first, then manages the temporary firewall rule.
Invoke-Native (@('capture')+$captureArguments)
