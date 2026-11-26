# ASCII-only: walk RLE stream, list runs outside expected tile rects (sel=0 layout)
# Expected sel=0: Camera DE33 (118..202,78..162) s84 | Album A0FC (206..266,90..150) s60
# Setting 728C (270..314,98..142) s44 | Game C6F9 (6..50,98..142) s44 | Notes 60FE (54..114,90..150) s60
# Shadow 18C6 (122..205,82..165) | white elsewhere
$text = [IO.File]::ReadAllText("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_text.txt")
$s = $text.IndexOf("B64 ")
$b64Start = $text.IndexOf("`n", $s) + 1
$b64End = $text.IndexOf("ENDB64")
$b64 = $text.Substring($b64Start, $b64End - $b64Start).Replace("`r","").Replace("`n","").Trim()
$rle = [Convert]::FromBase64String($b64)

function TileOk($c, $x, $y) {
    if ($c -eq 0xFFFF) { return $true }
    if ($c -eq 0xDE33) { return ($x -ge 118 -and $x -le 201 -and $y -ge 78 -and $y -le 161) }
    if ($c -eq 0xA0FC) { return ($x -ge 206 -and $x -le 265 -and $y -ge 90 -and $y -le 149) }
    if ($c -eq 0x60FE) { return ($x -ge 54 -and $x -le 113 -and $y -ge 90 -and $y -le 149) }
    if ($c -eq 0x728C) { return ($x -ge 270 -and $x -le 313 -and $y -ge 98 -and $y -le 141) }
    if ($c -eq 0xC6F9) { return ($x -ge 6 -and $x -le 49 -and $y -ge 98 -and $y -le 141) }
    if ($c -eq 0x18C6) { return ($x -ge 122 -and $x -le 204 -and $y -ge 82 -and $y -le 164) }
    return $false   # any other color is unexpected
}

$px = 0; $pi = 0; $shown = 0
while ($px -lt 76800 -and ($pi + 4) -le $rle.Length) {
    $c = [int]$rle[$pi] + 256 * [int]$rle[$pi+1]
    $r = [int]$rle[$pi+2] + 256 * [int]$rle[$pi+3]
    if ($r -le 0) { Write-Output ("BAD run 0 at px " + $px); break }
    $x = $px % 320; $y = [int]($px / 320)
    $xEnd = ($px + $r - 1) % 320
    $ok = TileOk $c $x $y
    if ($c -ne 0xFFFF -and -not ($ok -and ($xEnd -ge $x))) {
        $sameRow = ([int](($px + $r - 1) / 320)) -eq $y
        if ($shown -lt 40) {
            Write-Output ("STRAY color=0x{0:X4} x={1}..{2} y={3} run={4} sameRow={5}" -f $c, $x, $xEnd, $y, $r, $sameRow)
            $shown++
        }
    }
    $px += $r; $pi += 4
}
Write-Output ("done px=" + $px)
