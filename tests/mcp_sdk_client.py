# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in conformance run using the official MCP Python SDK, test-only."""
import asyncio
import base64
import struct
import math
import statistics
import time
import zlib
import json
import os
import sys
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp import types
from mcp.client.stdio import stdio_client
from mcp.shared.exceptions import McpError
from mcp.shared.message import SessionMessage

# Dark canvas token from docs/design/DESIGN_SYSTEM.md. These assertions verify
# rendered preview pixels as well as the model/UV content that follows them.
DARK_CANVAS = (19, 20, 24, 255)


def tool_error(result):
    """Return a tool error, which Nova reports as JSON text without structured content."""
    assert result.isError and result.structuredContent is None, result
    assert [item.type for item in result.content] == ["text"], result
    error = json.loads(result.content[0].text)["error"]
    # "details" is optional data (a JSON pointer, save outcomes, supported versions).
    assert set(error) - {"details"} == {"code", "message"} and error["code"] and error["message"], error
    assert isinstance(error.get("details", {}), dict), error
    return error


class StrictClientSession(ClientSession):
    """The Python SDK skips error results; the TypeScript SDK validates any structuredContent."""

    async def call_tool(self, name, arguments=None, *args, **kwargs):
        result = await super().call_tool(name, arguments, *args, **kwargs)
        if result.isError:
            tool_error(result)
        return result


class TranscriptStream:
    def __init__(self, stream, direction, transcript):
        self.stream = stream
        self.direction = direction
        self.transcript = transcript
        self.cancel_mutation = False
        self.mutation_id = None

    def record(self, frame):
        if isinstance(frame, Exception):
            raise frame
        self.transcript.append({
            "direction": self.direction,
            "message": frame.message.model_dump(by_alias=True, mode="json", exclude_none=True),
        })

    async def send(self, frame):
        self.record(frame)
        await self.stream.send(frame)
        message = frame.message.root
        if (isinstance(message, types.JSONRPCRequest)
                and message.method == "tools/call"
                and message.params["name"] in ("nova.texture.edit", "nova.workflow.execute")):
            self.mutation_id = message.id
            if self.cancel_mutation:
                await self.cancel_edit()

    async def cancel_edit(self):
        assert self.mutation_id is not None
        notification = types.JSONRPCNotification(
            jsonrpc="2.0", method="notifications/cancelled", params={"requestId": self.mutation_id}
        )
        await self.send(SessionMessage(types.JSONRPCMessage(notification)))

    async def __aiter__(self):
        async for frame in self.stream:
            self.record(frame)
            yield frame

    async def __aenter__(self):
        await self.stream.__aenter__()
        return self

    async def __aexit__(self, *args):
        return await self.stream.__aexit__(*args)


async def edit(client: ClientSession) -> None:
    async def call(tool, args=None):
        response = await client.call_tool(tool, args)
        assert not response.isError, response.structuredContent
        return response.structuredContent

    info = await call("nova.project.info")
    generation = {"generation": info["generation"]}
    listing = await call("nova.assets.list", generation)
    texture = next(row for row in listing["assets"] if row["type"] == "texture")
    target = {**generation, "assetId": texture["assetId"]}
    capabilities = await call("nova.edit.capabilities", target)
    assert capabilities["permission"] == "editing"
    document = capabilities["document"]
    target.update(projectId=info["projectId"], expectedRevision=document["revision"])
    layer = document["layers"][0]["id"]
    model = next(row for row in listing["assets"] if row["path"].endswith("/block/a.json"))
    group = {**generation, "projectId": info["projectId"],
             "assetIds": [texture["assetId"], model["assetId"]]}
    context = await call("nova.assets.context", group)
    assert len(context["assets"]) == 2
    batch = await call("nova.assets.inspect_batch", group)
    assert batch["results"][0]["metadata"]["width"] == 16
    prepare = await call("nova.workflow.prepare", group)
    assert prepare["results"][0]["revision"] == document["revision"]
    operations = [
        {"command": "stroke", "layer": layer, "points": [[2, 2]], "size": 1,
         "color": "#123456", "opacity": 255},
        {"command": "layer_rename", "layer": layer, "name": "SDK edit"},
    ]
    preview = await call("nova.edit.preview", {**target, "operations": operations})
    assert preview["revision"] == document["revision"] and len(preview["operations"]) == 2
    result = await call("nova.edit.batch", {**target, "operations": operations})
    validated = await call("nova.workflow.validate_changes", {
        **group, "expectedRevisions": [{"assetId": texture["assetId"], "revision": result["revision"]}],
    })
    assert validated["files"] == 2
    stale = await client.call_tool("nova.edit.preview", {**target, "operations": operations})
    assert tool_error(stale)["code"] == "revision_conflict"
    assert result["changed"] and result["undoAvailable"] and result["appliedOperations"] == 2
    undone = await call("nova.edit.undo", {**target, "expectedRevision": result["revision"]})
    assert undone["redoAvailable"]
    redone = await call("nova.edit.redo", {**target, "expectedRevision": undone["revision"]})
    assert redone["layers"][0]["name"] == "SDK edit"
    print(json.dumps({"sdk_edit": "passed", "batch": result["appliedOperations"],
                      "undo": undone["changed"], "redo": redone["changed"]}))


async def preview_lifecycle(client: ClientSession, reconnect=False) -> None:
    async def call(tool, args=None, error=None):
        for attempt in range(100):
            result = await client.call_tool(tool, args)
            unavailable = result.isError and tool_error(result)["code"] == "validation_unavailable"
            if not unavailable or tool not in ("nova.asset.preview_image", "nova.edit.capabilities",
                                               "nova.project.info", "nova.assets.list"):
                break
            await asyncio.sleep(0.05)
        if error:
            assert tool_error(result)["code"] == error, result
            assert all(item.type != "image" for item in result.content)
        else:
            assert not result.isError, result.structuredContent
        return result

    async def boundary(action):
        print(action, flush=True)
        assert (await asyncio.to_thread(sys.stdin.readline)).strip() == "continue"

    info = (await call("nova.project.info")).structuredContent
    project = {key: info[key] for key in ("generation", "projectId")}
    assets = (await call("nova.assets.list", {"generation": project["generation"]})).structuredContent["assets"]
    ids = {Path(row["path"]).stem: row["assetId"] for row in assets if row["path"].endswith(".json")}

    async def state(asset):
        return (await call("nova.edit.capabilities", {"generation": project["generation"],
                           "assetId": asset})).structuredContent["document"]

    async def preview(asset, revision, view, error=None, **extra):
        result = await call("nova.asset.preview_image", {**project, "assetId": asset, "view": view,
                            "expectedRevision": revision, **extra}, error)
        if error:
            return None
        assert result.structuredContent["revision"] == revision
        assert result.structuredContent["assetId"] == asset
        data = base64.b64decode(next(item.data for item in result.content if item.type == "image"))
        assert struct.unpack(">II", data[16:24]) == (256, 256)
        return decode_png(data, 256, 256)

    if reconnect:
        asset = ids["lifecycle"]
        reopened = await state(asset)
        assert not reopened["dirty"]
        for view in ("model", "uv"):
            assert len(set(await preview(asset, reopened["revision"], view))) > 4
        await call("nova.uv.edit", {**project, "assetId": asset, "expectedRevision": reopened["revision"],
                   "operation": {"command": "uv_set", "element": 0, "face": "down",
                                 "uv": [2, 3, 9, 11], "rotation": 90}}, "permission_denied")
        return

    created = (await call("nova.asset.create", {
        **project, "indexRevision": info["indexRevision"], "namespace": "nova",
        "path": "block/lifecycle", "conflictPolicy": "reject",
        "parameters": {"command": "model", "cubeTexture": "nova:block/seed"},
    })).structuredContent
    asset = created["assetId"]
    revision = created["document"]["revision"]
    viewport = await preview(asset, revision, "model")
    uv = await preview(asset, revision, "uv")
    assert viewport[0] == DARK_CANVAS and len(set(viewport)) > 4
    assert uv[128 * 256 + 64] == (199, 51, 66, 255)
    assert uv[128 * 256 + 192] == (50, 173, 125, 255)
    await preview(asset, revision, "uv", "invalid_arguments", faceIndex=1536)
    await preview(asset, revision, "model", "invalid_arguments", width=4096)

    await boundary("save")
    clean = await state(asset)
    assert not clean["dirty"] and clean["revision"] != revision
    for view in ("model", "uv"):
        await preview(asset, revision, view, "revision_conflict")
        assert await preview(asset, clean["revision"], view) == (viewport if view == "model" else uv)

    await boundary("close")
    for view in ("model", "uv"):
        await preview(asset, clean["revision"], view, "document_not_open")

    await boundary("reopen")
    reopened = await state(asset)
    assert reopened["revision"] != clean["revision"] and not reopened["dirty"]
    for view in ("model", "uv"):
        await preview(asset, clean["revision"], view, "revision_conflict")
        assert await preview(asset, reopened["revision"], view) == (viewport if view == "model" else uv)

    await boundary("select-race")
    await preview(asset, reopened["revision"], "model", "preview_unavailable")

    for name in ("empty", "invalid", "oversized"):
        await boundary(name)
        other = await state(ids[name])
        for view in ("model", "uv"):
            await preview(asset, reopened["revision"], view, "preview_unavailable")
            await preview(ids[name], other["revision"], view, "preview_unavailable")

    await boundary("switch")
    for view in ("model", "uv"):
        await preview(asset, reopened["revision"], view, "stale_project")

    await boundary("revoke")
    try:
        await preview(asset, reopened["revision"], "uv")
    except McpError as error:
        assert error.error.code == types.CONNECTION_CLOSED
    else:
        raise AssertionError("revoked pipe accepted a preview")


