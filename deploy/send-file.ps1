<#
.SYNOPSIS
    Send a file to the ATK-MP157 board over the CH340 serial console using
    the rx.py robust protocol (per-line md5 + iterative resend of bad lines).

.DESCRIPTION
    The serial tty drops characters on bulk input (the Qt desktop keeps the
    CPU busy, so the tty reader is scheduled late and the 4KB tty buffer
    overflows), so a plain base64 stream is not reliable. This script:
      1. verifies the shell prompt, then starts python3 /tmp/rx.py (echo off)
      2. streams "<seq> <md5_8> <base64chunk>" lines
      3. sends __END__, reads the RESULT report (full bad/missing line lists)
      4. resends only the bad/missing lines, sends __REPORT__, repeats until
         the receiver reports nothing bad (max -MaxRounds)
      5. sends __DONE__, verifies the final md5 reported by the receiver
    Echo stays off during the protocol (no echo backlog) and is restored
    afterwards.

.PARAMETER File
    Local file to transfer.

.PARAMETER DestPath
    Absolute destination path on the board.

.EXAMPLE
    .\send-file.ps1 -File probe-armhf -DestPath /tmp/probe
#>
param(
  [Parameter(Mandatory = $true)][string]$File,
  [Parameter(Mandatory = $true)][string]$DestPath,
  [string]$ComPort = 'COM3',
  [int]$Baud = 115200,
  [int]$ChunkChars = 980,
  [int]$ChunkDelayMs = 250,
  [int]$MaxRounds = 8,
  [int]$Attempts = 2
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $File)) { throw "file not found: $File" }
$resolved = (Resolve-Path -LiteralPath $File).Path
$bytes = [System.IO.File]::ReadAllBytes($resolved)
$localMd5 = (Get-FileHash -Algorithm MD5 -LiteralPath $resolved).Hash.ToLower()

$b64 = [Convert]::ToBase64String($bytes)
$md5alg = [System.Security.Cryptography.MD5]::Create()
$lines = New-Object System.Collections.Generic.List[string]
$seq = 1
for ($i = 0; $i -lt $b64.Length; $i += $ChunkChars) {
  $data = $b64.Substring($i, [Math]::Min($ChunkChars, $b64.Length - $i))
  $chk = ([BitConverter]::ToString($md5alg.ComputeHash([Text.Encoding]::ASCII.GetBytes($data))).Replace('-', '')).Substring(0, 8).ToLower()
  $lines.Add("$seq $chk $data")
  $seq++
}
$total = $lines.Count
Write-Output "file=$resolved size=$($bytes.Length)B lines=$total local_md5=$localMd5"

$port = New-Object System.IO.Ports.SerialPort $ComPort, $Baud, 'None', 8, 'One'
$port.ReadTimeout = 2000
$port.WriteTimeout = 5000
$port.DtrEnable = $true
$port.RtsEnable = $true

function Read-UntilIdle([double]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $out = ''
  while ((Get-Date) -lt $deadline) {
    if ($port.BytesToRead -gt 0) { $out += $port.ReadExisting() }
    Start-Sleep -Milliseconds 50
  }
  return $out
}

function Parse-Ints($text, $key) {
  $vals = @()
  if ($text -match "$key=\[([^\]]*)\]") {
    foreach ($m in [regex]::Matches($Matches[1], '\d+')) { $vals += [int]$m.Value }
  }
  return $vals
}

function Send-Lines($seqs) {
  foreach ($s in $seqs) {
    if ($s -ge 1 -and $s -le $total) {
      $port.Write("$($lines[$s - 1])`n")
      Start-Sleep -Milliseconds $ChunkDelayMs
    }
  }
}

$success = $false
try {
  $port.Open()
  Start-Sleep -Milliseconds 500
  Read-UntilIdle 0.5 | Out-Null
  $port.Write("dmesg -n 1; stty -echo`r")
  Read-UntilIdle 1.5 | Out-Null

  for ($attempt = 1; $attempt -le $Attempts -and -not $success; $attempt++) {
    Write-Output "--- attempt $attempt/$Attempts ---"
    # make sure we are at a clean prompt before starting the receiver
    $port.Write("echo PTEST$attempt`r")
    $pv = Read-UntilIdle 3
    if ($pv -notmatch "PTEST$attempt") {
      Write-Output "no prompt marker ($($pv.Trim())); sending Ctrl-C and continuing"
      $port.Write([char]3)
      Start-Sleep -Milliseconds 500
    }
    $port.Write("python3 /usr/local/bin/rx.py '$DestPath' $localMd5`r")
    Start-Sleep -Milliseconds 2000
    foreach ($ln in $lines) {
      $port.Write("$ln`n")
      Start-Sleep -Milliseconds $ChunkDelayMs
    }
    $port.Write("__END__`n")
    $res = Read-UntilIdle 8
    if ($res.Trim()) { Write-Output "report: $($res.Trim())" }
    $bad = Parse-Ints $res 'badlist'
    $missing = Parse-Ints $res 'missing'
    $round = 0
    while (($bad.Count -gt 0 -or $missing.Count -gt 0) -and $round -lt $MaxRounds) {
      $round++
      $fixset = New-Object System.Collections.Generic.HashSet[int]
      foreach ($v in @($bad)) { if ($null -ne $v) { [void]$fixset.Add([int]$v) } }
      foreach ($v in @($missing)) { if ($null -ne $v) { [void]$fixset.Add([int]$v) } }
      $fix = @($fixset | Sort-Object)
      Write-Output "fix round ${round}: resending $($fix.Count) lines"
      Send-Lines $fix
      $port.Write("__REPORT__`n")
      $res = Read-UntilIdle 8
      if ($res.Trim()) { Write-Output "report: $($res.Trim())" }
      $bad = Parse-Ints $res 'badlist'
      $missing = Parse-Ints $res 'missing'
    }
    if ($bad.Count -gt 0 -or $missing.Count -gt 0) {
      Write-Output 'did not converge; aborting receiver and retrying'
      $port.Write([char]3)
      Start-Sleep -Milliseconds 500
      continue
    }
    $port.Write("__DONE__`n")
    $fin = Read-UntilIdle 8
    if ($fin.Trim()) { Write-Output "final: $($fin.Trim())" }
    if ($fin -match 'md5=([0-9a-f]{32})' -and $Matches[1] -eq $localMd5 -and $fin -match '(?m)^OK') {
      $success = $true
      Write-Output 'PROTOCOL OK - md5 verified by receiver'
    } else {
      Write-Output 'final verification failed; aborting receiver and retrying'
      $port.Write([char]3)
      Start-Sleep -Milliseconds 500
    }
  }
} finally {
  try {
    if ($port.IsOpen) {
      $port.Write("stty echo`r")
      Start-Sleep -Milliseconds 300
      $port.Close()
    }
  } catch { }
}

if (-not $success) {
  Write-Output 'TRANSFER FAILED'
  exit 1
}
Write-Output 'TRANSFER OK'
