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

$iconPath = Join-Path $env:RUNNER_TEMP 'Pakr.App.ico'
& python Windows/Process-Icon.py $iconPath
if ($LASTEXITCODE -ne 0) { throw 'Icon processing failed' }

$config = [System.Xml.XmlDocument]::new()
$app = $config.AppendChild($config.CreateElement('App'))
$app.AppendChild($config.CreateElement('Url')).InnerText = $target.AbsoluteUri
$app.AppendChild($config.CreateElement('Name')).InnerText = $env:PAKR_APP_NAME
$app.AppendChild($config.CreateElement('Id')).InnerText = $env:PAKR_APP_ID
$configPath = Join-Path $env:RUNNER_TEMP 'Pakr.AppConfig.xml'
$config.Save($configPath)

$publishArgs = @('publish', 'Windows/Pakr.Windows.csproj', '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true',
    '-o', 'windows-publish', '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true',
    '-p:IncludeAllContentForSelfExtract=true', '-p:EnableCompressionInSingleFile=true',
    "-p:ApplicationIcon=$iconPath", "-p:PakrConfigFile=$configPath", "-p:Version=$($env:PAKR_VERSION)")
& dotnet @publishArgs
if ($LASTEXITCODE -ne 0) { throw "Windows app compilation failed ($LASTEXITCODE)" }

# The artifact prefix distinguishes portable builds from historical installers.
$fileName = "Pakr-Portable-$($env:PAKR_APP_NAME.Trim()).exe"
New-Item -ItemType Directory -Path 'windows-artifact' -Force | Out-Null
Copy-Item -LiteralPath 'windows-publish/Pakr.Windows.exe' -Destination (Join-Path 'windows-artifact' $fileName)
