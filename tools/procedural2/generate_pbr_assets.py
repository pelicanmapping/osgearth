"""Compile original generated texture sources into low-poly, textured Vegetation2 assets.

Requires numpy and Pillow with BC3 encoding. No GL context or network is used by the baker.
Albedo sources are retained verbatim; normal/DRAM maps are approximate material reconstructions,
not measured scans. Runtime meshes, mipmapped DDS, and view-dependent proxies are reproducible.
"""
from pathlib import Path
import argparse
import io
import json
import math
import struct
import numpy as np
from PIL import Image, ImageFilter
from generate_starter_assets import write_dds, impostor, boulder


def unit(value):
    """Normalize vectors along their last axis, with a finite fallback for degenerate inputs."""
    value = np.asarray(value, dtype=float)
    return value / np.maximum(np.linalg.norm(value, axis=-1, keepdims=True), 1e-9)


class Part:
    """Triangle soup with shared material, UVs, and optional foliage-volume normals."""

    def __init__(self, material, volume=False):
        """Start a static material part without allocating texture copies."""
        self.material, self.volume = material, volume
        self.v, self.n, self.uv, self.c = [], [], [], []

    def tri(self, points, uv, normals=None, color=(1, 1, 1)):
        """Append a triangle, rejecting degeneracy and normalizing authored normals."""
        points = np.asarray(points)
        cross = np.cross(points[1]-points[0], points[2]-points[0])
        if np.linalg.norm(cross) < 1e-9:
            return
        self.v.extend(points)
        self.n.extend(unit(normals) if normals is not None else [unit(cross)]*3)
        self.uv.extend(uv)
        self.c.extend([color]*3)

    def branch(self, a, b, radius, sides=6, tip=0.25):
        """Build a tapered, smooth-normal branch; repeating UVs place bark grain along its axis."""
        a, b = np.asarray(a), np.asarray(b)
        axis = unit(b-a)
        side = unit(np.cross(axis, (0, 1, 0)))
        other = np.cross(axis, side)
        length = np.linalg.norm(b-a)
        for i in range(sides):
            angles = np.array([i, i+1])*math.tau/sides
            p, q = [side*math.cos(t)+other*math.sin(t) for t in angles]
            points = [a+radius*p, a+radius*q, b+radius*tip*q, b+radius*tip*p]
            normals = [p, q, q, p]
            uv = [(i/sides, 0), ((i+1)/sides, 0), ((i+1)/sides, length/2), (i/sides, length/2)]
            for indices in ((0, 1, 2), (0, 2, 3)):
                self.tri([points[j] for j in indices], [uv[j] for j in indices], [normals[j] for j in indices])

    def card(self, root, axis, width, height, rotation, normal, color=(1, 1, 1)):
        """Place one tight two-triangle foliage spray, rooted at its texture's bottom center."""
        axis = unit(axis)
        side = unit(np.cross(axis, (0, 0, 1) if abs(axis[2]) < 0.95 else (0, 1, 0)))
        side = side*math.cos(rotation) + np.cross(axis, side)*math.sin(rotation)
        root = np.asarray(root)
        points = [root-side*width/2, root+side*width/2,
                  root+axis*height+side*width/2, root+axis*height-side*width/2]
        uv = [(0, 0), (1, 0), (1, 1), (0, 1)]
        for indices in ((0, 1, 2), (0, 2, 3)):
            self.tri([points[j] for j in indices], [uv[j] for j in indices], [normal]*3, color)

    def append(self, other, offset, scale, angle):
        """Copy a source into an offline cluster, preserving material-space UVs and normal orientation."""
        if getattr(other, 'baked_normals', False):
            self.baked_normals = True
        rotation = np.array([[math.cos(angle), -math.sin(angle), 0],
                             [math.sin(angle), math.cos(angle), 0], [0, 0, 1]])
        self.v.extend(np.asarray(other.v) @ rotation.T * scale + offset)
        self.n.extend(np.asarray(other.n) @ rotation.T)
        self.uv.extend(other.uv)
        self.c.extend(other.c)


