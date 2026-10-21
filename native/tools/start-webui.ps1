[CmdletBinding()]
param([ValidateRange(1024,65535)][int]$Port=8765,[switch]$NoBrowser,[switch]$LocalOnly,[uint32]$LanIfIndex=0,[string]$LanIp,[string]$OutputDirectory,[switch]$Check)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false)
$nativeRoot=Split-Path -Parent $PSScriptRoot
$repoRoot=Split-Path -Parent $nativeRoot
. (Join-Path $PSScriptRoot 'runtime-paths.ps1')
$exe=Get-RadaerExecutable
Initialize-RadaerRuntime $exe
$assets=Join-Path $nativeRoot 'webui'
$driver=Join-Path $repoRoot 'runtime/release_resources/WinDivert.dll'
$assetFiles=@('index.html','app.js','styles.css','radar/index.html','radar/view.css','radar/model.js','radar/replay.js','radar/view.js','radar/items.json','radar/maps.json','radar/operators.json','vendor/leaflet/leaflet.js','vendor/leaflet/leaflet.css','vendor/leaflet/LICENSE')
$requiredFiles=@($assetFiles | ForEach-Object {Join-Path $assets $_}) + @($driver,(Join-Path (Split-Path $driver -Parent) 'WinDivert64.sys'))
foreach($required in $requiredFiles) {
    if(-not(Test-Path -LiteralPath $required -PathType Leaf)) {throw "Required file is missing: $required"}
}
$operatorCatalog=Get-Content -LiteralPath (Join-Path $assets 'radar/operators.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach($operator in $operatorCatalog.operators.PSObject.Properties) {
    if($operator.Value.portrait) {
        if($operator.Name -notmatch '^[0-9]{1,15}$') {throw 'Invalid operator catalog ID'}
        $portraitPath=Join-Path $assets ('radar/portraits/'+$operator.Name+'.png')
        if(-not(Test-Path -LiteralPath $portraitPath -PathType Leaf)) {throw "Required portrait is missing: $portraitPath"}
    }
}
if($Check) {
    & $exe help
    if($LASTEXITCODE -ne 0) {exit $LASTEXITCODE}
    Write-Host 'Startup check passed: executable, runtime DLLs, WebUI assets and driver files.'
    exit 0
}
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repoRoot 'captures/webui'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$bindIp='127.0.0.1'
$lanArguments=@()
if($LocalOnly -and ($LanIfIndex -or $LanIp)){throw '-LocalOnly cannot be combined with -LanIfIndex or -LanIp.'}
if(-not $LocalOnly) {
    $inventoryText=& $exe network-info
    if($LASTEXITCODE -ne 0){throw 'Cannot enumerate network adapters.'}
    $inventory=$inventoryText | ConvertFrom-Json
    $choices=@(foreach($adapter in $inventory.adapters) {
        if(-not $adapter.up){continue}
        foreach($address in $adapter.ipv4) {
            $bytes=([Net.IPAddress]::Parse($address.address)).GetAddressBytes()
            $private=$bytes[0] -eq 10 -or ($bytes[0] -eq 172 -and $bytes[1] -ge 16 -and $bytes[1] -le 31) -or ($bytes[0] -eq 192 -and $bytes[1] -eq 168)
            if(-not $private -or $address.prefixLength -lt 1 -or $address.prefixLength -gt 32){continue}
            if($LanIfIndex -and $adapter.ifIndex -ne $LanIfIndex){continue}
            if($LanIp -and $address.address -ne $LanIp){continue}
            $networkBytes=New-Object byte[] 4
            for($octet=0;$octet -lt 4;$octet++) {
                $bits=[Math]::Min(8,[Math]::Max(0,[int]$address.prefixLength-8*$octet))
                $mask=if($bits -eq 0){0}else{(255 -shl (8-$bits)) -band 255}
                $networkBytes[$octet]=$bytes[$octet] -band $mask
            }
            [pscustomobject]@{IfIndex=$adapter.ifIndex;Name=$adapter.name;Ip=$address.address;Prefix=$address.prefixLength;Subnet=($networkBytes -join '.')+'/'+$address.prefixLength;Gateway=($adapter.gateways -join ', ')}
        }
    })
    if(-not $choices.Count){throw 'No matching active private IPv4 adapter. Connect the desired LAN/hotspot, or use -LocalOnly.'}
    if($choices.Count -eq 1 -and ($LanIfIndex -or $LanIp)){$selected=$choices[0]}
    else {
        Write-Host 'Select ONE adapter/address for WebUI access (0 = local only):'
        for($i=0;$i -lt $choices.Count;$i++) {
            $c=$choices[$i]
            Write-Host ("[{0}] {1} | ifIndex {2} | {3}/{4} | Gateway: {5}" -f ($i+1),$c.Name,$c.IfIndex,$c.Ip,$c.Prefix,$c.Gateway)
        }
        do {
            $answer=Read-Host 'Adapter number'
            $choice=0
            $valid=[int]::TryParse($answer,[ref]$choice) -and $choice -ge 0 -and $choice -le $choices.Count
            if(-not $valid){Write-Host 'Enter a number from the list.'}
        } until($valid)
        if($choice -eq 0){$LocalOnly=$true}else{$selected=$choices[$choice-1]}
    }
    if(-not $LocalOnly) {
        $bindIp=$selected.Ip
        $lanArguments=@('--lan-ip',$bindIp,'--lan-if-index',[string]$selected.IfIndex)
    }
}
Write-Host "Radaer WebUI: http://${bindIp}:$Port"
Write-Host "Standalone radar: http://${bindIp}:$Port/radar"
Write-Host "Capture output: $OutputDirectory"
Write-Host 'Keep this window open. Press Ctrl+C to stop the server and capture.'
$openBrowser=if($NoBrowser){'false'}else{'true'}
$lan=if($LocalOnly){'false'}else{'true'}
$firewallRule=$null
$serverExit=1
try {
    if(-not $LocalOnly) {
        $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
        $principal=New-Object Security.Principal.WindowsPrincipal($identity)
        if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            $ruleName='Radaer-WebUI-'+[guid]::NewGuid().ToString('N')
            $interfaces=@(Get-NetAdapter -IncludeHidden -ErrorAction Stop | Where-Object {$_.ifIndex -eq $selected.IfIndex})
            if($interfaces.Count -ne 1){throw 'Selected adapter is no longer available. Restart and select again.'}
            $interface=$interfaces[0]
            New-NetFirewallRule -Name $ruleName -DisplayName "Radaer WebUI LAN TCP $Port" -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port -LocalAddress $bindIp -InterfaceAlias $interface.Name -RemoteAddress $selected.Subnet -Program $exe -Profile Any -ErrorAction Stop | Out-Null
            $firewallRule=$ruleName
            Write-Host "LAN access: $($selected.Name) / $bindIp only; clients in $($selected.Subnet)."
        } else {
            Write-Warning 'LAN access may be blocked by Windows Firewall. Use start-admin.cmd to open the port automatically.'
        }
    }
    & $exe webui --port $Port --lan $lan @lanArguments --open-browser $openBrowser --web-root $assets --output-directory $OutputDirectory --windivert-dll $driver
    $serverExit=$LASTEXITCODE
} finally {
    if($firewallRule) {
        Remove-NetFirewallRule -Name $firewallRule -ErrorAction Continue
    }
}
exit $serverExit