async def restored_preview(client: ClientSession) -> None:
    async def call(tool, args=None):
        result = await client.call_tool(tool, args)
        assert not result.isError, result.structuredContent
        return result

    info = (await call("nova.project.info")).structuredContent
    project = {key: info[key] for key in ("generation", "projectId")}
    assets = (await call("nova.assets.list", {"generation": project["generation"]})).structuredContent["assets"]
    asset = next(row["assetId"] for row in assets if row["path"].endswith("/sdk_model.json"))
    target = {**project, "assetId": asset}
    state = (await call("nova.edit.capabilities", {"generation": project["generation"], "assetId": asset})).structuredContent["document"]
    assert state["dirty"] and state["elementCount"] == 1 and not state["undoAvailable"]
    for view in ("model", "uv"):
        result = await call("nova.asset.preview_image", {**target, "view": view, "expectedRevision": state["revision"]})
        assert result.structuredContent["revision"] == state["revision"]
        data = base64.b64decode(next(item.data for item in result.content if item.type == "image"))
        assert struct.unpack(">II", data[16:24]) == (256, 256)
        pixels = decode_png(data, 256, 256)
        assert len(set(pixels)) > 4
        assert pixels[0] == (DARK_CANVAS if view == "model" else (199, 51, 66, 255))
    print("restored model and UV PNGs decoded")


async def phase4e_pack(client: ClientSession) -> None:
    async def call(tool, args=None):
        response = await client.call_tool(tool, args)
        assert not response.isError, response.structuredContent
        return response.structuredContent

    async def boundary(name):
        print(name, flush=True)
        assert (await asyncio.to_thread(sys.stdin.readline)).strip() == "continue"

    info = await call("nova.project.info")
    project = {key: info[key] for key in ("generation", "projectId")}

    async def create(path, parameters):
        info = await call("nova.project.info")
        return await call("nova.asset.create", {**project, "indexRevision": info["indexRevision"],
                          "namespace": "minecraft", "path": path, "conflictPolicy": "reject",
                          "parameters": parameters})

    async def validate(targets, results):
        validation = await call("nova.workflow.validate_changes", {
            **project, "assetIds": [target["assetId"] for target in targets],
            "expectedRevisions": [{"assetId": target["assetId"], "revision": result["revision"]}
                                  for target, result in zip(targets, results, strict=True)],
        })
        assert not validation["counts"].get("Error"), validation
        print(json.dumps(validation), flush=True)

    texture = await create("block/gold_block", {"command": "texture", "width": 16, "height": 16,
                           "fill": "#f528ab", "layerName": "Magenta and green", "pixelFormat": "rgba8888"})
    assets = (await call("nova.assets.list", {"generation": project["generation"]}))["assets"]
    animation = next(asset for asset in assets if asset["path"].endswith("/diamond_block.png"))
    animation["document"] = (await call("nova.edit.capabilities", {
        "generation": project["generation"], "assetId": animation["assetId"],
    }))["document"]

    def stage(asset, operations):
        return {"assetId": asset["assetId"], "expectedRevision": asset["document"]["revision"],
                "operations": operations}

    def fill(asset, x, width, color, y=0, height=16):
        return {"command": "fill_rect", "layer": asset["document"]["activeLayer"],
                "x": x, "y": y, "width": width, "height": height, "color": color, "opacity": 255}

    # A stale stage must reject the workflow before any stage commits.
    stale = [stage(texture, [fill(texture, 0, 1, "#000000")]),
             {**stage(animation, [fill(animation, 0, 1, "#000000")]), "expectedRevision": "0" * 64}]
    response = await client.call_tool("nova.workflow.execute", {**project, "stages": stale})
    rejected = response.structuredContent or {}
    assert response.isError or rejected.get("status") != "complete", rejected
    await boundary("review-rejected")

    # The blue quadrant makes UV rotation visible on the model's south face in game.
    stages = [stage(texture, [fill(texture, 8, 8, "#52ef25"), fill(texture, 0, 8, "#2742f5", 0, 8)]),
              stage(animation, [fill(animation, 0, 16, "#f37c12"),
                                {"command": "animation_timing", "ticks": 40, "interpolate": False}])]
    plan = await call("nova.workflow.plan", {**project, "stages": stages})
    assert plan["status"] == "ready"
    await boundary("review-ready")
    workflow = await call("nova.workflow.execute", {**project, "stages": stages})
    assert workflow["status"] == "complete", workflow
    await validate(stages, workflow["stages"])
    await boundary("review-raster")

    model = await create("block/bricks", {"command": "model", "cubeTexture": "minecraft:block/gold_block"})
    model_stage = stage(model, [
        {"command": "uv_set", "element": 0, "face": "south", "uv": [0, 0, 8, 16], "rotation": 90},
        {"command": "model_transform", "element": 0, "from": [0, 0, 0], "to": [16, 8, 16],
         "rotation": {"enabled": False}},
    ])
    result = await call("nova.edit.batch", {**project, **model_stage})
    await validate([model_stage], [result])
    await boundary("review-model")
    print("Minecraft pack MCP authoring complete", flush=True)