def make_broadleaf(shrub=False):
    """Spend geometry on a branching silhouette; leaf detail comes entirely from shared spray cards."""
    rng = np.random.default_rng(149 if shrub else 73)
    wood, leaves = Part('bark'), Part('oak', True)
    height = 1.5 if shrub else 12.0
    wood.branch((0, 0, -0.05), (height*0.02, 0, height*0.9), height*0.026, 8, 0.08)
    count = 8 if shrub else 16
    for i in range(count):
        a = i*2.399963 + rng.uniform(-0.3, 0.3)
        z = height*(0.25+0.58*i/count)
        radius = height*0.30*math.sqrt(max(0.1, 1-((z/height-0.54)/0.5)**2))
        radius *= rng.uniform(0.7, 1.05)
        origin = np.array((0, 0, z*0.8))
        end = np.array((math.cos(a)*radius, math.sin(a)*radius, z+height*0.10))
        wood.branch(origin, end, height*0.007, 5, 0.15)
        for j in range(3):
            az = a+(j-1)*0.6
            fork = end + np.array((math.cos(az)*0.065*height, math.sin(az)*0.065*height,
                                   height*rng.uniform(0.01, 0.10)))
            wood.branch(origin*0.2+end*0.8, fork, height*0.0025, 3, 0.1)
            for k in range(5):
                offset = rng.normal(0, height*0.035, 3)
                direction = unit((math.cos(az+k*1.4), math.sin(az+k*1.4), rng.uniform(0.2, 1.3)))
                normal = unit((math.cos(az)*0.45, math.sin(az)*0.45, 0.9))
                leaves.card(fork+offset, direction, height*rng.uniform(0.11, 0.15),
                            height*rng.uniform(0.12, 0.17), rng.uniform(0, math.tau), normal,
                            (rng.uniform(0.86, 1.0), rng.uniform(0.90, 1.0), rng.uniform(0.85, 1.0)))
    return [wood, leaves]


def make_conifer():
    """Build sparse radial woody tiers with overlapping needle-spray cards; no individual needle triangles."""
    wood, needles = Part('bark'), Part('spruce', True)
    rng = np.random.default_rng(962)
    wood.branch((0, 0, -0.05), (0, 0, 14), 0.28, 8, 0.015)
    for tier in range(11):
        z = 1.1+tier*1.08
        radius = 3.5*(1-z/15)
        for branch in range(7):
            angle = branch*math.tau/7 + tier*2.399963
            axis = np.array((math.cos(angle), math.sin(angle), 0))
            root = np.array((0, 0, z))
            end = root+axis*radius + (0, 0, -0.25)
            wood.branch(root, end, 0.044*(1-tier/14), 3, 0.03)
            for k in range(3):
                direction = unit(axis + (0, 0, 0.2+k*0.18))
                needles.card(root+axis*radius*k*0.22, direction, radius*0.90, radius*0.75,
                             rng.uniform(-0.7, 0.7), unit(axis*0.35+(0, 0, 0.9)),
                             (rng.uniform(0.91, 1.0), rng.uniform(0.94, 1.0), rng.uniform(0.92, 1.0)))
    return [wood, needles]


