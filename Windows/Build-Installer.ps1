$ErrorActionPreference = 'Stop'

$target = $null
if ($env:PAKR_APP_URL.Length -gt 2048 -or
    -not [Uri]::TryCreate($env:PAKR_APP_URL, [UriKind]::Absolute, [ref]$target) -or
    $target.Scheme -ne 'https' -or $target.UserInfo) {
    throw 'Invalid HTTPS target URL'
}
if ($env:PAKR_APP_NAME -notmatch '\A[\p{L}\p{M}\p{N} _.-]{1,60}\z' -or
    [string]::IsNullOrWhiteSpace($env:PAKR_APP_NAME)) {
    throw 'Invalid app name'
}
if ($env:PAKR_APP_ID -cnotmatch '\A[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+\z' -or $env:PAKR_APP_ID.Length -gt 100) {
    throw 'Invalid app identifier'
}
if ($env:PAKR_VERSION -notmatch '\A[0-9]{1,5}(\.[0-9]{1,5}){2}\z' -or
    ($env:PAKR_VERSION.Split('.') | Where-Object { [int]$_ -gt 65535 })) {
    throw 'Invalid version'
}

$publishArgs = @('publish', 'Windows/Pakr.Windows.csproj', '-c', 'Release', '-o', 'windows-publish')
$iconPath = Join-Path $env:RUNNER_TEMP 'Pakr.App.ico'
& python Windows/Process-Icon.py $iconPath
if ($LASTEXITCODE -ne 0) { throw 'Icon processing failed' }
$publishArgs += "-p:ApplicationIcon=$iconPath"

& dotnet @publishArgs
if ($LASTEXITCODE -ne 0) { throw "Windows app compilation failed ($LASTEXITCODE)" }

$config = [System.Xml.Linq.XElement]::new('App')
$config.SetElementValue('Url', $target.AbsoluteUri)
$config.SetElementValue('Name', $env:PAKR_APP_NAME)
$config.SetElementValue('Id', $env:PAKR_APP_ID)
$config.Save((Join-Path $PWD 'windows-publish/app-config.xml'))

$compiler = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Inno Setup 6/ISCC.exe'
& $compiler "/DPakrAppName=$($env:PAKR_APP_NAME)" "/DPakrAppId=$($env:PAKR_APP_ID)" "/DPakrVersion=$($env:PAKR_VERSION)" "/DPakrIconFile=$iconPath" "$PSScriptRoot/Setup.iss"
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed ($LASTEXITCODE)" }