async def phase4d(client: ClientSession) -> None:
    async def call(tool, args=None, error=None):
        response = await client.call_tool(tool, args)
        if error:
            assert tool_error(response)["code"] == error
        else:
            assert not response.isError, response.structuredContent
        return response

    info = (await call("nova.project.info")).structuredContent
    project = {key: info[key] for key in ("generation", "projectId")}
    listing = (await call("nova.assets.list", {"generation": info["generation"]})).structuredContent
    asset = next(item for item in listing["assets"] if item["type"] == "texture")
    namespace = asset["namespace"]
    directory = asset["path"].split("/textures/")[1].rsplit("/", 1)[0]
    targets = []
    stages = []
    for kind in ("texture", "animation"):
        info = (await call("nova.project.info")).structuredContent
        parameters = {"command": kind, "width": 16, "height": 16, "fill": "#123456",
                      "layerName": "SDK base", "pixelFormat": "rgba8888"}
        if kind == "animation":
            parameters.update(frames=3, ticks=4, interpolate=False)
        args = {**project, "indexRevision": info["indexRevision"], "namespace": namespace,
                "path": f"{directory}/sdk_{kind}", "conflictPolicy": "reject", "parameters": parameters}
        await call("nova.asset.create_preview", {**args, "path": "../escape"}, "invalid_asset_path")
        preview = (await call("nova.asset.create_preview", args)).structuredContent
        after = (await call("nova.project.info")).structuredContent
        assert after["assetCount"] == info["assetCount"] and after["indexRevision"] == info["indexRevision"]
        start = time.perf_counter()
        created = (await call("nova.asset.create", args)).structuredContent
        print(f"phase4d_create_{kind}_ms={(time.perf_counter() - start) * 1000:.3f}")
        assert created["provisional"] and created["assetId"] == preview["assetId"]
        doc = created["document"]
        assert doc["dirty"] and doc["layers"][0]["name"] == "SDK base"
        await call("nova.asset.create", args, "stale_index")
        fresh = (await call("nova.project.info")).structuredContent
        await call("nova.asset.create", {**args, "indexRevision": fresh["indexRevision"]}, "asset_conflict")
        await call("nova.asset.create_preview", {
            **args, "indexRevision": fresh["indexRevision"],
            "parameters": {**parameters, "width": 257}
        }, "invalid_arguments")
        target = {"assetId": created["assetId"], "expectedRevision": doc["revision"]}
        targets.append(target)
        operation = {"command": "fill_rect", "layer": doc["layers"][0]["id"],
                     "x": 0, "y": 0, "width": 16, "height": 16, "color": "#654321", "opacity": 255}
        if kind == "animation":
            operation = {"command": "animation_duration", "entry": 1, "ticks": 9}
        stages.append({**target, "operations": [operation]})

    request = {**project, "stages": stages}
    start = time.perf_counter()
    plan = (await call("nova.workflow.plan", request)).structuredContent
    print(f"phase4d_plan_ms={(time.perf_counter() - start) * 1000:.3f}")
    assert plan["status"] == "ready" and len(plan["stages"]) == 2
    await call("nova.workflow.plan", {**project, "stages": stages * 3}, "invalid_arguments")
    await call("nova.workflow.plan", {**project, "stages": stages[:1] * 2}, "invalid_arguments")
    start = time.perf_counter()
    workflow = (await call("nova.workflow.execute", request)).structuredContent
    print(f"phase4d_execute_ms={(time.perf_counter() - start) * 1000:.3f}")
    assert workflow["status"] == "complete", workflow
    for target, stage in zip(targets, workflow["stages"], strict=True):
        await call("nova.asset.preview_image", {**project, **target}, "revision_conflict")
        request = {**project, **target, "expectedRevision": stage["revision"]}
        validated = (await call("nova.workflow.validate_changes", {
            **project, "assetIds": [target["assetId"]],
            "expectedRevisions": [{"assetId": target["assetId"], "revision": stage["revision"]}],
        })).structuredContent
        assert validated["files"] == 1 and not validated["counts"].get("Error")
        frame = {"frameId": stage["frames"][1]["id"]} if stage["animated"] else {}
        samples = []
        for _ in range(8):
            start = time.perf_counter()
            image = await call("nova.asset.preview_image", {**request, **frame})
            samples.append(round((time.perf_counter() - start) * 1000, 3))
            png = next(item for item in image.content if item.type == "image")
            data = base64.b64decode(png.data)
            assert png.mimeType == "image/png" and data.startswith(b"\x89PNG\r\n\x1a\n")
            assert struct.unpack(">II", data[16:24]) == (256, 256)
            assert data[24:26] == bytes((8, 6))
            offset = 8
            idat = bytearray()
            while offset + 12 <= len(data):
                length, chunk = struct.unpack(">I4s", data[offset:offset + 8])
                if chunk == b"IDAT":
                    idat.extend(data[offset + 8:offset + 8 + length])
                offset += length + 12
            # All PNG filters have zero predictors at the first pixel.
            expected = bytes.fromhex("123456ff" if frame else "654321ff")
            assert zlib.decompress(idat)[1:5] == expected
            assert image.structuredContent["revision"] == stage["revision"]
            assert image.structuredContent["frameId"] == frame.get("frameId", "")
        print(f"phase4d_preview_{'animation' if frame else 'texture'}_ms={samples}")
        start = time.perf_counter()
        undone = (await call("nova.edit.undo", request)).structuredContent
        print(f"phase4d_undo_ms={(time.perf_counter() - start) * 1000:.3f}")
        assert undone["redoAvailable"]
        redone = (await call("nova.edit.redo", {**request, "expectedRevision": undone["revision"]})).structuredContent
        assert redone["dirty"] and redone["undoAvailable"]


    info = (await call("nova.project.info")).structuredContent
    model_args = {**project, "indexRevision": info["indexRevision"], "namespace": namespace,
                  "path": f"{directory}/sdk_model", "conflictPolicy": "reject",
                  "parameters": {"command": "model", "cubeTexture": f"{namespace}:{directory}/seed"}}
    await call("nova.asset.create_preview", {**model_args, "path": "/escape"}, "invalid_asset_path")
    await call("nova.asset.create_preview", {**model_args, "namespace": ".."}, "invalid_asset_path")
    await call("nova.asset.create_preview", {**model_args, "parameters": {
        **model_args["parameters"], "parent": "../escape",
    }}, "invalid_model")
    await call("nova.asset.create_preview", {**model_args, "parameters": {
        **model_args["parameters"], "cubeTexture": "x" * 201,
    }}, "invalid_arguments")
    await call("nova.asset.create_preview", {**model_args, "path": "../escape"}, "invalid_asset_path")
    preview = (await call("nova.asset.create_preview", model_args)).structuredContent
    assert (await call("nova.project.info")).structuredContent["indexRevision"] == info["indexRevision"]
    created = (await call("nova.asset.create", model_args)).structuredContent
    assert created["provisional"] and created["assetId"] == preview["assetId"]
    fresh = (await call("nova.project.info")).structuredContent
    await call("nova.asset.create", {**model_args, "indexRevision": fresh["indexRevision"]}, "asset_conflict")
    model = {**project, "assetId": created["assetId"], "expectedRevision": created["document"]["revision"]}
    metadata = (await call("nova.model.inspect", {"generation": project["generation"],
                            "assetId": model["assetId"]})).structuredContent["metadata"]
    assert metadata["source"] == "live" and metadata["faceCount"] == 6
    related = (await call("nova.assets.context", {**project, "assetIds": [model["assetId"]]})).structuredContent
    assert any(ref["type"] == "texture" for ref in related["references"]), related

    async def pixels(revision, view):
        response = await call("nova.asset.preview_image", {**model, "expectedRevision": revision, "view": view})
        assert response.structuredContent["revision"] == revision
        data = base64.b64decode(next(item.data for item in response.content if item.type == "image"))
        width, height = struct.unpack(">II", data[16:24])
        assert (width, height) == (256, 256)
        return decode_png(data, width, height)

    first = model["expectedRevision"]
    viewport = await pixels(first, "model")
    assert viewport[0] == DARK_CANVAS and len(set(viewport)) > 4
    uv = await pixels(first, "uv")
    assert uv[128 * 256 + 64] == (199, 51, 66, 255)
    assert uv[128 * 256 + 192] == (50, 173, 125, 255)
    transformed = (await call("nova.model.edit", {**model, "operation": {
        "command": "model_transform", "element": 0, "from": [0, 0, 0], "to": [12, 16, 16],
        "rotation": {"enabled": False},
    }})).structuredContent
    transformed_pixels = await pixels(transformed["revision"], "model")
    assert transformed_pixels != viewport
    uv = await pixels(transformed["revision"], "uv")
    # The first native face is down.
    edited = (await call("nova.uv.edit", {**model, "expectedRevision": transformed["revision"],
        "operation": {"command": "uv_set", "element": 0, "face": "down",
                      "uv": [2, 3, 9, 11], "rotation": 90},
    })).structuredContent
    await call("nova.asset.preview_image", {**model, "view": "uv"}, "revision_conflict")
    changed = await pixels(edited["revision"], "uv")
    assert changed != uv
    report = (await call("nova.workflow.validate_changes", {**project, "assetIds": [model["assetId"]],
        "expectedRevisions": [{"assetId": model["assetId"], "revision": edited["revision"]}],
    })).structuredContent
    assert report["files"] == 1 and not report["counts"].get("Error"), report
    undone = (await call("nova.edit.undo", {**model, "expectedRevision": edited["revision"]})).structuredContent
    assert await pixels(undone["revision"], "uv") == uv
    assert await pixels(undone["revision"], "model") == transformed_pixels
    redone = (await call("nova.edit.redo", {**model, "expectedRevision": undone["revision"]})).structuredContent
    assert await pixels(redone["revision"], "uv") == changed
    print("phase4d1_model_and_uv_pixels=passed")


