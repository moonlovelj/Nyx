<#
Count triangles in glTF/GLB:
- RawTriangles: sum of triangles in meshes (geometry only, no instancing)
- InstancedTriangles: sum over scene nodes that reference meshes (including EXT_mesh_gpu_instancing count)

Usage:
  .\Count-GltfTriangles.ps1 -Path "scene.gltf"
  .\Count-GltfTriangles.ps1 -Path "scene.glb" -Detailed
#>

param(
  [Parameter(Mandatory = $true)]
  [string]$Path,

  [switch]$Detailed
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Has-Property {
  param($Obj, [string]$Name)
  return $null -ne $Obj -and ($Obj.PSObject.Properties.Name -contains $Name)
}

function To-Array {
  param($Obj)
  if ($null -eq $Obj) { return @() }
  return @($Obj)
}

function Read-UInt32LE([byte[]]$Bytes, [int]$Offset) {
  return [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Get-GltfJsonText {
  param([string]$FilePath)

  $ext = [IO.Path]::GetExtension($FilePath).ToLowerInvariant()
  if ($ext -eq ".gltf") {
    return Get-Content -LiteralPath $FilePath -Raw -Encoding UTF8
  }
  elseif ($ext -eq ".glb") {
    $bytes = [IO.File]::ReadAllBytes($FilePath)

    if ($bytes.Length -lt 12) { throw "Invalid GLB: too small." }

    $magic = Read-UInt32LE $bytes 0
    if ($magic -ne 0x46546C67) { throw "Invalid GLB: bad magic." } # 'glTF'
    $version = Read-UInt32LE $bytes 4
    if ($version -ne 2) { throw "Unsupported GLB version: $version (only glTF 2.0 supported)." }

    $totalLength = Read-UInt32LE $bytes 8
    if ($totalLength -gt $bytes.Length) { throw "Invalid GLB: length field larger than file." }

    $offset = 12
    $jsonText = $null

    while ($offset + 8 -le $bytes.Length) {
      $chunkLen  = Read-UInt32LE $bytes $offset
      $chunkType = Read-UInt32LE $bytes ($offset + 4)
      $offset += 8

      if ($offset + $chunkLen -gt $bytes.Length) { throw "Invalid GLB: chunk exceeds file size." }

      if ($chunkType -eq 0x4E4F534A) { # 'JSON'
        $jsonBytes = $bytes[$offset..($offset + $chunkLen - 1)]
        $jsonText = [Text.Encoding]::UTF8.GetString($jsonBytes).Trim([char]0)
        break
      }

      $offset += $chunkLen
    }

    if (-not $jsonText) { throw "Invalid GLB: JSON chunk not found." }
    return $jsonText
  }
  else {
    throw "Unsupported file extension: $ext (use .gltf or .glb)"
  }
}

function Get-JsonRoot {
  param([string]$JsonText)

  # Use System.Text.Json for speed/low overhead
  try {
    return $JsonText | ConvertFrom-Json -Depth 200
  } catch {
    throw "Failed to parse JSON: $($_.Exception.Message)"
  }
}

function Get-AccessorCount {
  param($Gltf, [int]$AccessorIndex)

  if (-not (Has-Property $Gltf "accessors")) { return $null }
  if ($AccessorIndex -lt 0 -or $AccessorIndex -ge $Gltf.accessors.Count) { return $null }
  if (-not (Has-Property $Gltf.accessors[$AccessorIndex] "count")) { return $null }
  return [int]$Gltf.accessors[$AccessorIndex].count
}

function Get-PrimitiveVertexCount {
  param($Gltf, $Prim)

  # Prefer indices accessor count if present (index count)
  if (Has-Property $Prim "indices") {
    $idxCount = Get-AccessorCount $Gltf ([int]$Prim.indices)
    return $idxCount
  }

  # Non-indexed: use POSITION accessor count
  if (Has-Property $Prim "attributes" -and (Has-Property $Prim.attributes "POSITION")) {
    $vCount = Get-AccessorCount $Gltf ([int]$Prim.attributes.POSITION)
    return $vCount
  }

  return $null
}

function Get-PrimitiveTriangleCount {
  param($Gltf, $Prim)

  # glTF primitive mode default is 4 (TRIANGLES)
  $mode = 4
  if (Has-Property $Prim "mode") { $mode = [int]$Prim.mode }

  $count = Get-PrimitiveVertexCount $Gltf $Prim
  if ($null -eq $count) { return 0 }

  switch ($mode) {
    4 { # TRIANGLES
      return [int]([Math]::Floor($count / 3))
    }
    5 { # TRIANGLE_STRIP
      return [int]([Math]::Max(0, $count - 2))
    }
    6 { # TRIANGLE_FAN
      return [int]([Math]::Max(0, $count - 2))
    }
    default {
      # Points/Lines/etc => 0 triangles
      return 0
    }
  }
}

function Build-MeshTriangleTable {
  param($Gltf)

  $meshes = To-Array $Gltf.meshes
  $meshTri = @()
  for ($m = 0; $m -lt $meshes.Count; $m++) {
    $mesh = $meshes[$m]
    $prims = To-Array $mesh.primitives
    $sum = 0
    for ($p = 0; $p -lt $prims.Count; $p++) {
      $sum += Get-PrimitiveTriangleCount $Gltf $prims[$p]
    }
    $meshTri += $sum
  }
  return $meshTri
}

function Get-NodeInstanceMultiplier {
  param($Gltf, $Node)

  # Default one instance
  $mult = 1

  # EXT_mesh_gpu_instancing: instance count = accessor.count of any attribute in the extension
  try {
    $ext = $Node.extensions
    if ($null -ne $ext -and $null -ne $ext.EXT_mesh_gpu_instancing) {
      $attrs = $ext.EXT_mesh_gpu_instancing.attributes
      if ($null -ne $attrs) {
        foreach ($prop in $attrs.PSObject.Properties) {
          $accIndex = [int]$prop.Value
          $cnt = Get-AccessorCount $Gltf $accIndex
          if ($null -ne $cnt -and $cnt -gt 0) {
            $mult = $cnt
            break
          }
        }
      }
    }
  } catch {
    # ignore extension parsing errors, keep mult=1
  }

  return $mult
}

function Traverse-SceneNodes {
  param(
    $Gltf,
    [int]$SceneIndex
  )

  $nodes = To-Array $Gltf.nodes
  if ($nodes.Count -eq 0) {
    return @()
  }

  if (-not $Gltf.PSObject.Properties.Name.Contains("scenes")) {
    return @(0..($nodes.Count - 1))
  }

  $scenes = To-Array $Gltf.scenes
  if ($SceneIndex -lt 0 -or $SceneIndex -ge $scenes.Count) {
    $SceneIndex = 0
  }

  $scene = $scenes[$SceneIndex]
  if (-not $scene.PSObject.Properties.Name.Contains("nodes")) {
    return @()
  }

  $roots = @($scene.nodes | ForEach-Object { [int]$_ })

  $visited = New-Object 'System.Collections.Generic.HashSet[int]'
  $stack = New-Object 'System.Collections.Generic.Stack[int]'
  foreach ($r in $roots) { $stack.Push($r) }

  $reachable = New-Object 'System.Collections.Generic.List[int]'

  while ($stack.Count -gt 0) {
    $n = $stack.Pop()
    if ($visited.Add($n)) {
      $reachable.Add($n)

      if ($n -lt 0 -or $n -ge $Gltf.nodes.Count) { continue }
      $node = $Gltf.nodes[$n]
      if ($node.PSObject.Properties.Name -contains "children") {
        foreach ($c in $node.children) {
          $stack.Push([int]$c)
        }
      }
    }
  }

  return @($reachable.ToArray())
}


# ---------------- main ----------------

$jsonText = Get-GltfJsonText -FilePath $Path
$gltf = Get-JsonRoot -JsonText $jsonText

$meshes = To-Array $gltf.meshes
if ($meshes.Count -eq 0) {
  Write-Host "No meshes found. RawTriangles=0, InstancedTriangles=0"
  exit 0
}

$meshTriTable = @(Build-MeshTriangleTable $gltf)
$rawTriangles = ($meshTriTable | Measure-Object -Sum).Sum
$rawTriangles = [int]$rawTriangles

# Choose default scene if present
$sceneIndex = 0
if (Has-Property $gltf "scene") { $sceneIndex = [int]$gltf.scene }

$reachableNodes = @()
if ((To-Array $gltf.nodes).Count -gt 0) {
  $reachableNodes = @(Traverse-SceneNodes -Gltf $gltf -SceneIndex $sceneIndex)
}

$instancedTriangles = 0L
$nodes = To-Array $gltf.nodes

if ($reachableNodes.Count -eq 0) {
  # No nodes: instanced triangles equals raw? In practice, nothing is placed => 0
  $instancedTriangles = 0
} else {
  foreach ($ni in $reachableNodes) {
    if ($ni -lt 0 -or $ni -ge $nodes.Count) { continue }
    $node = $nodes[$ni]
    if (Has-Property $node "mesh") {
      $meshIndex = [int]$node.mesh
      if ($meshIndex -ge 0 -and $meshIndex -lt $meshTriTable.Count) {
        $mult = Get-NodeInstanceMultiplier -Gltf $gltf -Node $node
        $instancedTriangles += [int64]$meshTriTable[$meshIndex] * [int64]$mult
      }
    }
  }
}

Write-Host "File: $Path"
Write-Host "DefaultSceneIndex: $sceneIndex"
Write-Host "Meshes: $($meshes.Count) | Nodes(reachable): $($reachableNodes.Count)"
Write-Host "RawTriangles (geometry only): $rawTriangles"
Write-Host "InstancedTriangles (scene * node instancing): $instancedTriangles"

if ($Detailed) {
  Write-Host ""
  Write-Host "Per-mesh triangles:"
  for ($m = 0; $m -lt $meshTriTable.Count; $m++) {
    $name = $null
    if (Has-Property $meshes[$m] "name") { $name = $meshes[$m].name }
    if ([string]::IsNullOrWhiteSpace($name)) { $name = "<mesh $m>" }
    Write-Host ("  [{0,4}] {1,-30} : {2}" -f $m, $name, $meshTriTable[$m])
  }

  Write-Host ""
  Write-Host "Per-node contributions (only nodes with mesh):"
  foreach ($ni in $reachableNodes) {
    if ($ni -lt 0 -or $ni -ge $nodes.Count) { continue }
    $node = $nodes[$ni]
    if (Has-Property $node "mesh") {
      $meshIndex = [int]$node.mesh
      $nodeName = $null
      if (Has-Property $node "name") { $nodeName = $node.name }
      if ([string]::IsNullOrWhiteSpace($nodeName)) { $nodeName = "<node $ni>" }
      $mult = Get-NodeInstanceMultiplier -Gltf $gltf -Node $node
      $tri = 0
      if ($meshIndex -ge 0 -and $meshIndex -lt $meshTriTable.Count) { $tri = $meshTriTable[$meshIndex] }
      $contrib = [int64]$tri * [int64]$mult
      Write-Host ("  [{0,4}] {1,-30} mesh={2,4} tri={3,10} inst={4,6} => {5}" -f $ni, $nodeName, $meshIndex, $tri, $mult, $contrib)
    }
  }
}
