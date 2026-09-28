"""Generate original MIT-licensed, meter-scale Z-up starter models and three-view impostors.

Run with Python 3, numpy and Pillow with BCn encoding support. No downloaded art or training assets are used.
Output goes to data/procedural2/starter by default; generation is deterministic.
"""
from pathlib import Path
import argparse
import io
import json
import math
import random
import struct
import numpy as np
from PIL import Image, ImageFilter


class Mesh:
    """Accumulate immutable, colored triangles for OSG export and albedo-only baking."""

    def __init__(self):
        """Start an empty triangle soup; vertices carry flat geometric normals."""
        self.v, self.n, self.c = [], [], []

    def tri(self, a, b, c, color):
        """Append a nondegenerate triangle, keeping generated normals normalized."""
        a, b, c = map(np.asarray, (a, b, c))
        n = np.cross(b - a, c - a)
        length = np.linalg.norm(n)
        if length < 1e-9:
            return
        self.v.extend((a, b, c))
        self.n.extend((n / length,) * 3)
        self.c.extend((color,) * 3)

    def branch(self, a, b, radius, color, sides=6):
        """Make a tapered woody branch with a stable orthonormal cross section."""
        a, b = np.asarray(a), np.asarray(b)
        axis = b - a
        axis /= np.linalg.norm(axis)
        side = np.cross(axis, (0, 1, 0))
        side /= np.linalg.norm(side)
        other = np.cross(axis, side)
        for i in range(sides):
            t, u = np.array((i, i + 1)) * 2 * math.pi / sides
            p = radius * (side * math.cos(t) + other * math.sin(t))
            q = radius * (side * math.cos(u) + other * math.sin(u))
            shade = tuple(k * (0.86 + 0.14 * math.sin(t) ** 2) for k in color)
            self.tri(a + p, a + q, b + q * 0.35, shade)
            self.tri(a + p, b + q * 0.35, b + p * 0.35, shade)

    def leaf(self, center, angle, length, width, tilt, color):
        """Make a folded four-triangle leaf with a raised midrib and a pointed tip."""
        center = np.asarray(center)
        axis = np.array((math.cos(angle), math.sin(angle), tilt))
        side = np.array((-math.sin(angle), math.cos(angle), 0)) * width
        root, tip = center - axis * length * 0.5, center + axis * length * 0.5
        ridge = center + (0, 0, width * 0.22)
        self.tri(root, center - side, ridge, color)
        self.tri(center - side, tip, ridge, color)
        self.tri(root, ridge, center + side, tuple(k * 0.91 for k in color))
        self.tri(ridge, tip, center + side, tuple(k * 0.91 for k in color))

    def append(self, other, offset=(0, 0, 0), scale=1.0, angle=0.0):
        """Merge one original mesh into a synthetic canopy cluster without external dependencies."""
        rotation = np.array(((math.cos(angle), -math.sin(angle), 0),
                             (math.sin(angle), math.cos(angle), 0), (0, 0, 1)))
        self.v.extend(np.asarray(other.v) @ rotation.T * scale + offset)
        self.n.extend(np.asarray(other.n) @ rotation.T)
        self.c.extend(other.c)


def broadleaf(seed=83, shrub=False):
    """Create an irregular branching crown with individually folded leaves; shrub uses several short stems."""
    rng, mesh = random.Random(seed), Mesh()
    height = 1.5 if shrub else 11.5
    wood = (0.30, 0.23, 0.16)
    mesh.branch((0, 0, -0.08), (0.22, -0.13, height * 0.87), height * 0.025, wood, 8)
    for i in range(21 if shrub else 30):
        angle = i * 2.399963 + rng.uniform(-0.35, 0.35)
        z = height * (0.42 + 0.46 * i / 30) if not shrub else rng.uniform(0.25, 1.15)
        radius = height * 0.36 * math.sqrt(max(0.05, 1 - ((z/height-0.64)/0.29)**2))
        radius *= rng.uniform(0.38, 1.0)
        end = np.array((radius * math.cos(angle), radius * math.sin(angle), z + height * 0.12))
        fork = end * 0.5 + (0, 0, z * 0.32)
        mesh.branch((0, 0, z * 0.72), fork, height * 0.006, wood)
        mesh.branch(fork, end, height * 0.003, wood, 5)
        cloud = height * (0.07 if shrub else 0.095)
        for j in range(20 if shrub else 33):
            direction = np.array([rng.uniform(-1, 1) for _ in range(3)])
            center = end + direction * cloud
            color = (rng.uniform(0.23, 0.39), rng.uniform(0.38, 0.55), rng.uniform(0.10, 0.20))
            length = rng.uniform(0.18, 0.30) if shrub else rng.uniform(0.45, 0.72)
            mesh.leaf(center, rng.uniform(0, math.tau), length, length * 0.30,
                      rng.uniform(-1.4, 1.4), color)
    return mesh