def decode_png(data, width, height):
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    assert data[24] == 8 and data[25] in (2, 6) and data[28] == 0
    channels = 4 if data[25] == 6 else 3
    offset = 8
    compressed = bytearray()
    while offset + 12 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        if kind == b"IDAT":
            compressed.extend(data[offset + 8:offset + 8 + length])
        offset += length + 12
    stride = width * channels
    raw = zlib.decompress(compressed)
    assert len(raw) == height * (stride + 1)
    prior = bytearray(stride)
    pixels = []
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = prior[i]
            upper_left = prior[i - channels] if i >= channels else 0
            if kind == 4:
                prediction = left + up - upper_left
                predictor = min((left, up, upper_left), key=lambda value: abs(prediction - value))
            else:
                predictor = (0, left, up, (left + up) // 2)[kind]
            row[i] = (row[i] + predictor) & 255
        for i in range(0, stride, channels):
            color = tuple(row[i:i + channels])
            pixels.append(color if channels == 4 else (*color, 255))
        prior = row
    return pixels


async def workflow_lifecycle(config, mode, wire):
    async with stdio_client(config) as (read, write):
        outgoing = TranscriptStream(write, "client", wire)
        async with StrictClientSession(TranscriptStream(read, "server", wire), outgoing) as client:
            await client.initialize()

            async def call(tool, args=None):
                response = await client.call_tool(tool, args)
                assert not response.isError, response.structuredContent
                return response.structuredContent

            info = await call("nova.project.info")
            project = {key: info[key] for key in ("projectId", "generation")}
            listing = await call("nova.assets.list", {"generation": project["generation"]})
            stages = []
            for name in ("a.png", "b.png"):
                asset = next(row["assetId"] for row in listing["assets"] if row["path"].endswith("/" + name))
                doc = (await call("nova.edit.capabilities", {
                    "generation": project["generation"], "assetId": asset,
                }))["document"]
                stages.append({"assetId": asset, "expectedRevision": doc["revision"],
                               "operations": [{"command": "fill_rect", "layer": doc["activeLayer"],
                                               "x": 0, "y": 0, "width": 1, "height": 1,
                                               "color": "#ba34ef", "opacity": 255}]})
            if mode == "workflow-reconnect":
                caps = await call("nova.edit.capabilities", {"generation": project["generation"]})
                assert caps["permission"] == "read_only"
                return
            if mode == "workflow-cancel-after":
                mutation = asyncio.create_task(call("nova.workflow.execute", {**project, "stages": stages}))
                assert (await asyncio.to_thread(sys.stdin.readline)).strip() == "continue"
                await outgoing.cancel_edit()
                await client.send_ping()
                print("cancel delivered", flush=True)
                result = await mutation
                assert result["status"] == "complete" and len(result["stages"]) == 2, result
                assert all(stage["status"] == "committed" for stage in result["stageOutcomes"])
                return
            if mode in ("workflow-cancel-preparation", "workflow-cancel-preflight"):
                mutation = asyncio.create_task(client.call_tool("nova.workflow.execute", {**project, "stages": stages}))
                assert (await asyncio.to_thread(sys.stdin.readline)).strip() == "continue"
                await outgoing.cancel_edit()
                await client.send_ping()
                print("cancel delivered", flush=True)
                result = await mutation
                assert tool_error(result)["code"] == "operation_canceled"
                return
            if mode == "workflow-switch-preparation":
                result = await client.call_tool("nova.workflow.execute", {**project, "stages": stages})
                assert tool_error(result)["code"] == "stale_project"
                return
            if mode.startswith("workflow-disconnect-"):
                mutation = asyncio.create_task(call("nova.workflow.execute", {**project, "stages": stages}))
                assert (await asyncio.to_thread(sys.stdin.readline)).strip() == "continue"
                assert not mutation.done(), "The workflow replied before disconnect"
                mutation.cancel()
                try:
                    await mutation
                except asyncio.CancelledError:
                    pass
                return
            try:
                result = await call("nova.workflow.execute", {**project, "stages": stages})
            except McpError as error:
                assert mode.startswith("workflow-revoke") and error.error.code == types.CONNECTION_CLOSED
            else:
                assert mode in ("workflow-close", "workflow-switch"), result
                assert result["status"] == "stopped" and len(result["stages"]) == 1, result
                assert result["failure"]["code"] == {
                    "workflow-close": "revision_conflict", "workflow-switch": "stale_project",
                }[mode], result
    print(f"{mode} passed")


async def lifecycle_profile(client: ClientSession) -> None:
    async def call(tool, args=None):
        response = await client.call_tool(tool, args)
        assert not response.isError, response.structuredContent
        return response.structuredContent

    warmup, samples = 2, 7
    report = {}

    async def timed(label, index, operation):
        started = time.perf_counter_ns()
        result = await operation
        if index >= warmup:
            report.setdefault(label, []).append((time.perf_counter_ns() - started) / 1e6)
        return result

    info = await call("nova.project.info")
    project = {key: info[key] for key in ("generation", "projectId")}
    for index in range(warmup + samples):
        await timed("ping", index, client.send_ping())
        created = {}
        for kind in ("texture", "animation", "model"):
            current = await call("nova.project.info")
            if kind == "model":
                parameters = {"command": kind, "cubeTexture": "nova:block/seed"}
            else:
                parameters = {"command": kind, "width": 16, "height": 16, "layerName": "Base",
                              "fill": "#123456", "pixelFormat": "rgba8888"}
                if kind == "animation":
                    parameters.update(frames=3, ticks=4, interpolate=False)
            created[kind] = await timed(f"create_{kind}", index, call("nova.asset.create", {
                **project, "indexRevision": current["indexRevision"], "namespace": "nova",
                "path": f"block/{kind}_{index}", "conflictPolicy": "reject", "parameters": parameters,
            }))

        def target(kind):
            item = created[kind]
            return {"assetId": item["assetId"], "expectedRevision": item["document"]["revision"]}

        async def mutate(kind, label, tool, operation):
            created[kind]["document"] = await timed(label, index, call(tool, {
                **project, **target(kind), "operation": operation,
            }))

        previous = {frame["id"] for frame in created["animation"]["document"]["frames"]}
        await mutate("animation", "frame_add", "nova.animation.edit",
                     {"command": "frame_add", "frame": created["animation"]["document"]["frames"][-1]["id"]})
        added, = {frame["id"] for frame in created["animation"]["document"]["frames"]} - previous
        await mutate("animation", "frame_duration", "nova.animation.edit",
                     {"command": "animation_duration", "entry": 1, "ticks": 9})
        await mutate("animation", "frame_reorder", "nova.animation.edit",
                     {"command": "frame_move", "frame": added, "value": 0})
        await mutate("model", "model_edit", "nova.model.edit", {
            "command": "model_transform", "element": 0, "from": [0, 0, 0], "to": [12, 16, 16],
            "rotation": {"enabled": False},
        })
        await mutate("model", "uv_edit", "nova.uv.edit",
                     {"command": "uv_set", "element": 0, "face": "down", "uv": [2, 3, 9, 11], "rotation": 90})
        for label, kind, options in (
            ("texture_png", "texture", {}), ("animation_png", "animation", {}),
            ("model_png", "model", {"view": "model"}), ("uv_png", "model", {"view": "uv"}),
        ):
            await timed(label, index, call("nova.asset.preview_image", {**project, **target(kind), **options}))
        stages = []
        for kind in ("texture", "animation"):
            operation = {"command": "layer_rename", "name": "Workflow stage",
                         "layer": created[kind]["document"]["activeLayer"]}
            stages.append({**target(kind), "operations": [operation]})
        result = await timed("workflow", index, call("nova.workflow.execute", {**project, "stages": stages}))
        assert result["status"] == "complete" and len(result["stages"]) == 2
    print("lifecycle_profile=" + json.dumps({"warmup": warmup, "samples": samples, "roundtrip_ms": report}))


async def animation_profile(client):
    async def call(tool, args=None):
        response = await client.call_tool(tool, args)
        assert not response.isError, response.structuredContent
        return response.structuredContent

    warmup, samples = 3, 16
    for dimension, frames in ((16, 3), (64, 12)):
        report = {}

        async def timed(label, iteration, tool, args):
            started = time.perf_counter()
            result = await call(tool, args)
            elapsed = (time.perf_counter() - started) * 1000
            if iteration >= warmup:
                report.setdefault(label, []).append(elapsed)
            return result

        info = await call("nova.project.info")
        project = {key: info[key] for key in ("projectId", "generation")}
        created = await timed("create_once", warmup, "nova.asset.create", {
            **project, "indexRevision": info["indexRevision"], "namespace": "nova",
            "path": f"block/profile_{dimension}", "conflictPolicy": "reject",
            "parameters": {"command": "animation", "width": dimension, "height": dimension,
                           "pixelFormat": "rgba8888", "layerName": "Profile", "fill": "#123456",
                           "frames": frames, "ticks": 4, "interpolate": False},
        })
        identity = {**project, "assetId": created["assetId"]}
        doc = created["document"]
        for iteration in range(warmup + samples):
            async def mutate(label, tool, operation=None):
                nonlocal doc
                args = {**identity, "expectedRevision": doc["revision"]}
                if operation is not None:
                    args["operations" if tool.endswith(".batch") else "operation"] = operation
                doc = await timed(label, iteration, tool, args)

            previous = {frame["id"] for frame in doc["frames"]}
            await mutate("add", "nova.animation.edit", {"command": "frame_add", "frame": doc["frames"][-1]["id"]})
            added, = {frame["id"] for frame in doc["frames"]} - previous
            await mutate("reorder", "nova.animation.edit", {"command": "frame_move", "frame": added, "value": 0})
            await mutate("remove", "nova.animation.edit", {"command": "frame_remove", "frame": added})
            await mutate("duration", "nova.animation.edit", {"command": "animation_duration", "entry": 0,
                                                             "ticks": 7 + iteration % 2})
            await mutate("batch", "nova.edit.batch", [
                {"command": "animation_duration", "entry": 0, "ticks": 4},
                {"command": "animation_duration", "entry": 1, "ticks": 9 - iteration % 2},
            ])
            await mutate("undo", "nova.edit.undo")
            await mutate("redo", "nova.edit.redo")
            await timed("preview", iteration, "nova.asset.preview_image",
                        {**identity, "expectedRevision": doc["revision"]})
        summary = {label: {"n": len(values), "median": round(statistics.median(values), 3),
                           "p95": round(sorted(values)[math.ceil(.95 * len(values)) - 1], 3)}
                   for label, values in report.items()}
        print(json.dumps({"dimension": dimension, "frames": frames, "warmup": warmup, "milliseconds": summary}))


async def phase1(client: ClientSession, initialized) -> dict:
    """MCP Phase 1 through the official SDK. The listing omits the spec-optional outputSchema (a
    size decision), so the SDK passes each structured result through instead of validating it."""
    instructions = initialized.instructions or ""
    for phrase in ("never filesystem paths", "not proof", "nova.drafts.save", "nova.guide.get"):
        assert phrase in instructions, (phrase, instructions)
    assert initialized.capabilities.resources is not None
    uris = {str(resource.uri) for resource in (await client.list_resources()).resources}
    assert {"nova://rules/index", "nova://rules/1.21.11/items", "nova://project/style-notes"} <= uris, uris
    templates = (await client.list_resource_templates()).resourceTemplates
    assert templates[0].uriTemplate == "nova://rules/{version}/{topic}"
    catalog = await client.call_tool("nova.guide.list", {})
    topics = [entry["topic"] for entry in catalog.structuredContent["topics"]]
    assert topics == ["pack", "items", "textures", "armor", "trims", "animations", "pixel-art", "pvp"], topics
    for topic in topics:
        read = await client.read_resource(f"nova://rules/1.21.11/{topic}")
        tool = await client.call_tool("nova.guide.get", {"topic": topic, "version": "1.21.11"})
        assert not tool.isError and tool.structuredContent["text"] == read.contents[0].text, topic
    assert tool_error(await client.call_tool("nova.guide.get", {"topic": "items", "version": "26.1"}))["code"] == "guide_unavailable"
    try:
        await client.read_resource("nova://rules/26.1/items")
        raise AssertionError("an unsupported guide version was served")
    except McpError as error:
        assert error.error.code == -32002, error
    style = json.loads((await client.read_resource("nova://project/style-notes")).contents[0].text)
    assert style["present"] is True and style["theme"] == "Cozy medieval" and style["kind"] == "user_content", style
    info = await client.call_tool("nova.project.info")
    for _ in range(50):
        if not info.isError:
            break
        await asyncio.sleep(0.1)
        info = await client.call_tool("nova.project.info")
    assert info.structuredContent["styleNotes"]["present"] is True
    assert info.structuredContent["unsavedCount"] >= 1
    scope = {"generation": info.structuredContent["generation"], "projectId": info.structuredContent["projectId"]}
    drafts = await client.call_tool("nova.drafts.list", scope)
    draft = next(row for row in drafts.structuredContent["drafts"] if row["dirty"])
    adopted = await client.call_tool("nova.drafts.adopt", {**scope, "documentId": draft["documentId"],
                                                           "path": draft["path"], "draftRevision": draft["draftRevision"]})
    assert not adopted.isError and adopted.structuredContent["adopted"] is True
    validation = await client.call_tool("nova.pack.validate", {"generation": scope["generation"]})
    assert validation.structuredContent["drafts"]["complete"] is True
    assert any(entry["path"] == draft["path"] for entry in validation.structuredContent["drafts"]["included"])
    saved = await client.call_tool("nova.drafts.save", {**scope, "documentId": draft["documentId"],
                                                        "expectedRevision": adopted.structuredContent["revision"]})
    assert not saved.isError and saved.structuredContent["status"] == "saved", saved
    assert saved.structuredContent["saved"][0]["path"] == draft["path"]
    assert "pack" in saved.structuredContent["saved"][0]["written"]
    again = await client.call_tool("nova.drafts.save", {**scope, "documentId": draft["documentId"],
                                                        "expectedRevision": adopted.structuredContent["revision"]})
    assert tool_error(again)["code"] == "revision_conflict"
    return {"saved": draft["path"], "unsavedCount": saved.structuredContent["unsavedCount"], "topics": len(topics)}


def wire_bytes(message):
    return len(json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")) + 1


def cycle_bytes(frames, native_preview=False):
    """Count compact UTF-8 JSON-RPC lines reconstructed from SDK request and response frames."""
    calls = {frame["message"]["id"] for frame in frames
             if frame["direction"] == "client" and frame["message"].get("method") == "tools/call"}
    sizes = {"calls": len(calls), "requestBytes": 0, "responseBytes": 0, "responseTextBytes": 0}
    for frame in frames:
        message = frame["message"]
        if message.get("id") not in calls:
            continue
        if frame["direction"] == "client":
            if native_preview and message["params"]["name"] == "nova.asset.preview_image":
                # maxEdge=16 reproduces the original native-size PNG. Old requests had no size field.
                params = message["params"]
                arguments = {k: v for k, v in params["arguments"].items() if k != "maxEdge"}
                message = {**message, "params": {**params, "arguments": arguments}}
            kind = "requestBytes"
        else:
            kind = "responseBytes"
            sizes["responseTextBytes"] += sum(len(block["text"].encode("utf-8"))
                                              for block in message["result"]["content"] if block["type"] == "text")
        sizes[kind] += wire_bytes(message)
    return sizes


async def texture_cycle(client: ClientSession, wire, target, revision):
    """Compare one edit of identical 16x16 pixels through the legacy and indexed workflows."""
    write = {**target, "x": 15, "y": 15, "width": 1, "height": 1,
             "palette": ["#325681"], "rows": ["0"]}

    async def call(tool, args):
        result = await client.call_tool(tool, args)
        assert not result.isError, result
        return result

    start = len(wire)
    legacy_target = {**target, "maxEdge": 16}
    pixels = await call("nova.asset.preview_image", {**legacy_target, "expectedRevision": revision})
    assert pixels.structuredContent["width"] == pixels.structuredContent["height"] == 16
    edit_start = len(wire)
    edit = await call("nova.texture.put_pixels", {**write, "expectedRevision": revision, "include": "summary"})
    typical = cycle_bytes(wire[edit_start:])
    assert edit.structuredContent["changed"]
    assert "layers" not in edit.structuredContent and "frames" not in edit.structuredContent
    text = json.loads(edit.content[0].text)
    assert text["revision"] == edit.structuredContent["revision"] and text["assetId"] == target["assetId"], text
    assert "transactionId" not in text and "summary" in text, text
    await call("nova.asset.preview_image", {**legacy_target, "expectedRevision": edit.structuredContent["revision"]})
    before = cycle_bytes(wire[start:], native_preview=True)
    full = await call("nova.texture.read", {**target, "verbosity": "full"})
    assert json.loads(full.content[0].text) == full.structuredContent, full
    reset = await call("nova.edit.undo", {**target, "expectedRevision": edit.structuredContent["revision"]})
    revision = reset.structuredContent["revision"]

    start = len(wire)
    pixels = await call("nova.texture.read", target)
    assert pixels.structuredContent["encoding"] == "indexed"
    assert pixels.structuredContent["pixels"]["width"] == pixels.structuredContent["pixels"]["height"] == 16
    edit = await call("nova.texture.put_pixels", {**write, "expectedRevision": revision, "returnPreview": True})
    assert edit.structuredContent["changed"] and edit.structuredContent["preview"]["width"] == 256
    assert any(block.type == "image" for block in edit.content)
    after = cycle_bytes(wire[start:])
    reset = await call("nova.edit.undo", {**target, "expectedRevision": edit.structuredContent["revision"]})
    assert before["calls"] == 3 and after["calls"] == 2
    return {"before": before, "after": after, "typicalEdit": typical}, reset.structuredContent["revision"]


async def phase2(client: ClientSession, wire) -> dict:
    """MCP Phase 2 compact texture tools through the official SDK. jsonschema checks that the
    published input schemas accept and reject the same arguments as Nova, except the checks that
    need the canvas. The listing omits the spec-optional outputSchema (a size decision), so the
    SDK passes each structured result through instead of validating it."""
    import jsonschema
    tools = {tool.name: tool for tool in (await client.list_tools()).tools}
    listing = next(frame["message"] for frame in reversed(wire) if frame["direction"] == "server"
                   and "tools" in frame["message"].get("result", {}))
    names = ["nova.texture.put_pixels", "nova.texture.palette_remap", "nova.texture.gradient_map", "nova.texture.erase"]
    validators = {}
    for name in names:
        tool = tools[name]
        assert tool.outputSchema is None and tool.annotations.readOnlyHint is False, name
        jsonschema.Draft202012Validator.check_schema(tool.inputSchema)
        validators[name] = jsonschema.Draft202012Validator(tool.inputSchema)
    info = await client.call_tool("nova.project.info")
    for _ in range(50):
        if not info.isError:
            break
        await asyncio.sleep(0.1)
        info = await client.call_tool("nova.project.info")
    scope = {"generation": info.structuredContent["generation"], "projectId": info.structuredContent["projectId"]}
    drafts = await client.call_tool("nova.drafts.list", scope)
    row = next(entry for entry in drafts.structuredContent["drafts"]
               if entry["path"] == "assets/nova/textures/item/plain.png")
    capabilities = await client.call_tool("nova.edit.capabilities", {"generation": scope["generation"], "assetId": row["assetId"]})
    assert set(names) <= set(capabilities.structuredContent["tools"]), capabilities.structuredContent["tools"]
    revision = capabilities.structuredContent["document"]["revision"]
    target = {**scope, "assetId": row["assetId"]}
    edits = [
        ("nova.texture.put_pixels", {"x": 0, "y": 0, "width": 4, "height": 2, "palette": ["#ff0000", "#00ff0080"],
                                     "rows": ["0101", "1.10"]}),
        ("nova.texture.palette_remap", {"mappings": [{"from": "#ff0000", "to": "#0000ff"}], "tolerance": 0}),
        ("nova.texture.gradient_map", {"stops": [{"position": 0, "color": "#000000"}, {"position": 255, "color": "#ffffff"}],
                                       "region": {"x": 0, "y": 0, "width": 4, "height": 2}}),
        ("nova.texture.erase", {"region": {"x": 0, "y": 0, "width": 1, "height": 1}}),
    ]
    for name, args in edits:
        full = {**target, "expectedRevision": revision, **args}
        previous = revision
        if name == "nova.texture.put_pixels":
            full["returnPreview"] = True
        validators[name].validate(full)
        result = await client.call_tool(name, full)
        assert not result.isError, result
        content = result.structuredContent
        assert content["path"] == row["path"] and content["documentId"] == row["documentId"], content
        assert content["changed"] is True and content["dirty"] is True and content["pixelsChanged"] > 0, content
        revision = content["revision"]
        if name == "nova.texture.put_pixels":
            image = base64.b64decode(next(item.data for item in result.content if item.type == "image"))
            assert content["preview"] == {"width": 256, "height": 256}, content
            assert struct.unpack(">II", image[16:24]) == (256, 256)
            rgba = decode_png(image, 256, 256)
            assert rgba[8 * 256 + 8] == (255, 0, 0, 255), rgba[8 * 256 + 8]
            png = await client.call_tool("nova.asset.preview_image", {
                **target, "expectedRevision": revision, "pixelGrid": True})
            assert not png.isError and png.structuredContent["width"] == 256, png
            grid = base64.b64decode(next(item.data for item in png.content if item.type == "image"))
            gridded = decode_png(grid, 256, 256)
            assert gridded[8 * 256 + 8] == rgba[8 * 256 + 8]
            assert gridded[8 * 256 + 16] != rgba[8 * 256 + 16]
            comparison = await client.call_tool("nova.asset.preview_image", {
                **target, "expectedRevision": revision, "compare": True})
            assert not comparison.isError and comparison.structuredContent["beforeRevision"] == previous, comparison
            assert comparison.structuredContent["width"] > 512, comparison.structuredContent
            assert any(item.type == "image" for item in comparison.content)
            assets = await client.call_tool("nova.assets.list", {"generation": scope["generation"]})
            other_id = next(entry["assetId"] for entry in assets.structuredContent["assets"]
                            if entry["path"] == "assets/nova/textures/item/second.png")
            sheet = await client.call_tool("nova.asset.preview_image", {
                **scope, "assetIds": [target["assetId"], other_id]})
            assert not sheet.isError and sheet.structuredContent["columns"] == 2, sheet
            assert [tile["assetId"] for tile in sheet.structuredContent["assets"]] == [target["assetId"], other_id]
            assert all(tile["label"] for tile in sheet.structuredContent["assets"])
            assert any(item.type == "image" for item in sheet.content)
            read = await client.call_tool("nova.texture.read", {**target, "layer": content["layer"],
                                          "region": {"x": 0, "y": 0, "width": 4, "height": 1}})
            assert not read.isError, read
            decoded = read.structuredContent
            assert decoded["encoding"] == "indexed" and decoded["source"] == "live", decoded
            assert decoded["revision"] == revision and decoded["colorCount"] == 2, decoded
            pixels = decoded["pixels"]
            colors = [pixels["palette"][int(symbol)] for symbol in pixels["rows"][0]]
            assert colors == ["#ff0000", "#00ff0080"] * 2, colors
            assert [entry["color"] for entry in decoded["paletteSummary"]] == pixels["palette"]
            same = await client.call_tool(name, {**target, **pixels, "expectedRevision": revision,
                                                 "layer": content["layer"]})
            assert not same.isError and not same.structuredContent["changed"], same
            revision = same.structuredContent["revision"]
    # Schema-level errors: Nova and jsonschema both reject them, and Nova names the field.
    base = {**target, "expectedRevision": revision, "x": 0, "y": 0, "width": 1, "height": 1, "palette": ["#000000"], "rows": ["0"]}
    invalid = [({**base, "palette": ["red"]}, "/palette/0"), ({**base, "rows": ["0 "]}, "/rows/0"),
               ({**base, "width": 0}, "/width"), ({**base, "mode": "add"}, "/mode"), ({**base, "extra": 1}, "/extra")]
    for args, pointer in invalid:
        assert not validators["nova.texture.put_pixels"].is_valid(args), args
        error = tool_error(await client.call_tool("nova.texture.put_pixels", args))
        assert error["code"] == "invalid_arguments" and error["details"]["pointer"] == pointer, (args, error)
    # Canvas checks: the schema cannot know the canvas, so only Nova rejects these.
    canvas = [({**base, "x": 15, "width": 4, "rows": ["0000"]}, "/width"), ({**base, "rows": ["1"]}, "/rows/0"),
              ({**base, "height": 2}, "/rows")]
    for args, pointer in canvas:
        assert validators["nova.texture.put_pixels"].is_valid(args), args
        error = tool_error(await client.call_tool("nova.texture.put_pixels", args))
        assert error["code"] == "invalid_arguments" and error["details"]["pointer"] == pointer, (args, error)
    undone = await client.call_tool("nova.edit.undo", {**target, "expectedRevision": revision})
    assert not undone.isError, undone
    redone = await client.call_tool("nova.edit.redo", {**target, "expectedRevision": undone.structuredContent["revision"]})
    assert not redone.isError and redone.structuredContent["revision"] != undone.structuredContent["revision"], redone
    cycle, revision = await texture_cycle(client, wire, target, redone.structuredContent["revision"])
    native = tools["nova.texture.edit"]
    native_validator = jsonschema.Draft202012Validator(native.inputSchema)
    region = {"x": 0, "y": 0, "width": 2, "height": 2}
    before_native = await client.call_tool("nova.texture.read", target)
    native_request = {**target, "expectedRevision": revision, "operation": {
        "command": "gen_gui_panel", "region": region,
        "params": {"base": "#12345680", "style": "Flat", "outline": False, "corner_radius": 0},
    }}
    native_validator.validate(native_request)
    # The listing names nova.texture.edit's operations by command only, pointing at
    # nova.edit.batch for the fields, and still rejects a command Nova rejects for this tool.
    assert not native_validator.is_valid({**target, "expectedRevision": revision,
                                          "operation": {"command": "layer_add"}})
    generated = await client.call_tool(native.name, native_request)
    assert not generated.isError and generated.structuredContent["changed"], generated
    after_native = await client.call_tool("nova.texture.read", {**target, "region": region})
    assert "#12345680" in after_native.structuredContent["pixels"]["palette"], after_native
    assert after_native.structuredContent["revision"] == generated.structuredContent["revision"]
    restored_native = await client.call_tool("nova.edit.undo", {
        **target, "expectedRevision": generated.structuredContent["revision"]})
    assert not restored_native.isError, restored_native
    after_undo = await client.call_tool("nova.texture.read", target)
    assert after_undo.structuredContent["pixels"] == before_native.structuredContent["pixels"]
    revision = restored_native.structuredContent["revision"]
    bad = await client.call_tool(native.name, {**target, "expectedRevision": revision, "operation": {
        "command": "resize_image", "params": {"width": 257, "height": 16}}})
    assert tool_error(bad)["code"] == "invalid_arguments", bad
    resized = await client.call_tool("nova.edit.batch", {**target, "expectedRevision": revision, "operations": [
        {"command": "resize_image", "params": {"width": 24, "height": 16}},
        {"command": "fill_rect", "layer": capabilities.structuredContent["document"]["activeLayer"],
         "x": 23, "y": 0, "width": 1, "height": 1, "color": "#aa1122", "opacity": 255},
        {"command": "put_pixels", "x": 22, "y": 0, "width": 1, "height": 1, "palette": ["#335577"], "rows": ["0"]},
    ]})
    assert not resized.isError and resized.structuredContent["width"] == 24, resized
    expanded = await client.call_tool("nova.texture.read", {**target, "region": {
        "x": 22, "y": 0, "width": 2, "height": 1}})
    assert set(expanded.structuredContent["pixels"]["palette"]) == {"#aa1122", "#335577"}, expanded
    undo_resize = await client.call_tool("nova.edit.undo", {
        **target, "expectedRevision": resized.structuredContent["revision"]})
    assert not undo_resize.isError and undo_resize.structuredContent["width"] == 16, undo_resize
    revision = undo_resize.structuredContent["revision"]
    saved = await client.call_tool("nova.drafts.save", {**scope, "documentId": row["documentId"],
                                                        "expectedRevision": revision})
    assert not saved.isError and saved.structuredContent["status"] == "saved", saved
    other = await client.call_tool("nova.asset.open", {**scope, "assetId": other_id})
    assert not other.isError and other.structuredContent["opened"], other
    opened = other.structuredContent
    assert len(opened["document"]["layers"]) == 2 and opened["document"]["layers"][-1]["name"] == "Overlay", opened
    second = {**scope, "assetId": opened["assetId"]}
    again = await client.call_tool("nova.asset.open", second)
    assert not again.isError and not again.structuredContent["opened"], again
    assert again.structuredContent["revision"] == opened["revision"], again
    wrote = await client.call_tool("nova.texture.put_pixels", {**base, **second, "expectedRevision": opened["revision"]})
    assert not wrote.isError and wrote.structuredContent["undoAvailable"], wrote
    restored = await client.call_tool("nova.edit.undo", {**second, "expectedRevision": wrote.structuredContent["revision"]})
    assert not restored.isError and restored.structuredContent["redoAvailable"], restored
    bulk = await bulk_recolor(client, wire, scope, assets.structuredContent["assets"], tools)
    return {"saved": row["path"], "opened": opened["path"], "tools": len(tools),
            "edits": len(edits), "argumentErrors": len(invalid) + len(canvas), "textureCycle": cycle,
            "bulkRecolor": bulk, "toolsListBytes": wire_bytes(listing)}


async def bulk_recolor(client, wire, scope, assets, tools):
    import jsonschema

    ids = {asset["path"]: asset["assetId"] for asset in assets}
    groups = [[ids[f"assets/nova/textures/item/zbench_{group}/ore_{i}.png"] for i in range(5)]
              for group in ("a", "b")]
    mapping = {"mappings": [{"from": "#808080", "to": "#325681"}]}

    async def call(tool, args):
        # validation_unavailable means nothing ran (the index refreshes after the save above).
        for attempt in range(100):
            response = await client.call_tool(tool, {**scope, **args})
            if not (response.isError and tool_error(response)["code"] == "validation_unavailable"):
                break
            await asyncio.sleep(0.05)
        assert not response.isError, response
        return response.structuredContent

    start = len(wire)
    for asset in groups[0]:
        opened = await call("nova.asset.open", {"assetId": asset})
        assert opened["opened"], opened
        edited = await call("nova.texture.palette_remap", {
            "assetId": asset, "expectedRevision": opened["revision"], **mapping})
        assert edited["pixelsChanged"] == 256, edited
    before = cycle_bytes(wire[start:])

    start = len(wire)
    tool = tools["nova.texture.adjust_bulk"]
    targets = [{"assetId": asset, "expectedRevision": "current"} for asset in groups[1]]
    operation = {"command": "palette_remap", **mapping}
    requests = {"targets": targets, "operations": [operation]}
    jsonschema.Draft202012Validator(tool.inputSchema).validate({**scope, **requests})
    result = await call(tool.name, requests)
    after = cycle_bytes(wire[start:])
    assert result["committed"] == 5 and result["rejected"] == 0, result
    assert [row["assetId"] for row in result["results"]] == groups[1]
    for row in result["results"]:
        assert row["opened"] and row["changed"] and row["undoAvailable"], row
        read = await call("nova.texture.read", {"assetId": row["assetId"]})
        assert read["revision"] == row["revision"] and read["pixels"]["palette"] == ["#325681"], read

    first, second, third = result["results"][:3]
    mixed = await call(tool.name, {"targets": [
        {"assetId": first["assetId"], "expectedRevision": "f" * 64},
        {"assetId": second["assetId"], "expectedRevision": second["revision"]},
    ], "operations": [{"command": "colorize", "params": {"color": "#00ff00"}}]})
    assert mixed["committed"] == 1 and mixed["rejected"] == 1, mixed
    assert mixed["results"][0]["error"]["code"] == "revision_conflict", mixed
    assert mixed["results"][1]["changed"] and not mixed["results"][1]["opened"], mixed

    stages = [{"assetId": row["assetId"], "expectedRevision": row["revision"], "operations": [
        {"command": "put_pixels", "x": 0, "y": 0, "width": 1, "height": 1,
         "rows": ["0"], "palette": ["#ff0000"]},
        {"command": "erase", "region": {"x": 1, "y": 0, "width": 1, "height": 1}},
    ]} for row in (first, third)]
    bad = await client.call_tool("nova.edit.batch", {**scope, **stages[0], "operations": [
        stages[0]["operations"][0], {"command": "erase", "region": {"x": 16, "y": 0, "width": 1, "height": 1}},
    ]})
    assert tool_error(bad)["code"] == "edit_rejected", bad
    planned = await call("nova.workflow.plan", {"stages": stages})
    assert planned["status"] == "ready", planned
    workflow = await call("nova.workflow.execute", {"stages": stages})
    assert workflow["status"] == "complete" and len(workflow["stages"]) == 2, workflow
    for stage, edited in zip(stages, workflow["stages"]):
        assert edited["appliedOperations"] == 2, edited
        restored = await call("nova.edit.undo", {"assetId": stage["assetId"], "expectedRevision": edited["revision"]})
        assert restored["redoAvailable"], restored
    return {"before": before, "after": after}


async def phase3(client: ClientSession) -> dict:
    """MCP Phase 3 read-only tools through the official SDK. The listing omits the spec-optional
    outputSchema (a size decision), so the SDK passes structured results through; errors must
    still carry no structured content (StrictClientSession)."""
    import hashlib
    import jsonschema
    tools = {tool.name: tool for tool in (await client.list_tools()).tools}
    names = ["nova.vanilla.list", "nova.vanilla.texture", "nova.asset.relationships", "nova.pack.validate",
             "nova.texture.analyze"]
    for name in names:
        tool = tools[name]
        assert tool.outputSchema is None and tool.annotations.readOnlyHint is True, name
        jsonschema.Draft202012Validator.check_schema(tool.inputSchema)
    info = await client.call_tool("nova.project.info")
    for _ in range(50):
        if not info.isError:
            break
        await asyncio.sleep(0.1)
        info = await client.call_tool("nova.project.info")
    scope = {"generation": info.structuredContent["generation"], "projectId": info.structuredContent["projectId"]}
    version = info.structuredContent["minecraftVersion"]
    listing = await client.call_tool("nova.vanilla.list", {**scope, "version": version, "limit": 3})
    assert not listing.isError and listing.structuredContent["textures"], listing
    path = listing.structuredContent["textures"][0]["path"]
    texture = await client.call_tool("nova.vanilla.texture", {**scope, "version": version, "path": path})
    assert not texture.isError, texture
    images = [item for item in texture.content if item.type == "image"]
    assert len(images) == 1 and images[0].mimeType == "image/png", texture.content
    assert hashlib.sha256(base64.b64decode(images[0].data)).hexdigest() == texture.structuredContent["sha256"]
    water = await client.call_tool("nova.texture.read", {
        **scope, "assetId": "vanilla:block/water_still.png", "frame": 1})
    assert not water.isError, water
    assert water.structuredContent["source"] == "vanilla" and water.structuredContent["frame"] == 1, water
    assert water.structuredContent["encoding"] == "indexed", water
    many = await client.call_tool("nova.texture.read", {
        **scope, "assetId": "vanilla:assets/minecraft/textures/gui/big.png",
        "region": {"x": 0, "y": 0, "width": 8, "height": 8}})
    assert not many.isError, many
    decoded = many.structuredContent
    assert decoded["encoding"] == "hex" and decoded["colorCount"] == 64, decoded
    assert decoded["paletteSummaryTruncated"] and len(decoded["paletteSummary"]) == 62, decoded
    rows = decoded["pixels"]["rows"]
    assert len(rows) == 8 and all(len(row) == 64 for row in rows), rows
    assert len({row[i:i + 8] for row in rows for i in range(0, 64, 8)}) == 64, rows
    metadata = await client.call_tool("nova.vanilla.texture", {**scope, "version": version, "path": path, "include": "metadata"})
    assert not metadata.isError and not [item for item in metadata.content if item.type == "image"], metadata
    errors = {
        "texture_not_in_cache": ("nova.vanilla.texture", {**scope, "version": version,
                                                          "path": "assets/minecraft/textures/block/not_cached.png"}),
        "vanilla_not_cached": ("nova.vanilla.list", {**scope, "version": "26.3"}),
        "unsupported_version": ("nova.vanilla.list", {**scope, "version": "1.20.1"}),
        "version_unavailable": ("nova.asset.relationships", {**scope, "path": path, "version": "26.3"}),
        "invalid_arguments": ("nova.asset.relationships", {**scope, "path": "assets/nova/../x.json"}),
    }
    for expected, (name, args) in errors.items():
        error = tool_error(await client.call_tool(name, args))
        assert error["code"] == expected, (expected, error)
    target = os.environ.get("NOVA_PHASE3_TEXTURE", path)
    related = await client.call_tool("nova.asset.relationships", {**scope, "path": target})
    assert not related.isError and related.structuredContent["relationships"], related
    if "NOVA_PHASE3_TEXTURE" in os.environ:
        assets = await client.call_tool("nova.assets.list", {"generation": scope["generation"], "search": target})
        asset = next(row["assetId"] for row in assets.structuredContent["assets"] if row["path"] == target)
        analysis = await client.call_tool("nova.texture.analyze", {**scope, "assetId": asset})
        assert not analysis.isError, analysis
        sampled = analysis.structuredContent["assets"][0]
        assert sampled["source"] == "disk" and sampled["layout"]["animated"], sampled
        assert sampled["colorCount"] >= 1, sampled
        denied = await client.call_tool("nova.texture.adjust_bulk", {**scope,
            "targets": [{"assetId": asset, "expectedRevision": "current"}],
            "operations": [{"command": "hue_saturation", "params": {"hue": 25}}]})
        assert tool_error(denied)["code"] == "permission_denied", denied
        disk = await client.call_tool("nova.texture.read", {**scope, "assetId": asset, "frame": 1})
        assert not disk.isError and disk.structuredContent["source"] == "disk", disk
        assert disk.structuredContent["frame"] == 1 and disk.structuredContent["pixels"]["height"] == 16, disk
    validation = await client.call_tool("nova.pack.validate", {"generation": scope["generation"], "limit": 50})
    assert not validation.isError, validation
    content = validation.structuredContent
    assert content["minecraftVersion"] == version and "resourceStack" in content["sources"], content["sources"]
    for finding in content["diagnostics"]:
        assert finding["rule"] and "nextStep" in finding and isinstance(finding["evidence"], list), finding
    return {"tools": len(tools), "relationships": len(related.structuredContent["relationships"]),
            "findings": len(content["diagnostics"]), "errors": len(errors)}


async def main() -> None:
    env = dict(os.environ)
    config = StdioServerParameters(command=sys.argv[1], env=env)
    wire = []
    mode = sys.argv[2] if len(sys.argv) > 2 else "inspect"
    def save_wire(name):
        if target := env.get("NOVA_MCP_EVIDENCE"):
            (Path(target) / name).write_text(
                "".join(json.dumps(frame) + "\n" for frame in wire), encoding="utf-8"
            )

    if mode.startswith("workflow-"):
        try:
            await workflow_lifecycle(config, mode, wire)
        finally:
            save_wire(f"sdk-{mode}.jsonl")
        return
    if mode not in ("inspect", "edit", "phase4d", "phase1", "phase2", "phase3", "animation_profile", "restored-preview", "preview-lifecycle", "preview-reconnect", "lifecycle-profile", "phase4e-pack"):
        try:
            await lifecycle(config, mode, wire)
        finally:
            save_wire(f"sdk-{mode}.jsonl")
        return
    async with stdio_client(config) as (read, write):
        async with StrictClientSession(TranscriptStream(read, "server", wire),
                                       TranscriptStream(write, "client", wire)) as client:
            initialized = await client.initialize()
            assert initialized.protocolVersion == "2025-11-25"
            assert initialized.capabilities.tools is not None
            tools = await client.list_tools()
            assert len(tools.tools) == 44
            if mode == "phase3":
                print(json.dumps(await phase3(client)), flush=True)
                save_wire("sdk-phase3.jsonl")
                return
            if mode == "phase2":
                try:
                    print(json.dumps(await phase2(client, wire)), flush=True)
                finally:
                    save_wire("sdk-phase2.jsonl")
                return
            if mode == "phase1":
                print(json.dumps(await phase1(client, initialized)), flush=True)
                save_wire("sdk-phase1.jsonl")
                return
            if mode == "phase4e-pack":
                await phase4e_pack(client)
                save_wire("sdk-phase4e-pack.jsonl")
                return
            if mode == "edit":
                await edit(client)
                save_wire("sdk-edit.jsonl")
                return
            if mode == "animation_profile":
                await animation_profile(client)
                return
            if mode == "lifecycle-profile":
                await lifecycle_profile(client)
                return
            if mode in ("preview-lifecycle", "preview-reconnect"):
                await preview_lifecycle(client, reconnect=mode == "preview-reconnect")
                save_wire(f"sdk-{mode}.jsonl")
                return
            if mode == "restored-preview":
                await restored_preview(client)
                save_wire("sdk-restored-preview.jsonl")
                return
            if mode == "phase4d":
                await phase4d(client)
                save_wire("sdk-phase4d.jsonl")
                return
            info = await client.call_tool("nova.project.info")
            for _ in range(50):
                if not info.isError:
                    break
                await asyncio.sleep(0.1)
                info = await client.call_tool("nova.project.info")
            assert not info.isError and info.structuredContent["name"] == "other-pack"
            generation = info.structuredContent["generation"]
            page = await client.call_tool("nova.assets.list", {"generation": generation, "limit": 1})
            assert len(page.structuredContent["assets"]) == 1
            assert page.structuredContent["nextOffset"] == 1
            listing = await client.call_tool("nova.assets.list", {"generation": generation})
            assets = {row["type"]: row["assetId"] for row in listing.structuredContent["assets"]}
            results = []
            for tool, kind, field in (
                ("nova.asset.inspect", "resource", "format"),
                ("nova.texture.inspect", "texture", "width"),
                ("nova.animation.inspect", "animation", "frameCount"),
                ("nova.model.inspect", "model", "modelType"),
            ):
                item = await client.call_tool(tool, {"generation": generation, "assetId": assets[kind]})
                assert not item.isError
                assert field in item.structuredContent or field in item.structuredContent["metadata"]
                results.append(item)
            pixels = await client.call_tool("nova.texture.read", {"generation": generation, "assetId": assets["texture"]})
            assert not pixels.isError and pixels.structuredContent["source"] == "live", pixels
            blocked = await client.call_tool("nova.asset.open", {
                "generation": generation, "projectId": info.structuredContent["projectId"],
                "assetId": assets["texture"]})
            assert tool_error(blocked)["code"] == "permission_denied", blocked
            validation = await client.call_tool("nova.pack.validate", {"generation": generation, "limit": 10})
            assert not validation.isError and validation.structuredContent["files"] == 4
            rejected = await client.call_tool("nova.asset.inspect", {"generation": generation, "assetId": "../secret"})
            assert tool_error(rejected)["code"] == "invalid_arguments"
            records = [initialized, tools, info, page, *results, validation, rejected]
            transcript = "\n".join(json.dumps(r.model_dump(by_alias=True, exclude_none=True)) for r in records)
            print(transcript)
            save_wire("sdk-wire.jsonl")


async def lifecycle(config, mode, wire):
    async def editor_event(event):
        print(event, flush=True)
        assert (await asyncio.to_thread(sys.stdin.readline)).strip() == "continue"

    async with stdio_client(config) as (read, write):
        outgoing = TranscriptStream(write, "client", wire)
        async with StrictClientSession(TranscriptStream(read, "server", wire), outgoing) as client:
            try:
                await asyncio.wait_for(client.initialize(), 8)
            except McpError as error:
                # A used, revoked or otherwise dead code must never connect.
                assert mode == "refused" and error.error.code == types.CONNECTION_CLOSED
                print("dead grant refused", flush=True)
                return
            assert mode != "refused"

            async def call(tool, args=None, error=None):
                response = await asyncio.wait_for(client.call_tool(tool, args), 8)
                assert response.isError == (error is not None), response
                if error:
                    assert tool_error(response)["code"] == error, response
                return response.structuredContent

            info = await call("nova.project.info")
            scope = {key: info[key] for key in ("projectId", "generation")}
            listing = await call("nova.assets.list", {"generation": scope["generation"]})
            asset = next(row for row in listing["assets"] if row["path"].endswith("/block/a.png"))
            target = {**scope, "assetId": asset["assetId"]}
            caps_args = {key: target[key] for key in ("generation", "assetId")}
            caps = await call("nova.edit.capabilities", caps_args)
            assert caps["permission"] == ("read_only" if mode == "readonly" else "editing")
            doc = caps["document"]
            if mode == "disconnect":
                return
            args = {**target, "expectedRevision": doc["revision"], "operation": {
                "command": "fill_rect", "layer": doc["activeLayer"], "x": 0, "y": 0,
                "width": 1, "height": 1, "color": "#11bc95", "opacity": 255,
            }}
            if mode == "switch":
                await editor_event("switch")
                await call("nova.texture.edit", args, "stale_project")
            elif mode == "readonly":
                await call("nova.texture.edit", args, "permission_denied")
            elif mode == "cancel":
                outgoing.cancel_mutation = True
                await call("nova.texture.edit", args, "operation_canceled")
                assert (await call("nova.edit.capabilities", caps_args))["document"] == doc
                await call("nova.edit.preview", {
                    **target, "expectedRevision": doc["revision"], "operations": [args["operation"]],
                })
            elif mode == "revoke":
                result = await call("nova.texture.edit", args)
                assert result["changed"] and result["undoAvailable"]
                args["expectedRevision"] = result["revision"]
                args["operation"]["color"] = "#b5c622"
                await editor_event("revoke")
                try:
                    await call("nova.texture.edit", args)
                except McpError as error:
                    assert error.error.code == types.CONNECTION_CLOSED
                else:
                    raise AssertionError("revoked session accepted a request")
            elif mode == "cancel-after":
                mutation = asyncio.create_task(call("nova.texture.edit", args))
                await editor_event("cancel-after")
                await outgoing.cancel_edit()
                await asyncio.wait_for(client.send_ping(), 5)
                print("cancel delivered", flush=True)
                result = await mutation
                assert result["changed"] and result["undoAvailable"]
                assert result["revision"] != doc["revision"]
                assert (await call("nova.edit.capabilities", caps_args))["document"]["revision"] == result["revision"]
            elif mode in ("drop-before", "drop-after"):
                mutation = asyncio.create_task(call("nova.texture.edit", args))
                await editor_event("drop")
                assert not mutation.done(), "result arrived before disconnect"
                mutation.cancel()
                try:
                    await mutation
                except asyncio.CancelledError:
                    pass
            else:
                raise AssertionError(f"unknown lifecycle scenario {mode}")
    print(f"{mode} passed", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
