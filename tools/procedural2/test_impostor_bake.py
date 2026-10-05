"""Small deterministic checks for source-normal transfer and the front/back atlas contract."""
import unittest
import struct
import tempfile
from pathlib import Path
import numpy as np
from PIL import Image
from generate_pbr_assets import Part, bake, proxy_parts
from generate_starter_assets import write_dds


class ImpostorBakeTests(unittest.TestCase):
    """Use known surfaces so loss of geometry normals cannot hide behind plausible-looking tree art."""

    def test_compact_atlas_mips(self):
        """A 3-by-2 view atlas needs complete BC3 mips without power-of-two padding or a vertical reversal."""
        atlas = Image.new('RGBA', (96, 64), (10, 20, 200, 255))
        atlas.paste((200, 20, 10, 255), (0, 0, 96, 32))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'six-views.dds'
            write_dds(atlas, path)
            data = path.read_bytes()
            self.assertEqual(data[84:88], b'DXT5')
            size, levels, width, height = 128, 0, 96, 64
            while True:
                size += max(1, (width+3)//4)*max(1, (height+3)//4)*16
                levels += 1
                if width == height == 1:
                    break
                width, height = max(1, width//2), max(1, height//2)
            self.assertEqual(len(data), size)
            self.assertEqual(struct.unpack_from('<I', data, 28)[0], levels)
            with Image.open(path) as image:
                decoded = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
                np.testing.assert_allclose(decoded.getpixel((20, 16)), (200, 20, 10, 255), atol=8)
                np.testing.assert_allclose(decoded.getpixel((20, 48)), (10, 20, 200, 255), atol=8)

    def test_source_normals_and_back_faces(self):
        """A tilted smooth normal plus a tangent perturbation must retain both terms on both faces."""
        part = Part('test')
        normal = np.array([0.0, 0.6, 0.8])
        part.tri([(-1, -1, 0), (1, -1, 0), (1, 1, 0)], [(0, 0), (1, 0), (1, 1)], [normal]*3)
        part.tri([(-1, -1, 0), (1, 1, 0), (-1, 1, 0)], [(0, 0), (1, 1), (0, 1)], [normal]*3)
        maps = (Image.new('RGBA', (4, 4), (80, 120, 40, 255)),
                Image.new('RGBA', (4, 4), (204, 128, 230, 255)),
                Image.new('RGBA', (4, 4), (0, 200, 220, 0)))
        atlas, low, high = bake([part], {'test': maps}, 32)
        directions = np.array(atlas[1]).astype(float)/127.5-1
        front, back = directions[16, 80, :3], directions[48, 80, :3]
        # Chonk's cotangent frame uses a common scale, making T=(.8,0,0), B=(0,.8,-.6).
        expected = np.array([0.48, 0.48, 0.64])
        expected /= np.linalg.norm(expected)
        np.testing.assert_allclose(front, expected, atol=0.015)
        np.testing.assert_allclose(back, expected*[1, -1, -1], atol=0.015)
        self.assertEqual(atlas[0].size, (96, 64))
        self.assertTrue(np.all(np.asarray(atlas[1])[:, :, 3] == 255))
        proxy = proxy_parts(low, high, 32, 'test')[0]
        self.assertEqual(len(proxy.v), 18)
        self.assertTrue(proxy.baked_normals)
        self.assertTrue(all(v > 0.5 for u, v in proxy.uv))
        for i in range(0, 18, 3):
            a, b, c = proxy.v[i:i+3]
            face = np.cross(b-a, c-a)
            face /= np.linalg.norm(face)
            np.testing.assert_allclose(proxy.n[i], face)


if __name__ == '__main__':
    unittest.main()
