#!/usr/bin/env python3
"""Regenerate the small OBJ kit models and deterministic text/binary city fixtures."""

from __future__ import annotations

import argparse
from array import array
import math
import random
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parent
MODELS = ROOT / "models"
CITIES = ROOT / "cities"
MAX_INSTANCE_RANGE = 3.402823466e38
WINDOW_MAX_RANGE = 1500.0

# Bounds measured from models/building.osgb. The residential generator keeps
# the model's lowest vertex on the local ground plane and uses its footprint
# to leave a small but visible yard between neighboring houses.
RESIDENTIAL_MIN_Z = -1.68484
RESIDENTIAL_HALF_WIDTH = 10.15
RESIDENTIAL_HALF_DEPTH = 9.10
RESIDENTIAL_HEIGHT = 9.78


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def material(name: str, color: tuple[float, float, float], texture: str | None = None) -> str:
    lines = [
        f"newmtl {name}",
        f"Kd {color[0]:.4f} {color[1]:.4f} {color[2]:.4f}",
        "Ka 0.08 0.08 0.08",
        "Ks 0.04 0.04 0.04",
        "Ns 12.0",
    ]
    if texture:
        lines.append(f"map_Kd ../textures/{texture}")
    return "\n".join(lines) + "\n"


def impostor_material(name: str, color: tuple[float, float, float]) -> str:
    """A bright, matte proxy material that remains readable in shadow."""
    ambient = tuple(component * 0.45 for component in color)
    emission = tuple(component * 0.06 for component in color)
    return "\n".join((
        f"newmtl {name}",
        f"Kd {color[0]:.4f} {color[1]:.4f} {color[2]:.4f}",
        f"Ka {ambient[0]:.4f} {ambient[1]:.4f} {ambient[2]:.4f}",
        f"Ke {emission[0]:.4f} {emission[1]:.4f} {emission[2]:.4f}",
        "Ks 0.02 0.02 0.02",
        "Ns 8.0",
        "illum 2",
    )) + "\n"


def quad_obj(material_file: str, material_name: str, roof: bool) -> str:
    # Define the model in osgEarth's Z-up local frame, then author Wavefront's
    # Y-up coordinates. OSG's OBJ reader performs the inverse conversion while
    # loading. Winding and normals face outward after that conversion.
    radius = 0.25
    if roof:
        name = "unit_roof_quad"
        normal = (0, 0, 1)
        vertices = [
            (-radius, -radius, 0),
            (radius, -radius, 0),
            (radius, radius, 0),
            (-radius, radius, 0),
        ]
    else:
        name = "unit_wall_quad"
        normal = (0, -1, 0)
        vertices = [
            (-radius, 0, 0),
            (radius, 0, 0),
            (radius, 0, 0.5),
            (-radius, 0, 0.5),
        ]

    obj_normal = to_obj_coordinates(normal)
    lines = [
        f"mtllib {material_file}",
        f"o {name}",
        f"usemtl {material_name}",
        f"vn {obj_normal[0]} {obj_normal[1]} {obj_normal[2]}",
    ]
    for vertex in vertices:
        obj_vertex = to_obj_coordinates(vertex)
        lines.append(f"v {obj_vertex[0]} {obj_vertex[1]} {obj_vertex[2]}")
    for uv in ((0, 0), (1, 0), (1, 1), (0, 1)):
        lines.append(f"vt {uv[0]} {uv[1]}")
    lines.append("f 1/1/1 2/2/1 3/3/1")
    lines.append("f 1/1/1 3/3/1 4/4/1")
    return "\n".join(lines) + "\n"


class ObjBuilder:
    def __init__(self, material_file: str, name: str):
        self.lines = [f"mtllib {material_file}", f"o {name}"]
        self.vertex_count = 0
        self.normal_count = 0
        self.triangle_count = 0

    def triangle(self, material_name: str, a, b, c) -> None:
        self.lines.append(f"usemtl {material_name}")
        normal = cross(subtract(b, a), subtract(c, a))
        length = math.sqrt(sum(value * value for value in normal)) or 1.0
        normal = tuple(value / length for value in normal)
        obj_normal = to_obj_coordinates(normal)
        self.lines.append(f"vn {obj_normal[0]:.6f} {obj_normal[1]:.6f} {obj_normal[2]:.6f}")
        self.normal_count += 1
        normal_index = self.normal_count
        first = self.vertex_count + 1
        for vertex in (a, b, c):
            obj_vertex = to_obj_coordinates(vertex)
            self.lines.append(f"v {obj_vertex[0]:.6f} {obj_vertex[1]:.6f} {obj_vertex[2]:.6f}")
            self.vertex_count += 1
        self.lines.append(f"f {first}//{normal_index} {first+1}//{normal_index} {first+2}//{normal_index}")
        self.triangle_count += 1

    def quad(self, material_name: str, a, b, c, d) -> None:
        self.triangle(material_name, a, b, c)
        self.triangle(material_name, a, c, d)

    def box(self, material_name: str, lo, hi) -> None:
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        self.quad(material_name, (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))
        self.quad(material_name, (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1))
        self.quad(material_name, (x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1))
        self.quad(material_name, (x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1))
        self.quad(material_name, (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0))
        self.quad(material_name, (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))

    def cylinder(self, material_name: str, radius0: float, radius1: float, z0: float, z1: float, sides=8) -> None:
        bottom = []
        top = []
        for i in range(sides):
            angle = 2.0 * math.pi * i / sides
            bottom.append((radius0 * math.cos(angle), radius0 * math.sin(angle), z0))
            top.append((radius1 * math.cos(angle), radius1 * math.sin(angle), z1))
        for i in range(sides):
            j = (i + 1) % sides
            self.quad(material_name, bottom[i], bottom[j], top[j], top[i])
            self.triangle(material_name, (0, 0, z0), bottom[j], bottom[i])
            self.triangle(material_name, (0, 0, z1), top[i], top[j])

    def text(self) -> str:
        return "\n".join(self.lines) + "\n"