def conifer():
    """Create layered whorls of drooping boughs using folded needle sprays, leaving natural crown gaps."""
    rng, mesh = random.Random(301), Mesh()
    mesh.branch((0, 0, -0.1), (0.08, 0.10, 14.2), 0.24, (0.31, 0.22, 0.14), 8)
    for tier in range(15):
        z = 2.7 + tier * 0.73
        radius = 3.3 * (1 - tier / 16) ** 0.7
        for branch in range(5):
            a = tier * 1.7 + branch * math.tau / 5
            direction = np.array((math.cos(a), math.sin(a), 0))
            end = direction * radius + (0, 0, z - 0.3)
            mesh.branch((0, 0, z), end, 0.035, (0.28, 0.22, 0.14), 4)
            for j in range(5):
                center = direction * radius * (j + 1) / 5 + (0, 0, z - 0.15)
                length = radius * 0.50 + 0.18
                color = (0.13 + rng.random() * 0.10, 0.29 + rng.random() * 0.12, 0.17 + rng.random() * 0.06)
                mesh.leaf(center, a, length, length * 0.28, -0.25, color)
                mesh.leaf(center + (0, 0, 0.1), a + rng.uniform(-0.4, 0.4), length * 0.75,
                          length * 0.23, 1.1, color)
    return mesh


def grass():
    """Build a windless asset tuft of bent ribbons, independent of the optional GPU grass path."""
    rng, mesh = random.Random(24), Mesh()
    for i in range(30):
        a, height = rng.uniform(0, math.tau), rng.uniform(0.34, 0.81)
        axis = np.array((math.cos(a), math.sin(a), 0))
        side = np.array((-axis[1], axis[0], 0)) * rng.uniform(0.008, 0.018)
        root = axis * rng.uniform(0.01, 0.17)
        for j in range(3):
            t, u = j / 3, (j + 1) / 3
            p = root + axis * height * 0.55 * t * t + (0, 0, height * t)
            q = root + axis * height * 0.55 * u * u + (0, 0, height * u)
            color = (0.36 + 0.12 * t, 0.43 + 0.10 * t, 0.12 + 0.05 * t)
            mesh.tri(p - side * (1-t), p + side * (1-t), q - side * (1-u), color)
            mesh.tri(p + side * (1-t), q + side * (1-u), q - side * (1-u), color)
    return mesh


def fern():
    """Build radial curved fronds with paired pinnae, distinct from both shrubs and grass."""
    mesh = Mesh()
    for frond in range(9):
        a = frond * 2.399963
        axis = np.array((math.cos(a), math.sin(a), 0))
        side = np.array((-axis[1], axis[0], 0))
        last = np.zeros(3)
        for j in range(1, 9):
            t = j / 9
            center = axis * t * 0.95 + (0, 0, math.sin(t * math.pi * 0.82) * 0.62)
            mesh.branch(last, center, 0.006, (0.27, 0.37, 0.09), 3)
            last = center
            length = 0.32 * math.sin(t * math.pi) + 0.035
            for sign in (-1, 1):
                leaf_axis = side * sign + axis * 0.45
                leaf_angle = math.atan2(leaf_axis[1], leaf_axis[0])
                mesh.leaf(center + leaf_axis * length * 0.43, leaf_angle, length, length * 0.18,
                          -0.15, (0.18 + t * 0.05, 0.38 + t * 0.08, 0.12))
    return mesh


def boulder():
    """Make an asymmetric triangulated stone with colored mineral and moss patches; no texture dependencies."""
    rng, mesh = random.Random(992), Mesh()
    rings = []
    for row in range(7):
        theta = (row + 0.12) / 6.25 * math.pi
        ring = []
        for col in range(13):
            phi = col * math.tau / 13
            r = 1 + rng.uniform(-0.12, 0.12)
            ring.append((math.sin(theta) * math.cos(phi) * r * 1.4,
                         math.sin(theta) * math.sin(phi) * r,
                         max(-0.15, (math.cos(theta) * r + 0.62) * 0.95)))
        rings.append(ring)
    for row in range(6):
        for col in range(13):
            a, b = rings[row][col], rings[row][(col+1) % 13]
            c, d = rings[row+1][col], rings[row+1][(col+1) % 13]
            v = rng.uniform(0.32, 0.51)
            color = (v * 0.88, v * 0.96, v * 0.89) if rng.random() > 0.18 else (0.28, 0.33, 0.16)
            mesh.tri(a, c, b, color)
            mesh.tri(b, c, d, color)
    return mesh


