<?
echo '<center>';
if($corec3 == 'true'){require "core.c";}else{echo "<font color=\"blue\">O arquivo core.c n�o existe</font><br>";}
if($grfioc3 == 'true'){require "grfio.c";}else{echo "<font color=\"blue\">O arquivo grfio.c n�o existe</font><br>";}

// Converter Changelog-Stable.txt para Changelog-SVN.txt ==========================
$CCV = file_get_contents('src/common/Makefile');
$CCV = str_replace('Changelog-Stable.txt','Changelog-SVN.txt',$CCV);
$fp = fopen('src/common/Makefile', 'w');
if (!fwrite($fp, $CCV)) {
echo "<font color=\"red\">O arquivo common/Makefile n�o foi convertido</font><br>";
}
echo "<font color=\"green\">O arquivo common/Makefile convertido</font><br>";
fclose($fp);
// ================================================================================

echo '</center>';
?>