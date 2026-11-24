"""Prepare the Quixel Megascans models that tests/clustermesh.earth uses.

Usage: python prepare.py <folder with the downloaded files> [--output <folder>]

README.md, next to this script, lists the six files to download from fab.com. This script
finds them anywhere under the given folder and writes the models the earth file refers to
into the output folder, which defaults to the folder of this script:

- Rocks: a copy of each .glb with its 8K textures resized to 4096 pixels.
- Plants: the glTF from each .zip with its textures resized to 2048 pixels, and with the
  lower-detail copies (nodes named *_LOD1, *_LOD2, ...) taken out of the scene. A cluster
  mesh makes its own levels of detail, and each remaining node is one variant of the plant.

Geometry is never changed. The smaller textures make the models load several times faster.
A model that already exists in the output folder is skipped. Requires Pillow.
"""
import argparse
import io
import json
import os
import posixpath
import re
import struct
import sys
import zipfile

from PIL import Image

# The scans are larger than Pillow's default decompression-bomb limit.
Image.MAX_IMAGE_PIXELS = None

# Folder name, downloaded file, and the largest texture size to keep.
ROCKS = [
    ("mossy_forest_boulder", "mossy_forest_boulder_wfstchwaw_raw.glb", 4096),
    ("icelandic_rock_formation", "icelandic_rock_formation_tfiqaftda_raw.glb", 4096),
    ("canyon_sandstone_boulder", "canyon_sandstone_boulder_ulmtabz_raw.glb", 4096),
    ("massive_nordic_coastal_cliff", "massive_nordic_coastal_cliff_veqnci0db_raw.glb", 4096),
]
PLANTS = [
    ("lady_fern", "lady_fern_wdvlditia_ue_high.zip", 2048),
    ("tundra_grass", "tundra_grass_wk2oaeyqx_ue_raw.zip", 2048),
]

GLB_JSON = 0x4E4F534A
GLB_BIN = 0x004E4942


def find(folder, name):
    """Return the path of the first file called name under folder, or None."""
    for root, _, files in os.walk(folder):
        if name in files:
            return os.path.join(root, name)
    return None


def shrink(data, max_size):
    """Return (bytes, mime type) of an encoded image resized to fit max_size on its longer side.

    Images that use their alpha channel stay PNG. The rest become JPEG, which loads faster.
    """
    image = Image.open(io.BytesIO(data))
    image.load()
    has_alpha = image.mode in ("RGBA", "LA") or "transparency" in image.info
    if has_alpha:
        image = image.convert("RGBA")
        if image.getextrema()[3][0] == 255:
            has_alpha = False
    if not has_alpha:
        image = image.convert("RGB")
    scale = max_size / float(max(image.size))
    if scale < 1.0:
        size = (max(1, round(image.size[0] * scale)), max(1, round(image.size[1] * scale)))
        image = image.resize(size, Image.LANCZOS)
    out = io.BytesIO()
    if has_alpha:
        image.save(out, "PNG", compress_level=6)
        return out.getvalue(), "image/png"
    image.save(out, "JPEG", quality=92, subsampling=0)
    return out.getvalue(), "image/jpeg"


