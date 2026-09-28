$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$asTaskGeneric = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 } |
    Select-Object -First 1
$asTaskAction = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and -not $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 } |
    Select-Object -First 1

function Await-Result($operation, [Type]$resultType) {
    $method = $asTaskGeneric.MakeGenericMethod($resultType)
    $task = $method.Invoke($null, @($operation))
    $task.Wait()
    return $task.Result
}

function Await-Action($operation) {
    $task = $asTaskAction.Invoke($null, @($operation))
    $task.Wait()
}

[Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
[Windows.Data.Pdf.PdfDocument, Windows.Data.Pdf, ContentType = WindowsRuntime] | Out-Null
[Windows.Storage.Streams.InMemoryRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime] | Out-Null
[Windows.Storage.Streams.DataReader, Windows.Storage.Streams, ContentType = WindowsRuntime] | Out-Null

$root = (Get-Location).Path
$pdfPath = Join-Path $root '.codex_tmp\MQTT_Server_Communication_Flow.pdf'
$renderDir = Join-Path $root '.codex_tmp\rendered_pdf_final'
New-Item -ItemType Directory -Force -Path $renderDir | Out-Null
Get-ChildItem -LiteralPath $renderDir -Filter 'page-*.png' -ErrorAction SilentlyContinue | Remove-Item -Force

$fileOp = [Windows.Storage.StorageFile]::GetFileFromPathAsync($pdfPath)
$file = Await-Result $fileOp ([Windows.Storage.StorageFile])
$pdfOp = [Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($file)
$pdf = Await-Result $pdfOp ([Windows.Data.Pdf.PdfDocument])

for ($i = 0; $i -lt $pdf.PageCount; $i++) {
    $page = $pdf.GetPage($i)
    $stream = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
    try {
        Await-Action ($page.RenderToStreamAsync($stream))
        $stream.Seek(0)
        $input = $stream.GetInputStreamAt(0)
        $reader = New-Object Windows.Storage.Streams.DataReader($input)
        try {
            $size = [uint32]$stream.Size
            [void](Await-Result ($reader.LoadAsync($size)) ([uint32]))
            $bytes = New-Object byte[] $size
            $reader.ReadBytes($bytes)
            $outPath = Join-Path $renderDir ('page-{0:D2}.png' -f ($i + 1))
            [System.IO.File]::WriteAllBytes($outPath, $bytes)
        } finally {
            $reader.Dispose()
            $input.Dispose()
        }
    } finally {
        $stream.Dispose()
        $page.Dispose()
    }
}

Write-Output ("Rendered {0} pages to {1}" -f $pdf.PageCount, $renderDir)
