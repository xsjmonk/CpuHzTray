& { `
	$ErrorActionPreference='Stop'; `
	$FontPath='r:\cpu\fonts\skinny-cat\Skinny Cat.ttf'; `
	$ScriptDir=$PSScriptRoot; `
	if([string]::IsNullOrWhiteSpace($ScriptDir)){ throw 'PSScriptRoot is empty. Script must be run from a file.'; }; `
	$TargetPath=Join-Path $ScriptDir 'Fonts\embedded.ttf'; `
	if(-not (Test-Path -LiteralPath $FontPath)){ throw "Font file not found: $FontPath"; }; `
	$targetDir=Split-Path -Path $TargetPath -Parent; `
	if(-not (Test-Path -LiteralPath $targetDir)){ New-Item -ItemType Directory -Path $targetDir -Force | Out-Null; }; `
	Copy-Item -LiteralPath $FontPath -Destination $TargetPath -Force; `
	Write-Host "Font copied to: $TargetPath"; `
	Add-Type -AssemblyName System.Drawing; `
	$pfc=$null; `
	try { `
		$pfc=New-Object System.Drawing.Text.PrivateFontCollection; `
		$pfc.AddFontFile($TargetPath); `
		if($pfc.Families.Count -eq 0){ throw "No font families found in file."; }; `
		$pfc.Families | ForEach-Object { [pscustomobject]@{ FontFile=$TargetPath; FamilyName=$_.Name } } | Format-Table -AutoSize; `
	} finally { `
		if($pfc){ $pfc.Dispose(); }; `
		$pfc=$null; `
		[GC]::Collect(); `
		[GC]::WaitForPendingFinalizers(); `
		[GC]::Collect(); `
	} `
}
