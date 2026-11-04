# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-G: list Nova's MCP tools through a given nova-mcp.exe with the official MCP Python SDK.

Usage: python phase4g_sdk_tools.py <path-to-nova-mcp.exe>
The bridge reads the connection Nova offered (no NOVA_MCP_CODE), exactly as an agent starts it.
Prints {"protocolVersion", "server", "tools": [...]} as JSON. Exit code 0 on success.
"""
import asyncio
import json
import os
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def main(bridge):
    env = {k: v for k, v in os.environ.items() if k != 'NOVA_MCP_CODE'}
    params = StdioServerParameters(command=bridge, args=[], env=env)
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            init = await session.initialize()
            tools = await session.list_tools()
            print(json.dumps({'protocolVersion': init.protocolVersion,
                              'server': init.serverInfo.model_dump(mode='json'),
                              'tools': sorted(tool.name for tool in tools.tools)}))


if __name__ == '__main__':
    asyncio.run(asyncio.wait_for(main(sys.argv[1]), 30))