class ImpostorBuilder:
    """Compact indexed OBJ containing five quads per tier box."""

    def __init__(self, material_file: str, name: str):
        self.material_file = material_file
        self.name = name
        self.vertices: list[tuple[float, float, float]] = []
        self.normals: list[tuple[float, float, float]] = []
        self.faces: dict[str, list[tuple[tuple[int, ...], int]]] = {}
        self.triangle_count = 0

    def box(self, wall_material: str, roof_material: str, vertices) -> None:
        first_vertex = len(self.vertices) + 1
        self.vertices.extend(to_obj_coordinates(vertex) for vertex in vertices)
        faces = (
            (wall_material, (0, 1, 5, 4)),
            (wall_material, (1, 2, 6, 5)),
            (wall_material, (2, 3, 7, 6)),
            (wall_material, (3, 0, 4, 7)),
            (roof_material, (4, 5, 6, 7)),
        )
        for material_name, offsets in faces:
            a, b, c = (vertices[offsets[i]] for i in range(3))
            normal = cross(subtract(b, a), subtract(c, a))
            length = math.sqrt(sum(value * value for value in normal)) or 1.0
            normal = tuple(value / length for value in normal)
            self.normals.append(to_obj_coordinates(normal))
            normal_index = len(self.normals)
            indices = tuple(first_vertex + offset for offset in offsets)
            self.faces.setdefault(material_name, []).append((indices, normal_index))
            self.triangle_count += 2

    def text(self) -> str:
        lines = [f"mtllib {self.material_file}", f"o {self.name}"]
        lines.extend(f"v {x:.3f} {y:.3f} {z:.3f}" for x, y, z in self.vertices)
        lines.extend(f"vn {x:.6f} {y:.6f} {z:.6f}" for x, y, z in self.normals)
        for material_name, faces in self.faces.items():
            lines.append(f"usemtl {material_name}")
            for indices, normal_index in faces:
                lines.append("f " + " ".join(
                    f"{index}//{normal_index}" for index in indices))
        return "\n".join(lines) + "\n"


class CitySceneBuilder:
    """Generic per-city scene companion for non-instanced OSG geometry."""

    def __init__(self, material_file: str, name: str):
        self.material_file = material_file
        self.name = name
        self.vertices: list[tuple[float, float, float]] = []
        self.road_quads: list[tuple[tuple[float, float, float], ...]] = []
        self.smoke_positions: list[tuple[float, float, float]] = []
        self.span_count = 0
        self.segment_count = 0

    def add_powerline_span(self, start, end, sag: float, segments: int = 24) -> None:
        def point(t: float):
            return (
                start[0] + (end[0] - start[0]) * t,
                start[1] + (end[1] - start[1]) * t,
                start[2] + (end[2] - start[2]) * t - 4.0 * sag * t * (1.0 - t),
            )

        for segment in range(segments):
            self.vertices.append(point(segment / segments))
            self.vertices.append(point((segment + 1) / segments))
        self.span_count += 1
        self.segment_count += segments

    def add_road_quad(
        self,
        x0: float,
        y0: float,
        x1: float,
        y1: float,
        z: float = 5.0,
    ) -> None:
        """Add one upward-facing indexed quad in the local ENU frame."""
        self.road_quads.append((
            (x0, y0, z),
            (x1, y0, z),
            (x1, y1, z),
            (x0, y1, z),
        ))

    def add_smoke_stack(self, position) -> None:
        self.smoke_positions.append(position)

    def text(self) -> str:
        lines = [f"mtllib {self.material_file}", f"o {self.name}"]
        if self.vertices:
            lines.append("g powerlines")
            lines.append("usemtl powerline")
            lines.extend(
                f"v {x:.3f} {y:.3f} {z:.3f}"
                for x, y, z in (to_obj_coordinates(vertex) for vertex in self.vertices)
            )
            # OSG's OBJ reader imports line elements as GL_LINES. Each adjacent
            # pair is therefore one segment. Keep source lines short enough for
            # the plugin's fixed-size parser; osgconv still combines them into
            # one ordinary scene-graph Geometry in the resulting OSGB.
            vertices_per_element = 512
            for first in range(1, len(self.vertices) + 1, vertices_per_element):
                last = min(first + vertices_per_element, len(self.vertices) + 1)
                lines.append("l " + " ".join(str(index) for index in range(first, last)))

        if self.road_quads:
            # Give every quad its own four vertices so the OBJ importer can
            # collapse the complete road grid into one ordinary indexed
            # Geometry. World-space UVs keep the asphalt pattern continuous
            # across independently authored strips and intersections.
            first_road_vertex = len(self.vertices) + 1
            texture_repeat_metres = 12.0
            lines.append("g roads")
            lines.append("usemtl road_surface")
            for quad in self.road_quads:
                lines.extend(
                    f"v {x:.3f} {y:.3f} {z:.3f}"
                    for x, y, z in (to_obj_coordinates(vertex) for vertex in quad)
                )
            for quad in self.road_quads:
                lines.extend(
                    f"vt {vertex[0] / texture_repeat_metres:.6f} "
                    f"{vertex[1] / texture_repeat_metres:.6f}"
                    for vertex in quad
                )
            road_normal = to_obj_coordinates((0.0, 0.0, 1.0))
            lines.append(
                f"vn {road_normal[0]:.1f} {road_normal[1]:.1f} {road_normal[2]:.1f}")
            for quad_index in range(len(self.road_quads)):
                vertex = first_road_vertex + quad_index * 4
                texcoord = 1 + quad_index * 4
                lines.append(
                    f"f {vertex}/{texcoord}/1 {vertex+1}/{texcoord+1}/1 "
                    f"{vertex+2}/{texcoord+2}/1")
                lines.append(
                    f"f {vertex}/{texcoord}/1 {vertex+2}/{texcoord+2}/1 "
                    f"{vertex+3}/{texcoord+3}/1")

        if self.smoke_positions:
            first_smoke_vertex = (
                len(self.vertices) + len(self.road_quads) * 4 + 1)
            lines.append("g smoke_stacks")
            lines.append("usemtl powerline")
            lines.extend(
                f"v {x:.3f} {y:.3f} {z:.3f}"
                for x, y, z in (
                    to_obj_coordinates(position)
                    for position in self.smoke_positions)
            )
            lines.append("p " + " ".join(
                str(first_smoke_vertex + index)
                for index in range(len(self.smoke_positions))))
        return "\n".join(lines) + "\n"


