#!/usr/bin/env bash
# Print the machine's power state in one line. Called before and after every M2.5 run,
# because a laptop's power mode and battery level change what the CPU is allowed to do
# (M2.4 measured 8-17% throughput and a 4x change in CoV between two Windows power modes).
powershell.exe -NoProfile -Command "
Add-Type -AssemblyName System.Windows.Forms
\$p=[System.Windows.Forms.SystemInformation]::PowerStatus
\$o=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes'
\$n=switch (\$o.ActiveOverlayAcPowerScheme) {
  'ded574b5-45a0-4f42-8737-46345c09c238' {'Best performance'}
  '961cc777-2547-4f9d-8174-7d86181b8a7a' {'Best power efficiency'}
  default {'Balanced/unknown(' + \$o.ActiveOverlayAcPowerScheme + ')'} }
'power=' + \$p.PowerLineStatus + ' battery=' + [int](\$p.BatteryLifePercent*100) + '% mode=' + \$n
" 2>/dev/null | tr -d '\r'
