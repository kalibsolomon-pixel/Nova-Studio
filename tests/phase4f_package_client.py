# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-F packaged-build MCP client (official MCP SDK 1.26.0).

Runs against the packaged nova-mcp.exe with the connection code issued by the packaged
Nova Studio window. Verifies tool registration, then creates and edits a texture and a
model so the GUI can save them explicitly. Prints one JSON object per stage.

usage: python phase4f_package_client.py <nova-mcp.exe> <stage>
  stage = tools | create | edit-unsaved
"""
import asyncio
import json
import os
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def main() -> None:
    config = StdioServerParameters(command=sys.argv[1], env=dict(os.environ))
    stage = sys.argv[2]
    async with stdio_client(config) as (read, write):
        async with ClientSession(read, write) as client:
            initialized = await client.initialize()
            tools = await client.list_tools()
            names = sorted(tool.name for tool in tools.tools)

            async def call(tool, args=None):
                response = await client.call_tool(tool, args)
                assert not response.isError, response.structuredContent
                return response.structuredContent

            info = await call("nova.project.info")
            project = {key: info[key] for key in ("generation", "projectId")}
            out = {"stage": stage, "protocol": initialized.protocolVersion, "tools": len(names),
                   "tool_names": names, "permission": None}
            if stage == "tools":
                print(json.dumps(out), flush=True)
                return

            async def create(path, parameters):
                current = await call("nova.project.info")
                return await call("nova.asset.create", {**project, "indexRevision": current["indexRevision"],
                                  "namespace": "minecraft", "path": path, "conflictPolicy": "reject",
                                  "parameters": parameters})

            def fill(asset, x, width, color):
                return {"command": "fill_rect", "layer": asset["document"]["activeLayer"], "x": x, "y": 0,
                        "width": width, "height": 16, "color": color, "opacity": 255}

            if stage == "create":
                texture = await create("block/phase4f_texture", {"command": "texture", "width": 16, "height": 16,
                                       "fill": "#203040", "layerName": "Base", "pixelFormat": "rgba8888"})
                edited = await call("nova.edit.batch", {**project, "assetId": texture["assetId"],
                                    "expectedRevision": texture["document"]["revision"],
                                    "operations": [fill(texture, 0, 8, "#f528ab")]})
                model = await create("block/phase4f_model", {"command": "model",
                                     "cubeTexture": "minecraft:block/phase4f_texture"})
                model_edit = await call("nova.edit.batch", {**project, "assetId": model["assetId"],
                                        "expectedRevision": model["document"]["revision"],
                                        "operations": [{"command": "model_transform", "element": 0,
                                                        "from": [0, 0, 0], "to": [16, 8, 16],
                                                        "rotation": {"enabled": False}}]})
                out.update(texture=texture["assetId"], texture_changed=edited.get("changed"),
                           model=model["assetId"], model_changed=model_edit.get("changed"))
                print(json.dumps(out), flush=True)
                return
            if stage == "edit-unsaved":
                assets = (await call("nova.assets.list", {"generation": project["generation"]}))["assets"]
                texture = next(a for a in assets if a["path"].endswith("/phase4f_texture.png"))
                texture["document"] = (await call("nova.edit.capabilities", {
                    "generation": project["generation"], "assetId": texture["assetId"]}))["document"]
                edited = await call("nova.edit.batch", {**project, "assetId": texture["assetId"],
                                    "expectedRevision": texture["document"]["revision"],
                                    "operations": [fill(texture, 8, 8, "#52ef25")]})
                out.update(texture=texture["assetId"], texture_changed=edited.get("changed"))
                print(json.dumps(out), flush=True)
                return
            if stage == "linked":
                # A genuine directory symbolic link inside the pack points outside it.
                assets = (await call("nova.assets.list", {"generation": project["generation"]}))["assets"]
                linked = [a["path"] for a in assets if "/linked/" in a["path"]]
                report = await call("nova.pack.validate", {"generation": project["generation"]})
                codes = sorted({d.get("code") for d in report.get("diagnostics", [])})
                unsafe = [d for d in report.get("diagnostics", []) if d.get("code") == "unsafe_path"]
                out.update(listed_through_link=linked, validation_codes=codes,
                           unsafe_paths=[d.get("path") or d.get("asset") for d in unsafe][:10])
                print(json.dumps(out), flush=True)
                return
            raise SystemExit(f"unknown stage {stage}")


asyncio.run(main())