def prepare_rock(source, target, max_size):
    """Write a copy of a binary glTF whose embedded images are resized; everything else is kept."""
    with open(source, "rb") as f:
        magic, version, _ = struct.unpack("<4sII", f.read(12))
        if magic != b"glTF" or version != 2:
            raise ValueError("%s is not a glTF 2.0 binary file" % source)
        length, kind = struct.unpack("<II", f.read(8))
        if kind != GLB_JSON:
            raise ValueError("%s does not start with a JSON chunk" % source)
        gltf = json.loads(f.read(length))
        length, kind = struct.unpack("<II", f.read(8))
        if kind != GLB_BIN:
            raise ValueError("%s has no binary chunk" % source)
        binary = f.read(length)

    # Rebuild the binary chunk view by view, since resized images change every later offset.
    image_views = {image["bufferView"]: image for image in gltf.get("images", []) if "bufferView" in image}
    out = bytearray()
    for index, view in enumerate(gltf["bufferViews"]):
        if view.get("buffer", 0) != 0:
            raise ValueError("%s uses more than one buffer" % source)
        start = view.get("byteOffset", 0)
        chunk = binary[start:start + view["byteLength"]]
        if index in image_views:
            chunk, mime = shrink(chunk, max_size)
            image_views[index]["mimeType"] = mime
        out += b"\0" * (-len(out) % 4)
        view["byteOffset"] = len(out)
        view["byteLength"] = len(chunk)
        out += chunk
    out += b"\0" * (-len(out) % 4)
    gltf["buffers"] = [{"byteLength": len(out)}]

    text = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(text) + 8 + len(out)))
        f.write(struct.pack("<II", len(text), GLB_JSON))
        f.write(text)
        f.write(struct.pack("<II", len(out), GLB_BIN))
        f.write(out)


def prepare_plant(source, target, max_size):
    """Write the glTF of a Megascans plant archive to target, with resized textures beside it.

    The geometry buffer goes one folder above target, where the glTF expects it. Files are
    read from the archive by name, so nothing is written outside the output folder.
    """
    with zipfile.ZipFile(source) as archive:
        names = [n for n in archive.namelist() if n.startswith("standard/") and n.endswith(".gltf")]
        if len(names) != 1:
            raise ValueError("%s does not hold exactly one standard/*.gltf" % source)
        gltf = json.loads(archive.read(names[0]))
        target_dir = os.path.dirname(target)
        os.makedirs(target_dir, exist_ok=True)

        def member(uri):
            """Archive member that a URI in the glTF refers to."""
            return posixpath.normpath(posixpath.join("standard", uri))

        for buffer in gltf.get("buffers", []):
            name = posixpath.basename(buffer["uri"])
            with open(os.path.join(os.path.dirname(target_dir), name), "wb") as f:
                f.write(archive.read(member(buffer["uri"])))
            buffer["uri"] = "../" + name

        for image in gltf.get("images", []):
            data, mime = shrink(archive.read(member(image["uri"])), max_size)
            stem = posixpath.splitext(posixpath.basename(image["uri"]))[0]
            name = "%s_%d.%s" % (stem, max_size, "png" if mime == "image/png" else "jpg")
            with open(os.path.join(target_dir, name), "wb") as f:
                f.write(data)
            image["uri"] = name
            image["mimeType"] = mime

    # Keep the full-detail node of each variant and drop its baked levels of detail.
    for scene in gltf.get("scenes", []):
        scene["nodes"] = [n for n in scene["nodes"]
                          if not re.search(r"_LOD[1-9]\d*$", gltf["nodes"][n].get("name", ""))]

    with open(target, "w", encoding="utf-8") as f:
        json.dump(gltf, f)


def main():
    """Prepare every model whose download is present; return the number that are missing."""
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description="Prepare the Megascans models for tests/clustermesh.earth.")
    parser.add_argument("downloads", help="folder that holds the files downloaded from fab.com")
    parser.add_argument("--output", default=here, help="folder to write the models to (default: this folder)")
    args = parser.parse_args()

    jobs = [(name, file, os.path.join(args.output, name, name + "_4k.glb"), size, prepare_rock)
            for name, file, size in ROCKS]
    jobs += [(name, file, os.path.join(args.output, name, "standard", name + "_2k.gltf"), size, prepare_plant)
             for name, file, size in PLANTS]

    missing = 0
    for name, file, target, size, prepare in jobs:
        if os.path.exists(target):
            print("%s: already prepared" % name)
            continue
        source = find(args.downloads, file)
        if not source:
            print("%s: %s was not found under %s" % (name, file, args.downloads))
            missing += 1
            continue
        print("%s: preparing from %s" % (name, source))
        prepare(source, target, size)
    return missing


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
