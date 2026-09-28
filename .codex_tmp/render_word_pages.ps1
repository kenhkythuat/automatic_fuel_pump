$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$docx = Join-Path $root 'docs\MQTT_Server_Communication_Flow.docx'
$renderDir = Join-Path $root '.codex_tmp\rendered_word'
$xps = Join-Path $renderDir 'MQTT_Server_Communication_Flow.xps'

New-Item -ItemType Directory -Force -Path $renderDir | Out-Null
Get-ChildItem -LiteralPath $renderDir -Filter 'page-*.png' -ErrorAction SilentlyContinue | Remove-Item -Force

$word = New-Object -ComObject Word.Application
$word.Visible = $false
$word.DisplayAlerts = 0
$doc = $word.Documents.Open($docx, $false, $true)
try {
    $doc.ExportAsFixedFormat($xps, 1)
} finally {
    $doc.Close($false)
    $word.Quit()
    [System.Runtime.InteropServices.Marshal]::ReleaseComObject($doc) | Out-Null
    [System.Runtime.InteropServices.Marshal]::ReleaseComObject($word) | Out-Null
}

Add-Type -AssemblyName WindowsBase
Add-Type -AssemblyName PresentationCore
Add-Type -AssemblyName PresentationFramework
Add-Type -AssemblyName ReachFramework

$xpsDoc = New-Object System.Windows.Xps.Packaging.XpsDocument($xps, [System.IO.FileAccess]::Read)
try {
    $sequence = $xpsDoc.GetFixedDocumentSequence()
    $paginator = $sequence.DocumentPaginator
    $pageCount = $paginator.PageCount
    for ($i = 0; $i -lt $pageCount; $i++) {
        $page = $paginator.GetPage($i)
        $width = [Math]::Ceiling($page.Size.Width)
        $height = [Math]::Ceiling($page.Size.Height)
        $bitmap = New-Object System.Windows.Media.Imaging.RenderTargetBitmap($width, $height, 96, 96, [System.Windows.Media.PixelFormats]::Pbgra32)
        $bitmap.Render($page.Visual)
        $encoder = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
        $encoder.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
        $path = Join-Path $renderDir ('page-{0:D2}.png' -f ($i + 1))
        $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Create)
        try { $encoder.Save($stream) } finally { $stream.Dispose() }
    }
    Write-Output ("Rendered {0} pages to {1}" -f $pageCount, $renderDir)
} finally {
    $xpsDoc.Close()
}
