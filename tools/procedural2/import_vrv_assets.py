"""Prepare a flattened source tree exported by osgearth_procedural2 --asset-mesh.

Retains source geometry, albedo compression, and alpha. Resolves VRV PBR sidecars,
decodes their XY normal encoding, and bakes new individual and compact-stand canopy
proxies using the same cutouts and material maps. Original inputs are unchanged.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import numpy as np
from PIL import Image
from generate_pbr_assets import Part, bake, export_material, proxy_parts, write_model


def source_image(path):
    """Match OSG's default unflipped DDS upload, expressed as a top-down Pillow raster for baking."""
    return Image.open(path).convert('RGBA').transpose(Image.Transpose.FLIP_TOP_BOTTOM)


def sidecar(path, suffix):
    """Locate both VRV's original and wrapper-derived sidecar names; fail on missing PBR data."""
    stem = path.stem
    for extension in ('_png', '_jpg', '_rgb', '_rgba', '_tga'):
        if stem.endswith(extension):
            candidate = path.with_name(stem[:-len(extension)] + suffix + extension + '.dds')
            if candidate.exists():
                return candidate
    candidate = path.with_name(stem + suffix + '.dds')
    if not candidate.exists():
        raise FileNotFoundError(candidate)
    return candidate


def prepare_material(path):
    """Decode VRV normals and MTL/GLS/AO to stock RGB normals and linear DRAM without baking lighting."""
    normal_path, packed_path = sidecar(path, '_NML'), sidecar(path, '_MTL_GLS_AO')
    albedo = source_image(path)
    raw = np.asarray(source_image(normal_path)).astype(float) / 255.0
    fourcc = normal_path.read_bytes()[84:88]
    # VRV samples RA after swizzling RGTC2; Pillow exposes BC5's original R/G channels.
    y_channel = 1 if fourcc in (b'ATI2', b'BC5U', b'BC5S') else 3
    xy = raw[:, :, [0, y_channel]] * 2 - 1
    z = np.sqrt(np.maximum(0, 1 - (xy*xy).sum(axis=2)))
    xyz = np.dstack((xy, z))
    xyz /= np.maximum(np.linalg.norm(xyz, axis=2, keepdims=True), 1e-8)
    normal = np.full(raw.shape, 255, dtype=np.uint8)
    normal[:, :, :3] = np.uint8(np.clip((xyz+1)*127.5, 0, 255))
    packed = np.asarray(source_image(packed_path))
    dram = np.zeros_like(packed)
    dram[:, :, 1] = 255-packed[:, :, 1]
    dram[:, :, 2] = packed[:, :, 2]
    dram[:, :, 3] = packed[:, :, 0]
    return (albedo, Image.fromarray(normal), Image.fromarray(dram)), [path, normal_path, packed_path]


def main():
    """Build all three representations and a provenance manifest into an explicit destination directory."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mesh', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--name', required=True)
    parser.add_argument('--size', type=int, default=512)
    parser.add_argument('--canopy-only', action='store_true', help='Rebuild shared canopy art without touching individual assets')
    args = parser.parse_args()
    report = json.loads(args.mesh.read_text())
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    parts, materials, sources = {}, {}, {}
    for record in report['materials']:
        path = Path(record['textures'][0]['file'])
        name = f"material-{record['id']}"
        maps, inputs = prepare_material(path)
        if not args.canopy_only:
            export_material(output, name, maps)
            # Keep source albedo bytes (including authored mips/alpha) exactly as delivered.
            shutil.copyfile(path, output/f'{name}-albedo.dds')
        materials[name] = tuple(image.resize(maps[0].size) for image in maps)
        part = Part(name)
        part.repeat = True
        parts[record['id']] = part
        for file in inputs:
            sources[str(file)] = hashlib.sha256(file.read_bytes()).hexdigest()
    vertices = report['mesh']
    for i in range(0, len(report['indices']), 3):
        tri = [vertices[j] for j in report['indices'][i:i+3]]
        assert len({v['material'] for v in tri}) == 1
        part = parts[tri[0]['material']]
        part.v.extend([v['position'] for v in tri])
        part.n.extend([v['normal'] for v in tri])
        part.uv.extend([v['uv'] for v in tri])
        part.c.extend([v['color'][:3] for v in tri])
    parts = list(parts.values())
    if not args.canopy_only:
        write_model(output/f'{args.name}-near.osg', parts)
        maps, low, high = bake(parts, materials, args.size)
        export_material(output, f'{args.name}-proxy', maps)
        write_model(output/f'{args.name}-coarse.osg', proxy_parts(low, high, args.size, f'{args.name}-proxy'))
        print(f'{args.name}: {report["triangles"]} near triangles; 6 impostor triangles', flush=True)
    else:
        vertices = np.concatenate([p.v for p in parts])
        low, high = vertices.min(0)-0.04, vertices.max(0)+0.04
    cluster_parts = []
    # A reusable far clump represents a closed stand, not five isolated silhouettes with leaf-sized gaps.
    # Pack more source trees offline; runtime geometry stays six triangles with the same atlas dimensions.
    spacing = max(high[0]-low[0], high[1]-low[1]) * 0.24
    for original in parts:
        merged = Part(original.material)
        merged.repeat = original.repeat
        for i in range(13):
            angle, radius = i*2.399963, np.sqrt(i)*spacing
            merged.append(original, (np.cos(angle)*radius, np.sin(angle)*radius, 0), 0.88+0.04*(i % 5), angle)
        cluster_parts.append(merged)
    maps, cluster_low, cluster_high = bake(cluster_parts, materials, args.size)
    export_material(output, f'{args.name}-canopy', maps)
    write_model(output/f'{args.name}-canopy.osg',
                proxy_parts(cluster_low, cluster_high, args.size, f'{args.name}-canopy'))
    manifest = dict(name=args.name, source_mesh=report['source'], source_hashes=sources,
                    near_triangles=report['triangles'], coarse_triangles=6, canopy_triangles=6,
                    bounds_m=[low.tolist(), high.tolist()], cluster_bounds_m=[cluster_low.tolist(), cluster_high.tolist()],
                    canopy_recipe=dict(trees=13, spacing_diameters=0.24),
                    source_layout='MTL_GLS_AO', runtime_layout='DRAM',
                    normal_conversion='VRV tangent XY to RGB, positive reconstructed Z',
                    proxy_normals='six views; source geometry and tangent normal maps baked into planar card frames',
                    dds_bytes=sum(p.stat().st_size for p in output.glob('*.dds')))
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(f'{args.name}: source-derived compact 13-tree canopy -> 6 triangles', flush=True)


if __name__ == '__main__':
    main()