def make_ground(name):
    """Use just a few radial cards for ground cover; shaped fronds use three bent segments."""
    part = Part('grass' if name == 'grass-tuft' else 'fern', True)
    rng = np.random.default_rng(348)
    if name == 'grass-tuft':
        for i in range(5):
            angle = i*2.399963
            part.card((rng.uniform(-0.1, 0.1), rng.uniform(-0.1, 0.1), -0.015),
                      (math.cos(angle)*0.22, math.sin(angle)*0.22, 1),
                      rng.uniform(0.55, 0.72), rng.uniform(0.5, 0.82), angle, (0, 0, 1))
    else:
        for i in range(8):
            angle = i*2.399963
            axis, side = np.array((math.cos(angle), math.sin(angle), 0)), np.array((-math.sin(angle), math.cos(angle), 0))
            length = rng.uniform(0.8, 1.1)
            for seg in range(3):
                t0, t1 = seg/3, (seg+1)/3
                p0 = axis*t0*length + (0, 0, math.sin(t0*math.pi*0.78)*0.68)
                p1 = axis*t1*length + (0, 0, math.sin(t1*math.pi*0.78)*0.68)
                points = [p0-side*0.3, p0+side*0.3, p1+side*0.3, p1-side*0.3]
                uv = [(0, t0), (1, t0), (1, t1), (0, t1)]
                for ids in ((0, 1, 2), (0, 2, 3)):
                    part.tri([points[j] for j in ids], [uv[j] for j in ids], [(0, 0, 1)]*3)
    return [part]


def make_rock():
    """Reuse the existing 156-triangle silhouette with smooth normals and surface UVs instead of colored facets."""
    source, result = boulder(), Part('stone')
    for i in range(0, len(source.v), 3):
        points = np.array(source.v[i:i+3])
        normals = unit((points-(0, 0, 0.55))/(1.4, 1.0, 0.95))
        # Dominant-axis projection avoids collapsed triangles at a spherical unwrap's poles.
        face = np.abs(np.cross(points[1]-points[0], points[2]-points[0]))
        axes = [(1, 2), (0, 2), (0, 1)][np.argmax(face)]
        result.tri(points, points[:, axes]*0.7, normals)
    return [result]


