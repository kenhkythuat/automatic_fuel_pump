$ErrorActionPreference = 'Stop'

$root = (Get-Location).Path
$sourcePath = Join-Path $root '.codex_tmp\mqtt_server_flow.md'
$outputDir = Join-Path $root 'docs'
$outputPath = Join-Path $outputDir 'MQTT_Server_Communication_Flow.docx'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$lines = [System.IO.File]::ReadAllLines($sourcePath, [System.Text.Encoding]::UTF8)

$word = New-Object -ComObject Word.Application
$word.Visible = $false
$word.DisplayAlerts = 0
$doc = $word.Documents.Add()

try {
    $section = $doc.Sections.Item(1)
    $section.PageSetup.PaperSize = 2
    $section.PageSetup.TopMargin = $word.CentimetersToPoints(1.8)
    $section.PageSetup.BottomMargin = $word.CentimetersToPoints(1.7)
    $section.PageSetup.LeftMargin = $word.CentimetersToPoints(2.0)
    $section.PageSetup.RightMargin = $word.CentimetersToPoints(1.8)

    $normal = $doc.Styles.Item('Normal')
    $normal.Font.Name = 'Aptos'
    $normal.Font.Size = 10.5
    $normal.Font.Color = 0
    $normal.ParagraphFormat.SpaceAfter = 6
    $normal.ParagraphFormat.LineSpacingRule = 0

    $titleStyle = $doc.Styles.Item('Title')
    $titleStyle.Font.Name = 'Aptos Display'
    $titleStyle.Font.Size = 25
    $titleStyle.Font.Bold = $true
    $titleStyle.Font.Color = 0
    $titleStyle.ParagraphFormat.SpaceAfter = 14

    foreach ($styleName in @('Heading 1','Heading 2','Heading 3')) {
        $st = $doc.Styles.Item($styleName)
        $st.Font.Name = 'Aptos Display'
        $st.Font.Bold = $true
        $st.Font.Color = 0x703000
        $st.ParagraphFormat.KeepWithNext = $true
        $st.ParagraphFormat.SpaceBefore = 10
        $st.ParagraphFormat.SpaceAfter = 5
    }
    $doc.Styles.Item('Heading 1').Font.Size = 16
    $doc.Styles.Item('Heading 2').Font.Size = 13
    $doc.Styles.Item('Heading 3').Font.Size = 11

    $header = $section.Headers.Item(1).Range
    $header.Text = 'FIRMWARE ESP32-S3  |  MQTT PROTOCOL REVIEW'
    $header.Font.Name = 'Aptos'
    $header.Font.Size = 8
    $header.Font.Color = 0x777777
    $header.ParagraphFormat.Alignment = 2

    $footer = $section.Footers.Item(1).Range
    $footer.ParagraphFormat.Alignment = 1
    $footer.Font.Name = 'Aptos'
    $footer.Font.Size = 8
    $footer.Text = 'Firmware source review  |  Page '
    $footer.Collapse(0)
    $footer.Fields.Add($footer, 33) | Out-Null

    function Add-Paragraph([string]$text, [string]$style = 'Normal') {
        $p = $doc.Paragraphs.Add()
        $p.Range.Text = $text.Replace('`', '')
        $p.Range.Style = $style
        return $p
    }

    function Add-Code([string[]]$codeLines) {
        $text = [string]::Join("`v", $codeLines)
        $p = Add-Paragraph $text 'Normal'
        $p.Range.Font.Name = 'Consolas'
        $p.Range.Font.Size = 8.5
        $p.Range.Shading.BackgroundPatternColor = 0xF2F2F2
        $p.Range.ParagraphFormat.LeftIndent = $word.CentimetersToPoints(0.35)
        $p.Range.ParagraphFormat.RightIndent = $word.CentimetersToPoints(0.2)
        $p.Range.ParagraphFormat.SpaceBefore = 4
        $p.Range.ParagraphFormat.SpaceAfter = 7
        $p.Range.ParagraphFormat.KeepTogether = $true
    }

    function Add-Table([System.Collections.Generic.List[object]]$rows) {
        if ($rows.Count -lt 2) { return }
        $dataRows = New-Object 'System.Collections.Generic.List[object]'
        for ($ri = 0; $ri -lt $rows.Count; $ri++) {
            if ($ri -eq 1) { continue }
            $dataRows.Add($rows[$ri])
        }
        $cols = $dataRows[0].Count
        $range = $doc.Range($doc.Content.End - 1, $doc.Content.End - 1)
        $table = $doc.Tables.Add($range, $dataRows.Count, $cols)
        $table.Borders.Enable = 1
        $table.AllowAutoFit = $true
        $table.AutoFitBehavior(1)
        $table.Rows.Item(1).HeadingFormat = $true
        for ($r = 0; $r -lt $dataRows.Count; $r++) {
            for ($c = 0; $c -lt $cols; $c++) {
                $cell = $table.Cell($r + 1, $c + 1)
                $cell.Range.Text = ([string]$dataRows[$r][$c]).Replace('`', '')
                $cell.Range.Font.Name = 'Aptos'
                $cell.Range.Font.Size = 8.2
                $cell.VerticalAlignment = 1
                $cell.TopPadding = 3
                $cell.BottomPadding = 3
                $cell.LeftPadding = 5
                $cell.RightPadding = 5
                if ($r -eq 0) {
                    $cell.Range.Font.Bold = $true
                    $cell.Range.Font.Color = 0xFFFFFF
                    $cell.Shading.BackgroundPatternColor = 0x703000
                } elseif (($r % 2) -eq 0) {
                    $cell.Shading.BackgroundPatternColor = 0xF7F3EF
                }
            }
        }
        $after = $doc.Paragraphs.Add()
        $after.Range.Text = ''
        $after.Range.ParagraphFormat.SpaceAfter = 3
    }

    $inCode = $false
    $codeLines = New-Object 'System.Collections.Generic.List[string]'
    $i = 0
    while ($i -lt $lines.Length) {
        $line = $lines[$i]

        if ($line -eq '---PAGE---') {
            $range = $doc.Range($doc.Content.End - 1, $doc.Content.End - 1)
            $range.InsertBreak(7)
            $i++
            continue
        }

        if ($line.StartsWith('```')) {
            if (-not $inCode) {
                $inCode = $true
                $codeLines.Clear()
            } else {
                Add-Code $codeLines.ToArray()
                $inCode = $false
            }
            $i++
            continue
        }

        if ($inCode) {
            $codeLines.Add($line)
            $i++
            continue
        }

        if ($line.StartsWith('|')) {
            $tableRows = New-Object 'System.Collections.Generic.List[object]'
            while ($i -lt $lines.Length -and $lines[$i].StartsWith('|')) {
                $parts = $lines[$i].Trim('|').Split('|') | ForEach-Object { $_.Trim() }
                $tableRows.Add(@($parts))
                $i++
            }
            Add-Table $tableRows
            continue
        }

        if ($line.StartsWith('# ')) {
            $p = Add-Paragraph $line.Substring(2) 'Title'
            $p.Alignment = 0
            $p.Range.ParagraphFormat.SpaceBefore = 24
        } elseif ($line.StartsWith('## ')) {
            Add-Paragraph $line.Substring(3) 'Heading 1' | Out-Null
        } elseif ($line.StartsWith('### ')) {
            Add-Paragraph $line.Substring(4) 'Heading 2' | Out-Null
        } elseif ($line -match '^\d+\. ') {
            $p = Add-Paragraph $line 'Normal'
            $p.Range.ParagraphFormat.LeftIndent = $word.CentimetersToPoints(0.45)
            $p.Range.ParagraphFormat.FirstLineIndent = $word.CentimetersToPoints(-0.45)
        } elseif ($line.StartsWith('- ')) {
            $p = Add-Paragraph ('- ' + $line.Substring(2)) 'Normal'
            $p.Range.ParagraphFormat.LeftIndent = $word.CentimetersToPoints(0.45)
            $p.Range.ParagraphFormat.FirstLineIndent = $word.CentimetersToPoints(-0.3)
        } elseif ([string]::IsNullOrWhiteSpace($line)) {
            # Keep source spacing compact; Word paragraph styles supply spacing.
        } else {
            Add-Paragraph $line 'Normal' | Out-Null
        }
        $i++
    }

    $doc.SaveAs2($outputPath, 16)
    $doc.Close($false)
    $word.Quit()
    Write-Output $outputPath
} catch {
    try { $doc.Close($false) } catch {}
    try { $word.Quit() } catch {}
    throw
} finally {
    if ($doc -ne $null) { [System.Runtime.InteropServices.Marshal]::ReleaseComObject($doc) | Out-Null }
    if ($word -ne $null) { [System.Runtime.InteropServices.Marshal]::ReleaseComObject($word) | Out-Null }
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}
