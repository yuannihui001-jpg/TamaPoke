param(
  [string]$Root = $PSScriptRoot,
  [int]$Port = 8000
)

$rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()

$mimeTypes = @{
  '.html' = 'text/html; charset=utf-8'
  '.json' = 'application/json; charset=utf-8'
  '.js'   = 'text/javascript; charset=utf-8'
  '.css'  = 'text/css; charset=utf-8'
  '.bin'  = 'application/octet-stream'
  '.png'  = 'image/png'
  '.svg'  = 'image/svg+xml'
  '.ico'  = 'image/x-icon'
}

try {
  while ($listener.IsListening) {
    $context = $listener.GetContext()
    try {
      $relative = [Uri]::UnescapeDataString($context.Request.Url.AbsolutePath.TrimStart('/'))
      if ([string]::IsNullOrWhiteSpace($relative)) { $relative = 'index.html' }
      $relative = $relative.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
      $filePath = [System.IO.Path]::GetFullPath((Join-Path $rootPath $relative))

      if (-not $filePath.StartsWith($rootPath, [StringComparison]::OrdinalIgnoreCase) -or
          -not [System.IO.File]::Exists($filePath)) {
        $context.Response.StatusCode = 404
        $context.Response.Close()
        continue
      }

      $bytes = [System.IO.File]::ReadAllBytes($filePath)
      $extension = [System.IO.Path]::GetExtension($filePath).ToLowerInvariant()
      $context.Response.StatusCode = 200
      $context.Response.ContentType = if ($mimeTypes.ContainsKey($extension)) { $mimeTypes[$extension] } else { 'application/octet-stream' }
      $context.Response.ContentLength64 = $bytes.Length
      $context.Response.Headers['Cache-Control'] = 'no-store'
      if ($context.Request.HttpMethod -ne 'HEAD') {
        $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
      }
      $context.Response.Close()
    } catch {
      try {
        $context.Response.StatusCode = 500
        $context.Response.Close()
      } catch {}
    }
  }
} finally {
  $listener.Stop()
  $listener.Close()
}
