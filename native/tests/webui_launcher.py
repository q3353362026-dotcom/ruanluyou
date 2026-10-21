"""Run the launcher with fake adapters, executable and firewall cmdlets."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    source = Path(sys.argv[1])
    powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    with tempfile.TemporaryDirectory(prefix='radaer-launcher-') as folder:
        root = Path(folder)
        scripts = root / 'native/tools'
        scripts.mkdir(parents=True)
        shutil.copy2(source, scripts / 'start-webui.ps1')
        assets = ['index.html', 'app.js', 'styles.css', 'radar/index.html', 'radar/view.css',
                  'radar/model.js', 'radar/replay.js', 'radar/view.js', 'radar/items.json', 'radar/maps.json',
                  'vendor/leaflet/leaflet.js', 'vendor/leaflet/leaflet.css', 'vendor/leaflet/LICENSE']
        for name in assets:
            path = root / 'native/webui' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture', encoding='utf-8')
        (root / 'native/webui/radar/operators.json').write_text('{"operators":{}}')
        driver = root / 'runtime/release_resources'
        driver.mkdir(parents=True)
        for name in ('WinDivert.dll', 'WinDivert64.sys'):
            (driver / name).write_bytes(b'TEST ONLY')
        (scripts / 'runtime-paths.ps1').write_text('''
function Get-RadaerExecutable { Join-Path $PSScriptRoot 'fake.ps1' }
function Initialize-RadaerRuntime($path) {}
''', encoding='utf-8')
        (scripts / 'fake.ps1').write_text('''
$global:LASTEXITCODE=0
if($args[0] -eq 'network-info') {
 '{"adapters":[{"up":true,"ifIndex":7,"name":"LAN A","ipv4":[{"address":"10.0.1.7","prefixLength":24}],"gateways":["10.0.1.1"]},{"up":true,"ifIndex":8,"name":"LAN B","ipv4":[{"address":"192.168.137.1","prefixLength":24}],"gateways":[]}]}'
} elseif($args[0] -eq 'webui') {
 $args | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot 'server.json')
 if($env:RADAER_TEST_FAIL -eq '1'){throw 'Simulated bind failure'}
}
''', encoding='utf-8')
        (scripts / 'harness.ps1').write_text('''
function New-NetFirewallRule {
 [CmdletBinding()] param($Name,$DisplayName,$Direction,$Action,$Protocol,$LocalPort,$LocalAddress,$InterfaceAlias,$RemoteAddress,$Program,$Profile)
 $PSBoundParameters | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot 'firewall.json')
}
function Remove-NetFirewallRule {
 [CmdletBinding()] param($Name)
 $Name | Set-Content (Join-Path $PSScriptRoot 'removed.txt')
}
function Get-NetAdapter {
 [CmdletBinding()] param([switch]$IncludeHidden)
 [pscustomobject]@{ifIndex=7;Name='LAN A'}
 [pscustomobject]@{ifIndex=8;Name='LAN B'}
}
try { & (Join-Path $PSScriptRoot 'start-webui.ps1') @args; exit $LASTEXITCODE }
catch { Write-Output $_; exit 1 }
''', encoding='utf-8')
        def run(args, input_text='', failure=False):
            for name in ('server.json', 'firewall.json', 'removed.txt'):
                (scripts / name).unlink(missing_ok=True)
            result = subprocess.run([str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                     '-File', str(scripts / 'harness.ps1')] + args,
                                    input=input_text, capture_output=True, text=True, errors='replace',
                                    env=dict(os.environ, RADAER_TEST_FAIL='1' if failure else '0'), timeout=15)
            return result

        firewall_checked = False
        for args, answer in ((['-LanIfIndex', '8'], ''), ([], '2\n')):
            result = run(args, answer)
            assert result.returncode == 0, (result.stdout, result.stderr)
            command = json.loads((scripts / 'server.json').read_text(encoding='utf-8-sig'))
            assert command[command.index('--lan-ip') + 1] == '192.168.137.1'
            assert command[command.index('--lan-if-index') + 1] == '8'
            if (scripts / 'firewall.json').exists():
                rule = json.loads((scripts / 'firewall.json').read_text(encoding='utf-8-sig'))
                assert rule['LocalAddress'] == '192.168.137.1' and rule['InterfaceAlias'] == 'LAN B'
                assert rule['RemoteAddress'] == '192.168.137.0/24'
                assert rule['Name'] == (scripts / 'removed.txt').read_text(encoding='utf-8-sig').strip()
                firewall_checked = True
        result = run(['-LanIfIndex', '8'], failure=True)
        assert result.returncode != 0
        if firewall_checked:
            assert (scripts / 'removed.txt').exists(), 'failure must remove the rule'
        result = run(['-LanIfIndex', '999'])
        assert result.returncode != 0 and not (scripts / 'server.json').exists()
        result = run(['-LocalOnly'])
        assert result.returncode == 0 and not (scripts / 'firewall.json').exists()
        command = json.loads((scripts / 'server.json').read_text(encoding='utf-8-sig'))
        assert command[command.index('--lan') + 1] == 'false' and '--lan-ip' not in command
        result = run(['-Check'])
        assert result.returncode == 0 and not (scripts / 'server.json').exists()
        assert not (scripts / 'firewall.json').exists()
        print(json.dumps({'interactiveAndExplicitSelection': True, 'mockFirewallScopeAndCleanup': firewall_checked,
                          'invalidSelectionRejected': True, 'realFirewallChanged': False}))


if __name__ == '__main__':
    main()