def write_dds(atlas, path):
    """Write bottom-up BC3 plus every mip for OSG; fail instead of silently saving uncompressed runtime art."""
    if atlas.mode != 'RGBA' or any(size < 1 or size & (size-1) for size in atlas.size):
        raise ValueError('Starter atlases must be RGBA with power-of-two dimensions')
    levels, header = [], None
    mip = atlas
    while True:
        encoded = io.BytesIO()
        # osgDB's DDS reader does not flip by default; match its PNG reader's bottom-left image data.
        try:
            mip.transpose(Image.Transpose.FLIP_TOP_BOTTOM).save(encoded, format='DDS', pixel_format='DXT5')
        except (OSError, ValueError) as error:
            raise RuntimeError('DDS export requires Pillow with BCn/DXT5 encoding support') from error
        data = encoded.getvalue()
        expected = max(1, (mip.width+3)//4) * max(1, (mip.height+3)//4) * 16
        if data[:4] != b'DDS ' or data[84:88] != b'DXT5' or len(data) != 128+expected:
            raise RuntimeError('DDS encoder did not produce the expected BC3 payload')
        if header is None:
            header = bytearray(data[:128])
        levels.append(data[128:])
        if mip.size == (1, 1):
            break
        size = (max(1, mip.width//2), max(1, mip.height//2))
        # Match ordinary GL mip averaging, retaining dilated RGB independently of alpha.
        # Chonk already compensates alpha during minification; do not apply coverage scaling twice.
        mip = Image.merge('RGBA', tuple(channel.resize(size, Image.Resampling.BOX) for channel in mip.split()))
    flags = struct.unpack_from('<I', header, 8)[0] | 0x20000  # DDSD_MIPMAPCOUNT
    struct.pack_into('<I', header, 8, flags)
    struct.pack_into('<I', header, 20, len(levels[0]))  # DDSD_LINEARSIZE: top-level compressed bytes
    struct.pack_into('<I', header, 28, len(levels))
    struct.pack_into('<I', header, 108, 0x1000 | 0x8 | 0x400000)  # TEXTURE | COMPLEX | MIPMAP
    path.write_bytes(header + b''.join(levels))


def compress_existing_atlases(output):
    """Refresh DDS files from editable PNGs and switch generated model references without rebaking geometry."""
    atlases = sorted(output.glob('*-atlas.png'))
    if not atlases:
        raise ValueError(f'No starter atlases found in {output}')
    for path in atlases:
        with Image.open(path) as image:
            write_dds(image.convert('RGBA'), path.with_suffix('.dds'))
        print(f'{path.stem}: BC3 with complete mip chain', flush=True)
    for path in output.glob('*.osg'):
        original = text = path.read_text(encoding='utf-8')
        for atlas in atlases:
            text = text.replace(f'file "{atlas.name}"', f'file "{atlas.with_suffix(".dds").name}"')
        if text != original:
            path.write_text(text, encoding='utf-8', newline='\r\n')


def write_osg(path, mesh, uv=None, texture=None, volume_normals=False):
    """Write portable OSG ASCII geometry; alpha cutout and lighting are supplied by Chonk at runtime."""
    def array(label, kind, values):
        """Serialize one OSG array with deterministic finite decimal values."""
        return f'{label} {kind} {len(values)} {{\n' + '\n'.join(
            ' '.join(f'{float(x):.6f}' for x in v) for v in values) + '\n}\n'
    state = ''
    if texture:
        state = ('StateSet { textureUnit 0 { GL_TEXTURE_2D ON Texture2D {\n'
                 f'file "{texture}"\nwrap_s CLAMP_TO_EDGE\nwrap_t CLAMP_TO_EDGE\n'
                 'min_filter LINEAR_MIPMAP_LINEAR\nmag_filter LINEAR\n'
                 'resizeNonPowerOfTwo FALSE\n} } }\n')
    text = ('Geode {\nDataVariance STATIC\n' + state + 'num_drawables 1\nGeometry {\n'
            'useDisplayList FALSE\nuseVertexBufferObjects TRUE\n'
            f'PrimitiveSets 1 {{ DrawArrays TRIANGLES 0 {len(mesh.v)} }}\n' +
            array('VertexArray', 'Vec3Array', mesh.v) + 'NormalBinding PER_VERTEX\n' +
            array('NormalArray', 'Vec3Array', mesh.n) + 'ColorBinding PER_VERTEX\n' +
            array('ColorArray', 'Vec4Array', [(*c, 1) for c in mesh.c]))
    if volume_normals:
        # Chonk normal technique 3: these describe a crown volume, not the facing of its carrier card.
        text += 'VertexAttribBinding 6 OVERALL\nVertexAttribArray 6 UByteArray 1 { 3 }\n'
    if uv is not None:
        text += array('TexCoordArray 0', 'Vec2Array', uv)
    path.write_text(text + '}\n}\n', encoding='utf-8', newline='\r\n')


def bake(mesh, size=512):
    """Rasterize unlit albedo and binary coverage from two sides and overhead; use per-pixel depth."""
    vertices, colors = np.asarray(mesh.v), np.asarray(mesh.c)
    low, high = vertices.min(axis=0) - 0.04, vertices.max(axis=0) + 0.04
    atlas = Image.new('RGBA', (size * 4, size), (70, 90, 30, 0))
    for view, (x_axis, y_axis, depth_axis) in enumerate(((0, 2, 1), (1, 2, 0), (0, 1, 2))):
        screen = (vertices[:, (x_axis, y_axis)] - low[[x_axis, y_axis]]) / (high-low)[[x_axis, y_axis]]
        screen[:, 1] = 1 - screen[:, 1]
        screen = screen * (size - 8) + 4
        result = np.zeros((size, size, 4), dtype=np.uint8)
        depth = np.full((size, size), -np.inf)
        for i in range(0, len(vertices), 3):
            p = screen[i:i+3]
            x0, y0 = np.maximum(0, np.floor(p.min(axis=0)).astype(int))
            x1, y1 = np.minimum(size-1, np.ceil(p.max(axis=0)).astype(int))
            if x1 < x0 or y1 < y0:
                continue
            xx, yy = np.meshgrid(np.arange(x0, x1+1)+0.5, np.arange(y0, y1+1)+0.5)
            den = (p[1, 1]-p[2, 1])*(p[0, 0]-p[2, 0]) + (p[2, 0]-p[1, 0])*(p[0, 1]-p[2, 1])
            if abs(den) < 1e-7:
                continue
            a = ((p[1, 1]-p[2, 1])*(xx-p[2, 0])+(p[2, 0]-p[1, 0])*(yy-p[2, 1])) / den
            b = ((p[2, 1]-p[0, 1])*(xx-p[2, 0])+(p[0, 0]-p[2, 0])*(yy-p[2, 1])) / den
            c = 1-a-b
            z = a*vertices[i, depth_axis]+b*vertices[i+1, depth_axis]+c*vertices[i+2, depth_axis]
            target = depth[y0:y1+1, x0:x1+1]
            mask = (a >= 0) & (b >= 0) & (c >= 0) & (z > target)
            target[mask] = z[mask]
            rgba = result[y0:y1+1, x0:x1+1]
            rgba[mask] = (*np.clip(colors[i]*255, 0, 255).astype(np.uint8), 255)
        # Dilate only RGB under transparent texels to avoid dark fringes in mipmaps.
        tile = Image.fromarray(result)
        rgb = tile.convert('RGB')
        for _ in range(5):
            rgb = rgb.filter(ImageFilter.MaxFilter(3))
        rgb = Image.composite(tile.convert('RGB'), rgb, tile.getchannel('A'))
        tile = rgb.convert('RGBA')
        tile.putalpha(Image.fromarray(result[:, :, 3]))
        atlas.paste(tile, (view*size, 0))
    return atlas, low, high


def impostor(low, high, size):
    """Build crossed vertical cards plus an overhead card with approximate crown normals and padded UVs."""
    mesh, uv = Mesh(), []
    center = (low + high) / 2
    for view, (x, y, depth) in enumerate(((0, 2, 1), (1, 2, 0), (0, 1, 2))):
        points = []
        for s, t in ((0, 0), (1, 0), (1, 1), (0, 1)):
            p = center.copy()
            p[x], p[y] = low[x] if not s else high[x], low[y] if not t else high[y]
            if view == 2:
                p[2] = low[2] + 0.72 * (high[2]-low[2])
            points.append(p)
        inset = 4 / size
        texcoords = [((view + inset + s*(1-2*inset))/4, inset+t*(1-2*inset))
                     for s, t in ((0, 0), (1, 0), (1, 1), (0, 1))]
        for indices in ((0, 1, 2), (0, 2, 3)):
            mesh.tri(*(points[i] for i in indices), (1, 1, 1))
            uv.extend(texcoords[i] for i in indices)
            for j, i in enumerate(indices):
                n = (points[i] - center) / np.maximum(high-low, 0.1)
                n[2] = max(0.35, n[2])
                mesh.n[len(mesh.n)-3+j] = n / np.linalg.norm(n)
    return mesh, uv


def bake_canopy_sources(output, models):
    """Bake reusable five-tree pieces from the original full-detail trees; never bake geographic pages or lighting."""
    manifest = {}
    for name in ('broadleaf', 'conifer'):
        cluster = Mesh()
        for i in range(5):
            angle, radius = i * 2.399963, math.sqrt(i) * 3.0
            cluster.append(models[name], (math.cos(angle)*radius, math.sin(angle)*radius, 0),
                           0.82 + (i % 4)*0.09, angle)
        atlas, low, high = bake(cluster, 512)
        texture = f'{name}-canopy-atlas.dds'
        atlas.save(output / f'{name}-canopy-atlas.png')
        write_dds(atlas, output / texture)
        proxy, uv = impostor(low, high, 512)
        write_osg(output / f'{name}-canopy.osg', proxy, uv, texture, volume_normals=True)
        manifest[name] = {'source': name, 'source_trees': 5, 'source_triangles': len(cluster.v)//3,
                          'proxy_triangles': 6, 'bounds_m': [low.tolist(), high.tolist()],
                          'atlas_size': list(atlas.size), 'lighting': 'unlit albedo and alpha'}
        print(f'{name} canopy: 5 source trees -> 6 triangles', flush=True)
    (output / 'canopy-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', newline='\r\n')


def main():
    """Generate seven original assets, their atlases, metadata and a reproducible synthetic canopy cluster."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'data/procedural2/starter')
    parser.add_argument('--canopy-only', action='store_true', help='Regenerate source-derived canopy pieces only')
    parser.add_argument('--compress-only', action='store_true', help='Prepare runtime DDS files from existing PNG atlases')
    args = parser.parse_args()
    if args.canopy_only and args.compress_only:
        parser.error('--canopy-only and --compress-only are mutually exclusive')
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    if args.compress_only:
        compress_existing_atlases(output)
        return
    models = {'broadleaf': broadleaf(), 'conifer': conifer(), 'shrub': broadleaf(99, True),
              'grass-tuft': grass(), 'fern': fern(), 'boulder': boulder()}
    bake_canopy_sources(output, models)
    if args.canopy_only:
        return
    cluster = Mesh()
    # A separate prototype, not a replacement for the source's individual trees. All roots share a local plane.
    for i in range(13):
        a, radius = i * 2.399963, math.sqrt(i) * 3.2
        tree = models['broadleaf' if i % 3 else 'conifer']
        # Bake the far aggregate from full trees; its near model uses the individual-tree impostors below.
        cluster.append(tree, (math.cos(a)*radius, math.sin(a)*radius, 0), 0.75 + (i % 4)*0.08)
    models['canopy-cluster'] = cluster
    manifest = {}
    for name, mesh in models.items():
        size = 512 if name in ('broadleaf', 'conifer', 'canopy-cluster') else 256
        atlas, low, high = bake(mesh, size)
        atlas.save(output / f'{name}-atlas.png')
        write_dds(atlas, output / f'{name}-atlas.dds')
        coarse, uv = impostor(low, high, size)
        write_osg(output / f'{name}-coarse.osg', coarse, uv, f'{name}-atlas.dds', volume_normals=True)
        if name == 'canopy-cluster':
            # Aggregate near view is an assembly of the same individual-tree impostors (78 triangles).
            text = 'Group {\nnum_children 13\n'
            for i in range(13):
                a, r, s = i*2.399963, math.sqrt(i)*3.2, 0.75+(i % 4)*0.08
                child = (output / ('broadleaf-coarse.osg' if i % 3 else 'conifer-coarse.osg')).read_text()
                text += (f'MatrixTransform {{ Matrix {{ {s} 0 0 0 0 {s} 0 0 0 0 {s} 0 '
                         f'{math.cos(a)*r} {math.sin(a)*r} 0 1 }} num_children 1\n{child}\n}}\n')
            (output / f'{name}-near.osg').write_text(text+'}\n', encoding='utf-8', newline='\r\n')
            triangles = 78
        else:
            write_osg(output / f'{name}-near.osg', mesh)
            triangles = len(mesh.v)//3
        manifest[name] = {'near_triangles': triangles, 'coarse_triangles': 6,
                          'bounds_m': [low.tolist(), high.tolist()], 'atlas_size': list(atlas.size)}
        print(f'{name}: {triangles} / 6 triangles')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', newline='\r\n')


if __name__ == '__main__':
    main()
