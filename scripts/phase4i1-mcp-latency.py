# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-I.1: MCP request latency through a given nova-mcp.exe with the official MCP Python SDK.

Usage: <sdk-python> phase4i1-mcp-latency.py <nova-mcp.exe> <rounds>
The bridge uses the connection Nova offered (Allow read-only), exactly as an agent starts it.
Times, in milliseconds: bridge start + initialize, tools/list, nova.project.info and a 100-asset
nova.assets.list page, each repeated <rounds> times over one session. Prints JSON.
"""
import asyncio
import json
import os
import statistics
import sys
import time

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def summary(values):
    values = sorted(values)
    return dict(n=len(values), median=statistics.median(values), p90=values[int(0.9 * (len(values) - 1))],
                min=values[0], max=values[-1])


async def main(bridge, rounds):
    env = {k: v for k, v in os.environ.items() if k != 'NOVA_MCP_CODE'}
    started = time.perf_counter()
    params = StdioServerParameters(command=bridge, args=[], env=env)
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            connect_ms = (time.perf_counter() - started) * 1000
            timings = {'tools/list': [], 'nova.project.info': [], 'nova.assets.list': []}
            info = None
            for _ in range(rounds):
                t = time.perf_counter()
                tools = await session.list_tools()
                timings['tools/list'].append((time.perf_counter() - t) * 1000)
                t = time.perf_counter()
                info = await session.call_tool('nova.project.info', {})
                timings['nova.project.info'].append((time.perf_counter() - t) * 1000)
                generation = (info.structuredContent or {}).get('generation', '')
                t = time.perf_counter()
                page = await session.call_tool('nova.assets.list', {'generation': generation, 'limit': 100})
                timings['nova.assets.list'].append((time.perf_counter() - t) * 1000)
                if info.isError or page.isError:
                    raise SystemExit(f'tool error: {info.content} / {page.content}')
            print(json.dumps({'connect_and_initialize_ms': connect_ms, 'tools': len(tools.tools),
                              'asset_count': (info.structuredContent or {}).get('assetCount'),
                              'assets_in_page': len((page.structuredContent or {}).get('assets', [])),
                              'timings_ms': {k: summary(v) for k, v in timings.items()}}))


if __name__ == '__main__':
    asyncio.run(asyncio.wait_for(main(sys.argv[1], int(sys.argv[2])), 300))
