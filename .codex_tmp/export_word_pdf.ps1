$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$docx = Join-Path $root 'docs\MQTT_Server_Communication_Flow.docx'
$pdf = Join-Path $root '.codex_tmp\MQTT_Server_Communication_Flow.pdf'
$word = New-Object -ComObject Word.Application
$word.Visible = $false
$word.DisplayAlerts = 0
$doc = $word.Documents.Open($docx, $false, $true)
try {
    $doc.ExportAsFixedFormat($pdf, 17)
    Write-Output $pdf
} finally {
    try { $doc.Close($false) } catch {}
    try { $word.Quit() } catch {}
    try { [System.Runtime.InteropServices.Marshal]::ReleaseComObject($doc) | Out-Null } catch {}
    try { [System.Runtime.InteropServices.Marshal]::ReleaseComObject($word) | Out-Null } catch {}
}