def dilate(image, iterations=8):
    """Extend covered RGB into UV padding without altering alpha; prevents filtered black card edges."""
    data = np.asarray(image).copy()
    valid = data[:, :, 3] > 16
    rgb = data[:, :, :3].astype(float)
    for _ in range(iterations):
        total, count = np.zeros_like(rgb), np.zeros(valid.shape)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            mask = np.roll(valid, (dy, dx), (0, 1))
            total += np.roll(rgb, (dy, dx), (0, 1))*mask[:, :, None]
            count += mask
        fill = ~valid & (count > 0)
        rgb[fill] = total[fill]/count[fill, None]
        valid[fill] = True
    data[:, :, :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    return Image.fromarray(data)


def material_maps(albedo, roughness, strength, foliage):
    """Derive restrained tangent micro-normal and DRAM data from source detail; metallic stays zero for natural materials."""
    rgb = np.asarray(albedo)[:, :, :3].astype(float)/255
    height = rgb @ np.array([0.2126, 0.7152, 0.0722])
    smooth = np.asarray(Image.fromarray(np.uint8(height*255)).filter(ImageFilter.GaussianBlur(1.2)))/255
    dx = (np.roll(smooth, -1, 1)-np.roll(smooth, 1, 1))*strength
    dy = (np.roll(smooth, -1, 0)-np.roll(smooth, 1, 0))*strength
    if foliage:
        mask = np.asarray(albedo)[:, :, 3] > 128
        interior = mask & np.roll(mask, 2, 0) & np.roll(mask, -2, 0) & np.roll(mask, 2, 1) & np.roll(mask, -2, 1)
        dx *= interior
        dy *= interior
    normal = unit(np.stack((-dx, dy, np.ones_like(height)), -1))
    normal = np.concatenate(((normal*0.5+0.5)*255, np.full((*height.shape, 1), 255)), axis=-1)
    dram = np.zeros((*height.shape, 4), dtype=np.uint8)
    dram[:, :, 0] = np.uint8(height*255)
    dram[:, :, 1] = np.uint8(np.clip(roughness+(0.5-height)*0.15, 0.35, 1)*255)
    dram[:, :, 2] = np.uint8(np.clip(0.90+0.10*height if foliage else 0.75+0.25*height, 0, 1)*255)
    return Image.fromarray(np.uint8(normal)), Image.fromarray(dram)


def write_opaque_dds(image, path):
    """Use BC1 without an alpha channel for solid bark/stone, retaining the full bottom-up mip chain."""
    mip, levels, header = image.convert('RGB'), [], None
    while True:
        encoded = io.BytesIO()
        mip.transpose(Image.Transpose.FLIP_TOP_BOTTOM).save(encoded, format='DDS', pixel_format='DXT1')
        data = encoded.getvalue()
        expected = max(1, (mip.width+3)//4)*max(1, (mip.height+3)//4)*8
        if data[:4] != b'DDS ' or data[84:88] != b'DXT1' or len(data) != 128+expected:
            raise RuntimeError('DDS encoder did not produce the expected opaque BC1 payload')
        if header is None:
            header = bytearray(data[:128])
        levels.append(data[128:])
        if mip.size == (1, 1):
            break
        mip = mip.resize((max(1, mip.width//2), max(1, mip.height//2)), Image.Resampling.BOX)
    struct.pack_into('<I', header, 8, struct.unpack_from('<I', header, 8)[0] | 0x20000)
    struct.pack_into('<I', header, 20, len(levels[0]))
    struct.pack_into('<I', header, 28, len(levels))
    struct.pack_into('<I', header, 108, 0x1000 | 0x8 | 0x400000)
    path.write_bytes(header+b''.join(levels))


def export_material(output, name, maps):
    """Save editable maps and BC3 mip chains in Chonk's albedo / normal / displacement-roughness-AO-metal order."""
    for suffix, image in zip(('albedo', 'normal', 'dram'), maps):
        image.save(output/f'{name}-{suffix}.png')
        if name in ('bark', 'stone') and suffix == 'albedo':
            write_opaque_dds(image, output/f'{name}-{suffix}.dds')
        else:
            write_dds(image, output/f'{name}-{suffix}.dds')


def prepare_materials(output):
    """Compile supplied RGBA botanical art and opaque surface sources into shared 512/1024px materials."""
    result = {}
    atlas = Image.open(output/'source/foliage.png').convert('RGBA')
    w, h = atlas.size
    for name, col, row in [('oak', 0, 0), ('spruce', 1, 0), ('grass', 0, 1), ('fern', 1, 1)]:
        albedo = atlas.crop((col*w//2, row*h//2, (col+1)*w//2, (row+1)*h//2)).resize((512, 512), Image.Resampling.LANCZOS)
        albedo = dilate(albedo)
        result[name] = (albedo, *material_maps(albedo, 0.66 if name == 'oak' else 0.78, 2.0, True))
    for name in ('bark', 'stone'):
        albedo = Image.open(output/f'source/{name}.png').convert('RGBA').resize((1024, 1024), Image.Resampling.LANCZOS)
        result[name] = (albedo, *material_maps(albedo, 0.92 if name == 'bark' else 0.86, 7.0, False))
    for name, maps in result.items():
        export_material(output, name, maps)
    return result


def write_model(path, parts):
    """Export static OSG parts with three standard Chonk texture units; no embedded programs or alpha blending."""
    def array(label, kind, values):
        """Write deterministic ASCII arrays readable by stock OSG across platforms."""
        return f'{label} {kind} {len(values)} {{\n' + '\n'.join(' '.join(f'{x:.6f}' for x in v) for v in values) + '\n}\n'
    text = f'Group {{\nDataVariance STATIC\nnum_children {len(parts)}\n'
    for part in parts:
        text += 'Geode {\nDataVariance STATIC\nStateSet {\n'
        for unit_id, suffix in enumerate(('albedo', 'normal', 'dram')):
            wrap = 'REPEAT' if getattr(part, 'repeat', part.material in ('bark', 'stone')) else 'CLAMP_TO_EDGE'
            text += (f'textureUnit {unit_id} {{ GL_TEXTURE_2D ON Texture2D {{\n'
                     f'file "{part.material}-{suffix}.dds"\nwrap_s {wrap}\nwrap_t {wrap}\n'
                     'min_filter LINEAR_MIPMAP_LINEAR\nmag_filter LINEAR\nmaxAnisotropy 4\n'
                     'resizeNonPowerOfTwo FALSE\n} }\n')
        text += ('}\nnum_drawables 1\nGeometry {\nuseDisplayList FALSE\nuseVertexBufferObjects TRUE\n'
                 f'PrimitiveSets 1 {{ DrawArrays TRIANGLES 0 {len(part.v)} }}\n')
        text += array('VertexArray', 'Vec3Array', part.v)
        text += 'NormalBinding PER_VERTEX\n' + array('NormalArray', 'Vec3Array', part.n)
        text += 'ColorBinding PER_VERTEX\n' + array('ColorArray', 'Vec4Array', [(*c, 1) for c in part.c])
        text += array('TexCoordArray 0', 'Vec2Array', part.uv)
        if part.volume:
            technique = 4 if getattr(part, 'baked_normals', False) else 3
            text += f'VertexAttribBinding 6 OVERALL\nVertexAttribArray 6 UByteArray 1 {{ {technique} }}\n'
        text += '}\n}\n'
    path.write_text(text+'}\n', encoding='utf-8', newline='\r\n')


def surface_normals(positions, normals, uv, screen, weights, bump, side, volume):
    """Reproduce Chonk's derivative normal mapping and two-sided leaves for an orthographic bake view."""
    a, b, c = weights
    n = unit(a[..., None]*normals[0]+b[..., None]*normals[1]+c[..., None]*normals[2])
    edges, st, tex = positions[1:]-positions[0], screen[1:]-screen[0], uv[1:]-uv[0]
    det = st[0, 0]*st[1, 1]-st[1, 0]*st[0, 1]
    # Raster Y points down; a back capture also reverses camera X while retaining the same atlas coordinates.
    dpdx = side*(edges[0]*st[1, 1]-edges[1]*st[0, 1])/det
    dpdy = -(edges[1]*st[0, 0]-edges[0]*st[1, 0])/det
    dtdx = side*(tex[0]*st[1, 1]-tex[1]*st[0, 1])/det
    dtdy = -(tex[1]*st[0, 0]-tex[0]*st[1, 0])/det
    p, q = np.cross(dpdy, n), np.cross(n, dpdx)
    tangent, bitangent = p*dtdx[0]+q*dtdy[0], p*dtdx[1]+q*dtdy[1]
    scale = np.maximum(np.linalg.norm(tangent, axis=-1), np.linalg.norm(bitangent, axis=-1))
    scale = np.maximum(scale[..., None], 1e-9)
    detail = unit(bump[..., :3].astype(float)/127.5-1)
    result = unit((tangent*detail[..., 0:1]+bitangent*detail[..., 1:2])/scale+n*detail[..., 2:3])
    # Negative screen determinant means the original triangle faces this view (raster Y is inverted).
    if not volume and side*det > 0:
        result = -result
    return result


def bake(parts, materials, size):
    """Bake six unlit views with source-surface normals in each receiving card's orthonormal tangent frame."""
    vertices = np.concatenate([p.v for p in parts])
    low, high = vertices.min(0)-0.04, vertices.max(0)+0.04
    atlases = [Image.new('RGBA', (3*size, 2*size), fill) for fill in
               ((75, 95, 40, 0), (128, 128, 255, 255), (0, 210, 245, 0))]
    textures = {name: [np.asarray(im) for im in maps] for name, maps in materials.items()}
    for capture in range(6):
        view, row = capture % 3, capture // 3
        x_axis, y_axis, depth_axis = ((0, 2, 1), (1, 2, 0), (0, 1, 2))[view]
        side = 1 if row == 0 else -1
        frame = np.array([np.eye(3)[x_axis], np.eye(3)[y_axis],
                          np.cross(np.eye(3)[x_axis], np.eye(3)[y_axis])])
        depth_sign = side*frame[2, depth_axis]
        color, normal, dram = [np.array(Image.new('RGBA', (size, size), fill)) for fill in
                               ((75, 95, 40, 0), (128, 128, 255, 255), (0, 210, 245, 0))]
        depth = np.full((size, size), -np.inf)
        for part in parts:
            v, uv, tint, normals = map(np.asarray, (part.v, part.uv, part.c, part.n))
            screen = (v[:, (x_axis, y_axis)]-low[[x_axis, y_axis]])/(high-low)[[x_axis, y_axis]]
            screen[:, 1] = 1-screen[:, 1]
            screen = screen*(size-8)+4
            albedo, bump, properties = textures[part.material]
            repeat = getattr(part, 'repeat', part.material in ('bark', 'stone'))
            for i in range(0, len(v), 3):
                p = screen[i:i+3]
                x0, y0 = np.maximum(0, np.floor(p.min(0)).astype(int))
                x1, y1 = np.minimum(size-1, np.ceil(p.max(0)).astype(int))
                if x1 < x0 or y1 < y0:
                    continue
                den = (p[1, 1]-p[2, 1])*(p[0, 0]-p[2, 0])+(p[2, 0]-p[1, 0])*(p[0, 1]-p[2, 1])
                if abs(den) < 1e-7:
                    continue
                xx, yy = np.meshgrid(np.arange(x0, x1+1)+0.5, np.arange(y0, y1+1)+0.5)
                a = ((p[1, 1]-p[2, 1])*(xx-p[2, 0])+(p[2, 0]-p[1, 0])*(yy-p[2, 1]))/den
                b = ((p[2, 1]-p[0, 1])*(xx-p[2, 0])+(p[0, 0]-p[2, 0])*(yy-p[2, 1]))/den
                c = 1-a-b
                tex = a[..., None]*uv[i]+b[..., None]*uv[i+1]+c[..., None]*uv[i+2]
                tex = np.mod(tex, 1) if repeat else np.clip(tex, 0, 1)
                tx = np.clip((tex[:, :, 0]*(albedo.shape[1]-1)).astype(int), 0, albedo.shape[1]-1)
                ty = np.clip(((1-tex[:, :, 1])*(albedo.shape[0]-1)).astype(int), 0, albedo.shape[0]-1)
                sample = albedo[ty, tx].copy()
                z = depth_sign*(a*v[i, depth_axis]+b*v[i+1, depth_axis]+c*v[i+2, depth_axis])
                target = depth[y0:y1+1, x0:x1+1]
                mask = (a >= 0) & (b >= 0) & (c >= 0) & (z > target) & (sample[:, :, 3] >= 100)
                if not mask.any():
                    continue
                target[mask] = z[mask]
                vertex_tint = a[..., None]*tint[i]+b[..., None]*tint[i+1]+c[..., None]*tint[i+2]
                sample[:, :, :3] = np.uint8(np.clip(sample[:, :, :3]*vertex_tint, 0, 255))
                color[y0:y1+1, x0:x1+1][mask] = sample[mask]
                dram[y0:y1+1, x0:x1+1][mask] = properties[ty, tx][mask]
                world_normal = surface_normals(v[i:i+3], normals[i:i+3], uv[i:i+3], p,
                                               (a, b, c), bump[ty, tx], side, part.volume)
                encoded = np.uint8(np.clip((world_normal @ frame.T+1)*127.5, 0, 255))
                normal[y0:y1+1, x0:x1+1, :3][mask] = encoded[mask]
        # Extend surface directions into transparent padding before filtering; retain nonzero normal-map alpha.
        normal[:, :, 3] = color[:, :, 3]
        normal = dilate(Image.fromarray(normal))
        normal.putalpha(255)
        for atlas, tile in zip(atlases, (dilate(Image.fromarray(color)), normal, Image.fromarray(dram))):
            atlas.paste(tile, (view*size, row*size))
    return atlases, low, high


def proxy_parts(low, high, size, material):
    """Emit six triangles with planar frames and front-row UVs; Chonk selects each card's back capture."""
    mesh, uv = impostor(low, high, size)
    part = Part(material, True)
    part.baked_normals = True
    part.v, part.c, part.uv = mesh.v, mesh.c, [(u*4/3, 0.5+v*0.5) for u, v in uv]
    for i in range(0, len(mesh.v), 3):
        a, b, c = mesh.v[i:i+3]
        part.n.extend([unit(np.cross(b-a, c-a))]*3)
    return [part]


def cluster(sources, count):
    """Assemble source assets offline only; geographic masks and terrain fitting remain runtime responsibilities."""
    output = {}
    for i in range(count):
        angle, radius, scale = i*2.399963, math.sqrt(i)*3.0, 0.82+(i % 4)*0.09
        for part in sources[i % len(sources)]:
            key = (part.material, part.volume)
            if key not in output:
                output[key] = Part(*key)
            output[key].append(part, (math.cos(angle)*radius, math.sin(angle)*radius, 0), scale, angle)
    return list(output.values())


def main():
    """Compile all populations, source-derived canopies, bounded mesh counts, and a reproducible material manifest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2]/'data/procedural2/pbr')
    args = parser.parse_args()
    output = args.output
    materials = prepare_materials(output)
    models = {'broadleaf': make_broadleaf(), 'conifer': make_conifer(), 'shrub': make_broadleaf(True),
              'grass-tuft': make_ground('grass-tuft'), 'fern': make_ground('fern'), 'boulder': make_rock()}
    limits = {'broadleaf': 1000, 'conifer': 1000, 'shrub': 512, 'grass-tuft': 16, 'fern': 64, 'boulder': 156}
    manifest = {'material_layout': 'albedo RGBA; normal OpenGL RGB; DRAM = displacement, roughness, AO, metallic',
                'normal_source': 'near micro-height reconstruction; six-view proxies bake source geometry and normal maps',
                'assets': {}}
    for name, parts in models.items():
        triangles = sum(len(p.v)//3 for p in parts)
        assert triangles <= limits[name], (name, triangles, limits[name])
        write_model(output/f'{name}-near.osg', parts)
        size = 512 if name in ('broadleaf', 'conifer') else 256
        maps, low, high = bake(parts, materials, size)
        export_material(output, f'{name}-proxy', maps)
        proxies = proxy_parts(low, high, size, f'{name}-proxy')
        write_model(output/f'{name}-coarse.osg', proxies)
        materials[f'{name}-proxy'] = maps
        manifest['assets'][name] = {'near_triangles': triangles, 'coarse_triangles': 6,
                                     'bounds_m': [low.tolist(), high.tolist()]}
        print(f'{name}: {triangles} / 6 triangles', flush=True)
    for name in ('broadleaf', 'conifer'):
        pieces = cluster([models[name]], 5)
        maps, low, high = bake(pieces, materials, 512)
        export_material(output, f'{name}-canopy', maps)
        write_model(output/f'{name}-canopy.osg', proxy_parts(low, high, 512, f'{name}-canopy'))
        manifest['assets'][name]['canopy_triangles'] = 6
        print(f'{name} canopy: 5 textured trees -> 6 triangles', flush=True)
    pieces = cluster([models['broadleaf'], models['conifer']], 13)
    maps, low, high = bake(pieces, materials, 512)
    export_material(output, 'canopy-cluster-proxy', maps)
    write_model(output/'canopy-cluster-coarse.osg', proxy_parts(low, high, 512, 'canopy-cluster-proxy'))
    sources = []
    for name in ('broadleaf', 'conifer'):
        v = np.concatenate([p.v for p in models[name]])
        sources.append(proxy_parts(v.min(0)-0.04, v.max(0)+0.04, 512, f'{name}-proxy'))
    write_model(output/'canopy-cluster-near.osg', cluster(sources, 13))
    manifest['assets']['canopy-cluster'] = {'near_triangles': 78, 'coarse_triangles': 6}
    manifest['runtime_dds_bytes'] = sum(p.stat().st_size for p in output.glob('*.dds'))
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8', newline='\r\n')


if __name__ == '__main__':
    main()
