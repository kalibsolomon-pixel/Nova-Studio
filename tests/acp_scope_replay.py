# SPDX-License-Identifier: GPL-3.0-or-later
"""Headless regression for fake_acp_agent's dependent MCP requests.

Run with the build's Qt/runtime folders on PATH:
    python tests/acp_scope_replay.py --agent <nova-fake-acp-agent.exe> --output <new-folder>
The scripted server speaks real stdio MCP; no Nova window, network or user data is used.
"""
import argparse
import json
import pathlib
import queue
import subprocess
import sys
import threading


def server(log, scenario):
    calls = 0
    for line in sys.stdin:
        request = json.loads(line)
        with log.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps({'request': request}) + '\n')
        if 'id' not in request:
            continue
        method, params = request['method'], request.get('params', {})
        error = None
        if method == 'initialize':
            result = {'protocolVersion': '2025-11-25', 'capabilities': {'tools': {}},
                      'serverInfo': {'name': 'scope-replay', 'version': '1'}}
        elif method == 'tools/list':
            result = {'tools': [{'name': 'nova.assets.list', 'inputSchema': {
                'type': 'object', 'properties': {'generation': {'type': 'string'}, 'projectId': {'type': 'string'}},
                'required': ['generation', 'projectId']}}]}
        elif params.get('name') == 'nova.project.info':
            calls += 1
            limit = {'ready': 0, 'transient': 1, 'last_attempt': 4, 'exhausted': 5}.get(scenario, 0)
            if calls <= limit or scenario == 'permanent':
                code = 'permission_denied' if scenario == 'permanent' else 'validation_unavailable'
                result = {'isError': True, 'content': [{'type': 'text', 'text': json.dumps({
                    'error': {'code': code, 'message': 'Scope unavailable in replay.'}})}]}
            elif scenario == 'transport':
                error = {'code': -32000, 'message': 'Replay transport failure.'}
                result = {}
            else:
                result = {'isError': False, 'structuredContent': {
                    'generation': 'a' * 64, 'projectId': 'b' * 64}, 'content': []}
                if scenario == 'incomplete':
                    del result['structuredContent']['generation']
        else:
            args = params.get('arguments', {})
            missing = next((key for key in ('generation', 'projectId') if key not in args), None)
            result = {'isError': False, 'structuredContent': {'assets': []}, 'content': []}
            if missing:
                result = {'isError': True, 'content': [{'type': 'text', 'text': json.dumps({
                    'error': {'code': 'invalid_arguments', 'message': 'Missing required argument /' + missing + '.'}})}]}
        response = {'jsonrpc': '2.0', 'id': request['id'], 'error' if error else 'result': error or result}
        with log.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps({'response': response}) + '\n')
        print(json.dumps(response), flush=True)


def replay(agent, folder, scenario):
    folder.mkdir()
    transcript, messages = [], queue.Queue()
    with (folder / 'stderr.txt').open('w', encoding='utf-8') as stderr:
        process = subprocess.Popen([str(agent)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=stderr, text=True, encoding='utf-8')

        def read():
            for line in process.stdout:
                messages.put(json.loads(line))
            messages.put(None)
        threading.Thread(target=read, daemon=True).start()

        def call(identity, method, params):
            request = {'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': params}
            transcript.append({'request': request})
            process.stdin.write(json.dumps(request) + '\n')
            process.stdin.flush()
            while True:
                response = messages.get(timeout=15)
                if response is None:
                    raise RuntimeError(f'Agent stdout closed before reply {identity}; exit={process.poll()}')
                transcript.append({'response': response})
                if response.get('id') == identity:
                    return response
        try:
            call(1, 'initialize', {'protocolVersion': 1})
            call(2, 'session/new', {'cwd': str(folder), 'mcpServers': [{
                'name': 'Nova', 'command': sys.executable,
                'args': [str(pathlib.Path(__file__).resolve()), '--server', str(folder / 'mcp.jsonl'), scenario], 'env': []}]})
            call(3, 'session/prompt', {'sessionId': 'fake-session-1',
                                     'prompt': [{'type': 'text', 'text': 'call nova.assets.list {}'}]})
            call(4, 'session/close', {'sessionId': 'fake-session-1'})
        finally:
            process.stdin.close()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            (folder / 'acp.json').write_text(json.dumps(transcript, indent=2), encoding='utf-8')
    requests = [row['request'] for line in (folder / 'mcp.jsonl').read_text().splitlines()
                if 'request' in (row := json.loads(line))]
    assets = [r for r in requests if r.get('params', {}).get('name') == 'nova.assets.list']
    info = [r for r in requests if r.get('params', {}).get('name') == 'nova.project.info']
    expected_calls = {'ready': 1, 'transient': 2, 'last_attempt': 5, 'exhausted': 5,
                      'permanent': 1, 'incomplete': 1, 'transport': 1}[scenario]
    success = scenario in ('ready', 'transient', 'last_attempt')
    assert process.returncode == 0, process.returncode
    assert len(info) == expected_calls, (scenario, len(info))
    assert len(assets) == int(success), (scenario, assets)
    if success:
        assert assets[0]['params']['arguments'] == {'generation': 'a' * 64, 'projectId': 'b' * 64}
    else:
        assert 'MCP SCOPE PROBLEM' in json.dumps(transcript)
    diagnostics = (folder / 'stderr.txt').read_text(encoding='utf-8')
    assert diagnostics.count('MCP SCOPE attempt ') == expected_calls - int(success), diagnostics
    return {'scenario': scenario, 'passed': True, 'info_calls': len(info), 'asset_calls': len(assets)}


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--server':
        server(pathlib.Path(sys.argv[2]), sys.argv[3])
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(exist_ok=False)
    results = []
    for scenario in ('ready', 'transient', 'last_attempt', 'exhausted', 'permanent', 'incomplete', 'transport'):
        results.append(replay(args.agent.resolve(), args.output / scenario, scenario))
    (args.output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(f'{len(results)}/{len(results)} ACP scope scenarios passed')


if __name__ == '__main__':
    main()
