param(
    [Parameter(Mandatory)]
    [string] $PasswordFile,
    [Parameter(ValueFromRemainingArguments)]
    [string[]] $RemoteArguments
)

$ErrorActionPreference = 'Stop'
$secure = (Get-Content -LiteralPath $PasswordFile -Raw).Trim() | ConvertTo-SecureString
$credential = [PSCredential]::new('hedc', $secure)
$originalEncoding = $OutputEncoding
try {
    $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $credential.GetNetworkCredential().Password | python -u "$PSScriptRoot\remote.py" @RemoteArguments
    exit $LASTEXITCODE
} finally {
    $OutputEncoding = $originalEncoding
}