def subtract(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def to_obj_coordinates(value):
    """Inverse of OSG OBJ's default (x, y, z) -> (x, -z, y) rotation."""
    return value[0], value[2], -value[1]


def cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def make_models() -> None:
    bricks = (
        ("brick_red", "brick_red.jpg", (0.75, 0.62, 0.55)),
        ("brick_weathered", "brick_weathered.jpg", (0.68, 0.62, 0.57)),
        ("brick_gray", "brick_gray.jpg", (0.67, 0.67, 0.66)),
    )
    for name, texture, color in bricks:
        write_text(MODELS / f"{name}.mtl", material("brick", color, texture))
        write_text(MODELS / f"{name}.obj", quad_obj(f"{name}.mtl", "brick", roof=False))

    write_text(MODELS / "roof_gray.mtl", material("roof", (0.67, 0.67, 0.66), "brick_gray.jpg"))
    write_text(MODELS / "roof_gray.obj", quad_obj("roof_gray.mtl", "roof", roof=True))

    write_text(
        MODELS / "tree.mtl",
        material("trunk", (0.24, 0.12, 0.045)) + material("leaves", (0.08, 0.42, 0.12)),
    )
    tree = ObjBuilder("tree.mtl", "unit_tree")
    tree.box("trunk", (-0.07, -0.07, 0.0), (0.07, 0.07, 0.48))
    equator = [(0.42 * math.cos(i * math.pi / 4), 0.42 * math.sin(i * math.pi / 4), 0.62) for i in range(8)]
    for i in range(8):
        j = (i + 1) % 8
        tree.triangle("leaves", (0, 0, 1.0), equator[i], equator[j])
        tree.triangle("leaves", (0, 0, 0.34), equator[j], equator[i])
    write_text(MODELS / "tree.obj", tree.text())

    write_text(
        MODELS / "hydrant.mtl",
        material("hydrant_red", (0.72, 0.035, 0.025)) + material("hydrant_dark", (0.12, 0.03, 0.02)),
    )
    hydrant = ObjBuilder("hydrant.mtl", "unit_fire_hydrant")
    hydrant.cylinder("hydrant_dark", 0.27, 0.27, 0.0, 0.10)
    hydrant.cylinder("hydrant_red", 0.19, 0.19, 0.10, 0.70)
    hydrant.cylinder("hydrant_red", 0.25, 0.16, 0.70, 0.88)
    hydrant.cylinder("hydrant_dark", 0.08, 0.08, 0.88, 1.0)
    hydrant.box("hydrant_red", (-0.38, -0.12, 0.42), (0.38, 0.12, 0.62))
    write_text(MODELS / "hydrant.obj", hydrant.text())

    write_text(
        MODELS / "window.mtl",
        material("window_glass", (0.055, 0.105, 0.135)) +
        material("window_frame", (0.10, 0.105, 0.11)),
    )
    window = ObjBuilder("window.mtl", "recessed_window")
    # The wall is at Y=0 and outward is -Y. The pane sits just off the wall;
    # the frame projects farther out and has four inner reveal faces. Since
    # every upper wall module receives a window, omit hidden backs and outer
    # frame sides to keep the model at 18 triangles instead of 60.
    pane_y = -0.045
    frame_y = -0.13
    window.quad(
        "window_glass",
        (-0.40, pane_y, 0.14), (0.40, pane_y, 0.14),
        (0.40, pane_y, 1.06), (-0.40, pane_y, 1.06))
    window.quad(
        "window_frame",
        (-0.50, frame_y, 0.04), (-0.40, frame_y, 0.04),
        (-0.40, frame_y, 1.16), (-0.50, frame_y, 1.16))
    window.quad(
        "window_frame",
        (0.40, frame_y, 0.04), (0.50, frame_y, 0.04),
        (0.50, frame_y, 1.16), (0.40, frame_y, 1.16))
    window.quad(
        "window_frame",
        (-0.40, frame_y, 0.04), (0.40, frame_y, 0.04),
        (0.40, frame_y, 0.14), (-0.40, frame_y, 0.14))
    window.quad(
        "window_frame",
        (-0.40, frame_y, 1.06), (0.40, frame_y, 1.06),
        (0.40, frame_y, 1.16), (-0.40, frame_y, 1.16))
    window.quad(
        "window_frame",
        (-0.40, frame_y, 0.14), (-0.40, pane_y, 0.14),
        (-0.40, pane_y, 1.06), (-0.40, frame_y, 1.06))
    window.quad(
        "window_frame",
        (0.40, pane_y, 0.14), (0.40, frame_y, 0.14),
        (0.40, frame_y, 1.06), (0.40, pane_y, 1.06))
    window.quad(
        "window_frame",
        (-0.40, pane_y, 0.14), (-0.40, frame_y, 0.14),
        (0.40, frame_y, 0.14), (0.40, pane_y, 0.14))
    window.quad(
        "window_frame",
        (-0.40, frame_y, 1.06), (-0.40, pane_y, 1.06),
        (0.40, pane_y, 1.06), (0.40, frame_y, 1.06))
    write_text(MODELS / "window.obj", window.text())

    write_text(
        MODELS / "door.mtl",
        material("door_panel", (0.24, 0.095, 0.045)) +
        material("door_frame", (0.085, 0.060, 0.045)) +
        material("door_handle", (0.62, 0.46, 0.12)),
    )
    door = ObjBuilder("door.mtl", "building_door")
    door.box("door_panel", (-0.62, -0.045, 0.0), (0.62, -0.020, 2.25))
    door.box("door_frame", (-0.72, -0.14, 0.0), (-0.62, -0.045, 2.38))
    door.box("door_frame", (0.62, -0.14, 0.0), (0.72, -0.045, 2.38))
    door.box("door_frame", (-0.62, -0.14, 2.25), (0.62, -0.045, 2.38))
    door.box("door_handle", (0.39, -0.19, 1.00), (0.48, -0.10, 1.11))
    write_text(MODELS / "door.obj", door.text())

    write_text(
        MODELS / "fire_escape.mtl",
        material("fire_escape_metal", (0.46, 0.18, 0.045)),
    )
    fire_escape = ObjBuilder("fire_escape.mtl", "fire_escape_module")
    fire_escape.box("fire_escape_metal", (-1.40, -0.92, -0.08), (1.40, -0.05, 0.08))
    for post_x in (-1.32, 0.0, 1.32):
        fire_escape.box(
            "fire_escape_metal",
            (post_x - 0.035, -0.92, 0.02),
            (post_x + 0.035, -0.84, 1.05))
    fire_escape.box("fire_escape_metal", (-1.35, -0.93, 0.94), (1.35, -0.83, 1.04))
    fire_escape.box("fire_escape_metal", (-1.35, -0.93, 0.48), (1.35, -0.86, 0.56))
    # A compact ladder descends to the next module/floor.
    for rail_x in (0.86, 1.16):
        fire_escape.box(
            "fire_escape_metal",
            (rail_x - 0.035, -1.00, -2.85),
            (rail_x + 0.035, -0.91, 0.02))
    for rung in range(8):
        rung_z = -2.65 + rung * 0.36
        fire_escape.box(
            "fire_escape_metal",
            (0.83, -1.01, rung_z),
            (1.19, -0.90, rung_z + 0.055))
    write_text(MODELS / "fire_escape.obj", fire_escape.text())

    write_text(
        MODELS / "trash_can.mtl",
        material("trash_body", (0.095, 0.16, 0.12)) +
        material("trash_lid", (0.055, 0.075, 0.060)),
    )
    trash_can = ObjBuilder("trash_can.mtl", "street_trash_can")
    trash_can.cylinder("trash_body", 0.34, 0.31, 0.0, 0.82, sides=8)
    trash_can.cylinder("trash_lid", 0.38, 0.30, 0.82, 0.96, sides=8)
    trash_can.box("trash_lid", (-0.09, -0.05, 0.96), (0.09, 0.05, 1.04))
    write_text(MODELS / "trash_can.obj", trash_can.text())


def instance(
    model: str,
    x: float,
    y: float,
    z: float,
    sx: float,
    sy: float,
    sz: float,
    yaw=0.0,
    min_range=0.0,
    max_range=MAX_INSTANCE_RANGE,
    tint=(1.0, 1.0, 1.0),
) -> str:
    half = 0.5 * yaw
    return (
        f'instance "{model}" {x:.3f} {y:.3f} {z:.3f} '
        f"0 0 {math.sin(half):.7f} {math.cos(half):.7f} {sx:.3f} {sy:.3f} {sz:.3f} "
        f"{min_range:.3f} {max_range:.7g} "
        f"{tint[0]:.5f} {tint[1]:.5f} {tint[2]:.5f}"
    )


class CityWriter:
    """Stream the debug text city and collect compact binary position batches."""

    MAGIC = b"OEKITB03"
    ENDIAN_MARKER = 0x01020304

    def __init__(self, text_path: Path, name: str, write_text_fixture: bool):
        text_path.parent.mkdir(parents=True, exist_ok=True)
        self.text_path = text_path
        self.binary_path = text_path.with_suffix(".kitcityb")
        self.impostor_path = text_path.with_name(text_path.stem + "_impostor.obj")
        self.scene_path = text_path.with_name(text_path.stem + "_scene.obj")
        self.text = (
            text_path.open("w", encoding="utf-8", newline="\n")
            if write_text_fixture else None
        )
        if self.text is None and text_path.exists():
            text_path.unlink()
        self.batches: dict[tuple, array] = {}
        self.model_counts: dict[str, int] = {}
        self.instance_count = 0
        self.building_count = 0
        self.skyscraper_count = 0
        self.cube_equivalent_count = 0
        self.neighborhood_counts: list[int] = []
        self.neighborhood_start = 0
        self.parent_position = (0.0, 0.0, 0.0)
        self.parent_yaw = 0.0
        self.impostor = ImpostorBuilder("city_impostor.mtl", text_path.stem + "_impostor")
        self.impostor_boxes = 0
        self.scene = CitySceneBuilder("city_scene.mtl", text_path.stem + "_scene")
        self.write("kitcity 3")
        self.write(f"# {name}: deterministic level-14 prototype, coordinates are local ENU metres.")
        self.write("# Models remain named references until osgEarth::Kit batches this graph.")
        self.write("# Brick instances are uniform two-metre voxels forming visible building shells.")

    def write(self, line: str) -> None:
        if self.text is not None:
            self.text.write(line + "\n")

    def begin_transform(self, name: str, x: float, y: float, z: float, yaw: float) -> None:
        half = 0.5 * yaw
        self.write(
            f'transform "{name}" {x:.3f} {y:.3f} {z:.3f} '
            f"0 0 {math.sin(half):.7f} {math.cos(half):.7f} 1 1 1"
        )
        self.parent_position = (x, y, z)
        self.parent_yaw = yaw
        self.neighborhood_start = self.instance_count

    def add_instance(
        self,
        model: str,
        x: float,
        y: float,
        z: float,
        sx: float,
        sy: float,
        sz: float,
        yaw: float = 0.0,
        min_range: float = 0.0,
        max_range: float = MAX_INSTANCE_RANGE,
        tint=(1.0, 1.0, 1.0),
    ) -> None:
        self.write(instance(
            model, x, y, z, sx, sy, sz, yaw, min_range, max_range, tint))

        # Flatten the generator's single neighborhood transform. The binary
        # file can therefore load as one KitNode, bypassing per-instance matrix
        # decomposition in Kit::createInstancedNode.
        c = math.cos(self.parent_yaw)
        s = math.sin(self.parent_yaw)
        px = self.parent_position[0] + c * x - s * y
        py = self.parent_position[1] + s * x + c * y
        pz = self.parent_position[2] + z
        total_yaw = self.parent_yaw + yaw
        half = 0.5 * total_yaw
        rotation = (0.0, 0.0, math.sin(half), math.cos(half))
        key = (model, *rotation, sx, sy, sz, min_range, max_range, *tint)
        positions = self.batches.setdefault(key, array("f"))
        positions.extend((px, py, pz))
        self.instance_count += 1
        self.model_counts[model] = self.model_counts.get(model, 0) + 1

    def add_impostor_box(
        self,
        wall_model: str,
        x0: float,
        y0: float,
        z0: float,
        x1: float,
        y1: float,
        z1: float,
    ) -> None:
        """Add four walls and a roof for one building height tier."""
        c = math.cos(self.parent_yaw)
        s = math.sin(self.parent_yaw)

        def world_point(x: float, y: float, z: float):
            return (
                self.parent_position[0] + c * x - s * y,
                self.parent_position[1] + s * x + c * y,
                self.parent_position[2] + z,
            )

        bsw = world_point(x0, y0, z0)
        bse = world_point(x1, y0, z0)
        bne = world_point(x1, y1, z0)
        bnw = world_point(x0, y1, z0)
        tsw = world_point(x0, y0, z1)
        tse = world_point(x1, y0, z1)
        tne = world_point(x1, y1, z1)
        tnw = world_point(x0, y1, z1)
        material_name = {
            "brick_red": "impostor_red",
            "brick_weathered": "impostor_weathered",
            "brick_gray": "impostor_gray",
            "building": "impostor_residential",
        }[wall_model]
        self.impostor.box(
            material_name,
            "impostor_roof",
            (bsw, bse, bne, bnw, tsw, tse, tne, tnw),
        )
        self.impostor_boxes += 1

    def add_smoke_stack(self, x: float, y: float, z: float) -> None:
        """Flatten one rooftop marker into the generic scene companion."""
        c = math.cos(self.parent_yaw)
        s = math.sin(self.parent_yaw)
        self.scene.add_smoke_stack((
            self.parent_position[0] + c * x - s * y,
            self.parent_position[1] + s * x + c * y,
            self.parent_position[2] + z,
        ))

    def end_transform(self) -> None:
        self.write("end")
        self.neighborhood_counts.append(self.instance_count - self.neighborhood_start)
        self.parent_position = (0.0, 0.0, 0.0)
        self.parent_yaw = 0.0

    def close(self) -> None:
        if self.text is not None:
            self.text.close()
        with self.binary_path.open("wb") as output:
            output.write(struct.pack(
                "<8sIIQ", self.MAGIC, self.ENDIAN_MARKER,
                len(self.batches), self.instance_count))
            for key, positions in self.batches.items():
                model = key[0].encode("utf-8")
                batch_values = key[1:]
                count = len(positions) // 3
                output.write(struct.pack("<I", len(model)))
                output.write(model)
                output.write(struct.pack("<12fQ", *batch_values, count))
                if struct.pack("=I", 1) != struct.pack("<I", 1):
                    positions = array("f", positions)
                    positions.byteswap()
                output.write(positions.tobytes())
        write_text(self.impostor_path, self.impostor.text())
        write_text(self.scene_path, self.scene.text())


def append_voxel_building(
    output: CityWriter,
    rng: random.Random,
    detail_rng: random.Random,
    x: float,
    y: float,
    width: int,
    depth: int,
    height: int,
    wall_model: str,
    tall: bool,
    skyscraper: bool,
    voxel_size: float,
    building_tint=(1.0, 1.0, 1.0),
) -> tuple[int, tuple[float, float, float]]:
    """Append the visible shell of a stepped Minecraft-style building."""
    heights = [[height for _ in range(width)] for _ in range(depth)]

    # Insets create terraces and a blocky skyline while keeping every emitted
    # cube visible. A few low-rise buildings get a smaller rooftop penthouse.
    inset = 2 if min(width, depth) >= 9 else 1
    add_tier = skyscraper or tall or (min(width, depth) >= 8 and rng.random() < 0.42)
    if add_tier:
        if skyscraper:
            tier_height = rng.randint(8, 18)
        else:
            tier_height = rng.randint(2, 7 if tall else 3)
        for iy in range(inset, depth - inset):
            for ix in range(inset, width - inset):
                heights[iy][ix] += tier_height

        add_second_tier = (
            skyscraper and min(width, depth) >= 7 and rng.random() < 0.76
        ) or (
            not skyscraper and tall and min(width, depth) >= 9 and rng.random() < 0.48
        )
        if add_second_tier:
            second_inset = inset + (1 if skyscraper else 2)
            second_height = rng.randint(5, 12) if skyscraper else rng.randint(2, 5)
            for iy in range(second_inset, depth - second_inset):
                for ix in range(second_inset, width - second_inset):
                    heights[iy][ix] += second_height

    # Courtyards make some footprints less uniform and expose inward-facing
    # walls. Zero-height columns are simply omitted from the shell.
    if min(width, depth) >= 7 and rng.random() < 0.12:
        courtyard_inset = 2
        for iy in range(courtyard_inset, depth - courtyard_inset):
            for ix in range(courtyard_inset, width - courtyard_inset):
                heights[iy][ix] = 0

    # The far impostor uses one simple box for each distinct height tier. This
    # preserves footprints, setbacks, and skyline height without voxel faces.
    previous_height = 0
    for tier_height in sorted({value for row in heights for value in row if value > 0}):
        active = [
            (ix, iy)
            for iy in range(depth)
            for ix in range(width)
            if heights[iy][ix] >= tier_height
        ]
        min_x = min(value[0] for value in active)
        max_x = max(value[0] for value in active)
        min_y = min(value[1] for value in active)
        max_y = max(value[1] for value in active)
        output.add_impostor_box(
            wall_model,
            x + (min_x - 0.5 * width) * voxel_size,
            y + (min_y - 0.5 * depth) * voxel_size,
            previous_height * voxel_size,
            x + (max_x + 1.0 - 0.5 * width) * voxel_size,
            y + (max_y + 1.0 - 0.5 * depth) * voxel_size,
            tier_height * voxel_size,
        )
        previous_height = tier_height

    # Add a small number of reusable facade details. Keep this on a separate
    # RNG stream so adding decorations does not perturb the deterministic
    # building footprints, heights, trees, or far impostors.
    half_width = 0.5 * width * voxel_size
    half_depth = 0.5 * depth * voxel_size

    def facade_pose(side: int, along: float, z: float):
        if side == 0:  # South; the canonical detail faces outward along -Y.
            return x + along, y - half_depth, z, 0.0
        if side == 1:  # East
            return x + half_width, y + along, z, 0.5 * math.pi
        if side == 2:  # North
            return x - along, y + half_depth, z, math.pi
        return x - half_width, y - along, z, -0.5 * math.pi  # West

    def facade_half_span(side: int) -> float:
        return half_width if side in (0, 2) else half_depth

    door_side = detail_rng.randrange(4)
    door_limit = max(0.0, facade_half_span(door_side) - 1.0)
    door_along = detail_rng.uniform(-0.55 * door_limit, 0.55 * door_limit)
    door_x, door_y, door_z, door_yaw = facade_pose(
        door_side, door_along, 0.0)
    output.add_instance(
        "door", door_x, door_y, door_z, 1.0, 1.0, 1.0, door_yaw)

    escape_chance = 1.0 if skyscraper else (0.55 if tall else 0.30)
    if detail_rng.random() < escape_chance:
        escape_side = detail_rng.randrange(4)
        escape_limit = max(0.0, facade_half_span(escape_side) - 1.6)
        escape_along = detail_rng.uniform(-escape_limit, escape_limit)
        available_height = height * voxel_size
        maximum_modules = max(
            0, int((available_height - 4.0) / 2.9) + 1)
        requested_modules = 8 if skyscraper else (3 if tall else 1)
        for module in range(min(maximum_modules, requested_modules)):
            escape_z = 2.9 + module * 2.9
            escape_x, escape_y, escape_z, escape_yaw = facade_pose(
                escape_side, escape_along, escape_z)
            output.add_instance(
                "fire_escape", escape_x, escape_y, escape_z,
                1.0, 1.0, 1.0, escape_yaw)

    scale = voxel_size / 0.5
    count = 0
    for iy in range(depth):
        for ix in range(width):
            column_height = heights[iy][ix]
            for iz in range(column_height):
                px = x + (ix - 0.5 * (width - 1)) * voxel_size
                py = y + (iy - 0.5 * (depth - 1)) * voxel_size
                pz = iz * voxel_size

                exposed = iz == column_height - 1
                exposed = exposed or ix == 0 or heights[iy][ix - 1] <= iz
                exposed = exposed or ix == width - 1 or heights[iy][ix + 1] <= iz
                exposed = exposed or iy == 0 or heights[iy - 1][ix] <= iz
                exposed = exposed or iy == depth - 1 or heights[iy + 1][ix] <= iz
                if exposed:
                    output.cube_equivalent_count += 1

                # The canonical wall normal is -Y. Rotate and offset a quad for
                # each exposed side instead of drawing all six faces of a cube.
                if iy == 0 or heights[iy - 1][ix] <= iz:
                    output.add_instance(
                        wall_model, px, py - 0.5 * voxel_size, pz,
                        scale, scale, scale, 0.0, tint=building_tint)
                    if iz > 0:
                        output.add_instance(
                            "window", px, py - 0.5 * voxel_size, pz + 0.40,
                            1.0, 1.0, 1.0, 0.0,
                            max_range=WINDOW_MAX_RANGE)
                    count += 1
                if ix == width - 1 or heights[iy][ix + 1] <= iz:
                    output.add_instance(
                        wall_model, px + 0.5 * voxel_size, py, pz,
                        scale, scale, scale, 0.5 * math.pi, tint=building_tint)
                    if iz > 0:
                        output.add_instance(
                            "window", px + 0.5 * voxel_size, py, pz + 0.40,
                            1.0, 1.0, 1.0, 0.5 * math.pi,
                            max_range=WINDOW_MAX_RANGE)
                    count += 1
                if iy == depth - 1 or heights[iy + 1][ix] <= iz:
                    output.add_instance(
                        wall_model, px, py + 0.5 * voxel_size, pz,
                        scale, scale, scale, math.pi, tint=building_tint)
                    if iz > 0:
                        output.add_instance(
                            "window", px, py + 0.5 * voxel_size, pz + 0.40,
                            1.0, 1.0, 1.0, math.pi,
                            max_range=WINDOW_MAX_RANGE)
                    count += 1
                if ix == 0 or heights[iy][ix - 1] <= iz:
                    output.add_instance(
                        wall_model, px - 0.5 * voxel_size, py, pz,
                        scale, scale, scale, -0.5 * math.pi, tint=building_tint)
                    if iz > 0:
                        output.add_instance(
                            "window", px - 0.5 * voxel_size, py, pz + 0.40,
                            1.0, 1.0, 1.0, -0.5 * math.pi,
                            max_range=WINDOW_MAX_RANGE)
                    count += 1

                if iz == column_height - 1:
                    output.add_instance(
                        "roof_gray", px, py, (iz + 1) * voxel_size,
                        scale, scale, scale)
                    count += 1
    maximum_height = max(max(row) for row in heights)
    roof_columns = [
        (ix, iy)
        for iy in range(depth)
        for ix in range(width)
        if heights[iy][ix] == maximum_height
    ]
    roof_ix, roof_iy = min(
        roof_columns,
        key=lambda value: (
            abs(value[0] - 0.5 * (width - 1)) +
            abs(value[1] - 0.5 * (depth - 1))),
    )
    return count, (
        x + (roof_ix - 0.5 * (width - 1)) * voxel_size,
        y + (roof_iy - 0.5 * (depth - 1)) * voxel_size,
        maximum_height * voxel_size,
    )


def city(output: CityWriter, name: str, blocks: int, per_side: int, stride: float, seed: int, tall: bool) -> None:
    rng = random.Random(seed)
    detail_rng = random.Random(seed ^ 0x5F3759DF)
    origin = -0.5 * stride * (blocks - 1)
    lot_span = stride * 0.72
    lot_stride = lot_span / per_side
    voxel_size = 2.0
    brick_names = ("brick_red", "brick_weathered", "brick_gray")
    building_tints = (
        (1.00, 1.00, 1.00),
        (0.94, 0.78, 0.68),
        (0.72, 0.85, 1.00),
        (0.78, 0.96, 0.73),
        (1.00, 0.73, 0.78),
        (0.82, 0.76, 1.00),
    )

    for row in range(blocks):
        for col in range(blocks):
            block_x = origin + col * stride
            block_y = origin + row * stride
            block_yaw = rng.uniform(-0.025, 0.025) if name != "downtown" else 0.0
            output.begin_transform(f"block_{row}_{col}", block_x, block_y, 0.0, block_yaw)

            for lot_y in range(per_side):
                for lot_x in range(per_side):
                    x = -0.5 * lot_span + (lot_x + 0.5) * lot_stride
                    y = -0.5 * lot_span + (lot_y + 0.5) * lot_stride
                    width = max(5, round(lot_stride * rng.uniform(0.46, 0.68) / voxel_size))
                    depth = max(5, round(lot_stride * rng.uniform(0.46, 0.68) / voxel_size))
                    skyscraper_chance = 0.004 if name == "downtown" else (
                        0.002 if name == "grid" else 0.001)
                    skyscraper = rng.random() < skyscraper_chance
                    if skyscraper:
                        height = rng.randint(30, 58)
                    elif tall:
                        radial = math.hypot(block_x, block_y) / max(stride * blocks * 0.5, 1.0)
                        height = max(4, round(rng.uniform(6.0, 16.0) * max(0.40, 1.12 - radial)))
                    else:
                        height = rng.randint(3, 8)
                    model = brick_names[(row * 5 + col * 3 + lot_x + lot_y + seed) % len(brick_names)]
                    building_tint = building_tints[
                        (row * 11 + col * 7 + lot_x * 3 + lot_y + seed) %
                        len(building_tints)
                    ]
                    _, roof_position = append_voxel_building(
                        output, rng, detail_rng, x, y, width, depth, height,
                        model, tall, skyscraper, voxel_size, building_tint)
                    if skyscraper and len(output.scene.smoke_positions) < 4:
                        output.add_smoke_stack(
                            roof_position[0], roof_position[1], roof_position[2] + 1.0)
                    output.building_count += 1
                    if skyscraper:
                        output.skyscraper_count += 1

            sidewalk = 0.43 * stride
            trees_per_side = 7
            tree_positions = []
            for index in range(trees_per_side):
                offset = -sidewalk + (index + 0.5) * (2.0 * sidewalk / trees_per_side)
                tree_positions.extend((
                    (offset, -sidewalk),
                    (sidewalk, offset),
                    (-offset, sidewalk),
                    (-sidewalk, -offset),
                ))
            for tree_x, tree_y in tree_positions:
                tree_scale = rng.uniform(5.5, 9.5)
                output.add_instance(
                    "tree",
                    tree_x + rng.uniform(-1.8, 1.8),
                    tree_y + rng.uniform(-1.8, 1.8),
                    0.0,
                    tree_scale * 0.55,
                    tree_scale * 0.55,
                    tree_scale)

            if (row + col) % 2 == 0:
                output.add_instance("hydrant", -sidewalk, sidewalk * 0.78, 0.0, 1.1, 1.1, 1.1, rng.uniform(0, math.pi))

            # Trash cans use one rotationally symmetric, fixed-scale kit model,
            # so they still collapse into a small number of instanced batches.
            for index in range(10):
                side = index % 4
                along = detail_rng.uniform(-0.82 * sidewalk, 0.82 * sidewalk)
                curb = sidewalk + detail_rng.uniform(-2.5, 2.5)
                if side == 0:
                    trash_x, trash_y = along, -curb
                elif side == 1:
                    trash_x, trash_y = curb, along
                elif side == 2:
                    trash_x, trash_y = -along, curb
                else:
                    trash_x, trash_y = -curb, -along
                output.add_instance(
                    "trash_can", trash_x, trash_y, 0.0,
                    1.0, 1.0, 1.0)
            output.end_transform()


def write_city(
    name: str,
    blocks: int,
    stride: float,
    seed: int,
    tall: bool,
    write_text_fixture: bool,
) -> CityWriter:
    output = CityWriter(CITIES / f"{name}_dense.kitcity", name, write_text_fixture)
    try:
        city(output, name, blocks=blocks, per_side=10, stride=stride, seed=seed, tall=tall)
        append_road_grid(output, blocks, stride, stride * 0.13)
        append_transmission_corridors(output)
    finally:
        output.close()
    return output


def append_road_grid(
    output: CityWriter,
    blocks: int,
    stride: float,
    road_width: float,
) -> None:
    """Fill every inter-block corridor with a non-overlapping road mesh."""
    origin = -0.5 * stride * (blocks - 1)
    extent_min = origin - 0.5 * stride
    extent_max = origin + (blocks - 0.5) * stride
    boundaries = [
        origin + (index + 0.5) * stride
        for index in range(blocks - 1)
    ]
    half_width = 0.5 * road_width

    # Horizontal strips own the intersections. Vertical strips stop at their
    # edges, avoiding coplanar overlap and the z-fighting it would cause.
    for y in boundaries:
        output.scene.add_road_quad(
            extent_min, y - half_width,
            extent_max, y + half_width)

    segment_starts = [extent_min] + [y + half_width for y in boundaries]
    segment_ends = [y - half_width for y in boundaries] + [extent_max]
    for x in boundaries:
        for y0, y1 in zip(segment_starts, segment_ends):
            output.scene.add_road_quad(
                x - half_width, y0,
                x + half_width, y1)


def append_transmission_corridors(output: CityWriter) -> None:
    """Add four correctly oriented utility corridors around one L14 tile."""
    # power_tower's crossarm spans local X, so yaw zero correctly aligns its
    # conductors with this north-south (local Y) corridor. Nine towers at 280 m
    # spacing span nearly the full 2.45 km tile while staying just inside its
    # east and west edges and clear of the generated building footprints.
    primary_corridors = [
        [(edge_x, -1120.0 + index * 280.0, 0.0) for index in range(9)]
        for edge_x in (-1160.0, 1160.0)
    ]
    for towers in primary_corridors:
        for x, y, z in towers:
            output.add_instance(
                "power_tower",
                x,
                y,
                z,
                1.0,
                1.0,
                1.0,
                0.0,
            )

    # Rotate local Y onto world X for the smaller north/south corridors. Their
    # 220 m spacing is appropriate for the 23 m tower and keeps the perimeter
    # circuits visually distinct instead of alternating unlike tower models.
    secondary_corridors = [
        [(-1100.0 + index * 220.0, edge_y, 0.0) for index in range(11)]
        for edge_y in (-1160.0, 1160.0)
    ]
    secondary_yaw = -0.5 * math.pi
    for towers in secondary_corridors:
        for x, y, z in towers:
            output.add_instance(
                "power_tower2",
                x,
                y,
                z,
                1.0,
                1.0,
                1.0,
                secondary_yaw,
            )

    def add_conductors(towers, attachments, yaw: float, sag: float) -> None:
        c = math.cos(yaw)
        s = math.sin(yaw)
        for first, second in zip(towers, towers[1:]):
            for crossarm_offset, height in attachments:
                offset_x = c * crossarm_offset
                offset_y = s * crossarm_offset
                output.scene.add_powerline_span(
                    (first[0] + offset_x, first[1] + offset_y, first[2] + height),
                    (second[0] + offset_x, second[1] + offset_y, second[2] + height),
                    sag,
                )

    # Three crossarm levels on each tower carry a balanced pair of conductors.
    # The attachment coordinates match the visible tips of the two supplied
    # tower models; the parabolic midpoint drop is intentionally modest.
    for towers in primary_corridors:
        add_conductors(
            towers,
            ((-6.2, 22.85), (6.2, 22.85),
             (-6.2, 27.85), (6.2, 27.85),
             (-3.8, 31.65), (3.8, 31.65)),
            0.0,
            8.0,
        )
    for towers in secondary_corridors:
        add_conductors(
            towers,
            ((-4.15, 15.10), (4.15, 15.10),
             (-4.15, 18.40), (4.15, 18.40),
             (-4.15, 21.70), (4.15, 21.70)),
            secondary_yaw,
            5.5,
        )


def write_residential_city(write_text_fixture: bool) -> CityWriter:
    """Write one densely packed L14 tile of complete residential models."""
    output = CityWriter(
        CITIES / "residential_dense.kitcity",
        "residential",
        write_text_fixture,
    )
    rng = random.Random(71)

    # A spherical-Mercator L14 tile is about 2446 metres wide at the equator.
    # One hundred 230 m blocks leave a 38 m street corridor between 192 m
    # residential interiors. Eight houses per side produces 6,400 residences,
    # or approximately 1,070 buildings/km^2 over the complete tile footprint.
    blocks = 10
    block_stride = 230.0
    interior_span = 192.0
    houses_per_side = 8
    lot_stride = interior_span / houses_per_side
    origin = -0.5 * block_stride * (blocks - 1)
    scales = (0.82, 0.90, 0.98)
    house_tints = (
        (1.00, 1.00, 1.00),
        (0.96, 0.80, 0.68),
        (0.72, 0.86, 1.00),
        (0.79, 0.96, 0.73),
        (1.00, 0.73, 0.79),
        (0.83, 0.77, 1.00),
        (1.00, 0.92, 0.61),
    )

    try:
        for row in range(blocks):
            for col in range(blocks):
                block_x = origin + col * block_stride
                block_y = origin + row * block_stride
                output.begin_transform(
                    f"residential_block_{row}_{col}",
                    block_x,
                    block_y,
                    0.0,
                    0.0,
                )

                for lot_y in range(houses_per_side):
                    for lot_x in range(houses_per_side):
                        x = (
                            -0.5 * interior_span +
                            (lot_x + 0.5) * lot_stride +
                            rng.uniform(-0.8, 0.8)
                        )
                        y = (
                            -0.5 * interior_span +
                            (lot_y + 0.5) * lot_stride +
                            rng.uniform(-0.8, 0.8)
                        )
                        scale = scales[(row * 7 + col * 5 + lot_x + lot_y) % len(scales)]
                        # Houses on opposite halves of a block face their
                        # nearest parallel street. Keeping these two rotations
                        # discrete preserves efficient binary/instanced batches.
                        yaw = 0.0 if lot_y < houses_per_side // 2 else math.pi
                        tint = house_tints[
                            (row * 13 + col * 7 + lot_x * 3 + lot_y) %
                            len(house_tints)
                        ]
                        output.add_instance(
                            "building",
                            x,
                            y,
                            -RESIDENTIAL_MIN_Z * scale,
                            scale,
                            scale,
                            scale,
                            yaw,
                            tint=tint,
                        )
                        output.add_impostor_box(
                            "building",
                            x - RESIDENTIAL_HALF_WIDTH * scale,
                            y - RESIDENTIAL_HALF_DEPTH * scale,
                            0.0,
                            x + RESIDENTIAL_HALF_WIDTH * scale,
                            y + RESIDENTIAL_HALF_DEPTH * scale,
                            RESIDENTIAL_HEIGHT * scale,
                        )
                        output.building_count += 1

                        if (
                            lot_x == 3 and lot_y == 3 and
                            row in (4, 5) and col in (4, 5)
                        ):
                            output.add_smoke_stack(
                                x,
                                y,
                                RESIDENTIAL_HEIGHT * scale + 1.0,
                            )

                # Three trees on each edge give every block a readable street
                # boundary without overwhelming the complete house models.
                sidewalk = 0.455 * block_stride
                for tree_index in range(3):
                    along = -0.62 * sidewalk + tree_index * 0.62 * sidewalk
                    for tree_x, tree_y in (
                        (along, -sidewalk),
                        (sidewalk, along),
                        (-along, sidewalk),
                        (-sidewalk, -along),
                    ):
                        output.add_instance(
                            "tree", tree_x, tree_y, 0.0,
                            4.4, 4.4, 8.0,
                        )

                output.end_transform()
        append_road_grid(output, blocks, block_stride, 28.0)
        append_transmission_corridors(output)
    finally:
        output.close()
    return output


def make_cities(write_text_fixture: bool) -> list[CityWriter]:
    write_text(
        CITIES / "city_impostor.mtl",
        impostor_material("impostor_red", (0.58, 0.38, 0.30)) +
        impostor_material("impostor_weathered", (0.64, 0.48, 0.37)) +
        impostor_material("impostor_gray", (0.52, 0.51, 0.50)) +
        impostor_material("impostor_residential", (0.72, 0.62, 0.48)) +
        impostor_material("impostor_roof", (0.46, 0.48, 0.51)),
    )
    write_text(
        CITIES / "city_scene.mtl",
        impostor_material("powerline", (0.18, 0.20, 0.22)) +
        material("road_surface", (1.0, 1.0, 1.0), "asphalt_road.jpg"),
    )
    return [
        write_city("grid", blocks=7, stride=315.0, seed=17, tall=False, write_text_fixture=write_text_fixture),
        write_city("downtown", blocks=7, stride=315.0, seed=29, tall=True, write_text_fixture=write_text_fixture),
        write_city("old_town", blocks=8, stride=275.0, seed=43, tall=False, write_text_fixture=write_text_fixture),
        write_residential_city(write_text_fixture),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--text",
        action="store_true",
        help="also emit the much larger human-readable .kitcity fixtures",
    )
    arguments = parser.parse_args()
    make_models()
    cities = make_cities(arguments.text)
    print(f"Wrote kit models to {MODELS}")
    for output in cities:
        binary_mb = output.binary_path.stat().st_size / (1024.0 * 1024.0)
        text_note = ""
        if output.text_path.exists():
            text_mb = output.text_path.stat().st_size / (1024.0 * 1024.0)
            text_note = f" ({text_mb:.1f} MiB text)"
        face_modules = sum(
            output.model_counts.get(model, 0)
            for model in ("brick_red", "brick_weathered", "brick_gray", "roof_gray")
        )
        impostor_mb = output.impostor_path.stat().st_size / (1024.0 * 1024.0)
        scene_kb = output.scene_path.stat().st_size / 1024.0
        print(
            f"{output.binary_path.name}: {output.building_count} buildings/"
            f"{output.skyscraper_count} skyscrapers/{output.model_counts.get('tree', 0)} trees, "
            f"{output.model_counts.get('power_tower', 0) + output.model_counts.get('power_tower2', 0)} power towers, "
            f"details {output.model_counts.get('window', 0)} windows/"
            f"{output.model_counts.get('door', 0)} doors/"
            f"{output.model_counts.get('fire_escape', 0)} fire escapes/"
            f"{output.model_counts.get('trash_can', 0)} trash cans, "
            f"{output.instance_count} instances, {binary_mb:.1f} MiB binary"
            f"{text_note}, {output.cube_equivalent_count * 12}->{face_modules * 2} "
            f"cube->quad shell triangles, neighborhood instances "
            f"{min(output.neighborhood_counts)}-{max(output.neighborhood_counts)}, "
            f"impostor {output.impostor_boxes} boxes/{output.impostor.triangle_count} triangles/"
            f"{impostor_mb:.1f} MiB, companion scene roads "
            f"{len(output.scene.road_quads)} quads/"
            f"{len(output.scene.road_quads) * 2} triangles, smoke stacks "
            f"{len(output.scene.smoke_positions)}, powerlines "
            f"{output.scene.span_count} spans/{output.scene.segment_count} segments/"
            f"{scene_kb:.1f} KiB"
        )


if __name__ == "__main__":
    main()
