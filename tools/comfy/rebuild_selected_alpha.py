"""Resumeable three-pass rebuild of the exact versions selected in Belorta's timeline.

Prepares and validates replacements; never edits the live project or old media.
"""
from pathlib import Path
import copy
import hashlib
import json
import os
import shutil
import subprocess
import time
import traceback
import urllib.request
import uuid

WORK = Path(__file__).resolve().parents[2]
AUDIT = WORK / 'artifacts/alpha-comparison/belorta-alpha-migration-audit.json'
RUN = WORK / 'artifacts/belorta-alpha-rebuild'
COMFY = Path('F:/1_PROGRAMS/BERNINI/ComfyUI')
PROJECT = Path('G:/9_PROJECT ARCHIVE/GACHA IS SUFFERING NLE/EPISODE ASSETS/BELORTA/GACHA IS SUFFERING - BELORTA')
URL = 'http://127.0.0.1:8188'
FFMPEG = WORK / 'build/bin/Release/ffmpeg.exe'


def atomic_json(path, value):
    temp = path.with_suffix('.json.partial')
    temp.write_text(json.dumps(value, indent=2), encoding='utf-8')
    os.replace(temp, path)


def request(path, body=None):
    req = urllib.request.Request(URL + path, data=None if body is None else json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=90) as response:
        return json.load(response)


def prune(graph, output):
    result = {}
    def visit(key):
        if key in result:
            return
        node = graph[key]
        result[key] = node
        for value in node['inputs'].values():
            if isinstance(value, list) and len(value) == 2 and isinstance(value[0], str):
                visit(value[0])
    visit(output)
    return result


def stage_graph(original, stage, key, prefix):
    graph = copy.deepcopy(original)
    def find(kind):
        ids = [k for k, v in graph.items() if v['class_type'] == kind]
        assert len(ids) == 1, (kind, ids)
        return ids[0]
    def put(k, kind, inputs):
        graph[k] = {'class_type': kind, 'inputs': inputs}
    generator, cleanup, create, output = [find(k) for k in
        ['GAPSCAIL2LongVideo', 'ToonOut Selected BG JNK', 'CreateVideo', 'SaveVideo']]
    generation = copy.deepcopy(graph[generator]['inputs'])
    video = copy.deepcopy(graph[create]['inputs'])
    if stage == 'prepare':
        ref = {k: generation[k] for k in ('reference_image', 'reference_image_mask', 'clip_vision_output',
               'character_count', 'vae', 'width', 'height', 'replacement_mode')}
        put('rt_batch_reference', 'RoundtableReference', ref)
        prep = {k: v for k, v in generation.items() if k not in
                ('model', 'vae', 'reference_image', 'reference_image_mask', 'clip_vision_output', 'character_count')}
        prep.update(reference=['rt_batch_reference', 0], artifact_key=key, audio=video['audio'], fps=video['fps'])
        put(generator, 'RoundtablePrepare', prep)
        output = generator
    elif stage == 'generate':
        put(generator, 'RoundtableGenerate', {'artifact_key': key, 'model': generation['model'], 'vae': generation['vae']})
        output = generator
    else:
        put('rt_batch_media', 'RoundtableLoadFrames', {'artifact_key': key})
        clean = copy.deepcopy(graph[cleanup]['inputs'])
        clean.pop('scail_model', None)
        clean.update(image=['rt_batch_media', 0], background_type='transparent', prepared_model=['rt_batch_toon', 0])
        put('rt_batch_toon', 'RoundtableToonModel', {'model_name': clean['model_name']})
        put(cleanup, 'ToonOut Selected BG JNK', clean)
        put('rt_alpha_repair', 'RoundtableAlphaRepair', {'image': [cleanup, 0]})
        put(output, 'VHS_VideoCombine', {'images': ['rt_alpha_repair', 0], 'audio': ['rt_batch_media', 1],
            'frame_rate': ['rt_batch_media', 2], 'filename_prefix': prefix, 'format': 'video/ProRes',
            'profile': '4444', 'loop_count': 0, 'pingpong': False, 'save_output': True,
            'save_metadata': True, 'trim_to_audio': False})
    return prune(graph, output)


def run_prompt(graph, recipe, stage, row, state, manifest):
    name = f'{row["label"][:3]}-{stage}'
    record = manifest['progress'].setdefault(name, {})
    if record.get('complete'):
        return record['history']
    if 'prompt_id' not in record:
        body = {'prompt': graph, 'client_id': manifest['batch_id'], 'extra_data': {
            'extra_pnginfo': {'roundtable_source_prompt': recipe, 'roundtable_alpha_revision': 3}}}
        queued = request('/prompt', body)
        record.update(prompt_id=queued['prompt_id'], started=time.time())
        atomic_json(state, manifest)
    print(f'{name} running {record["prompt_id"]}', flush=True)
    while True:
        data = request('/history/' + record['prompt_id'])
        if record['prompt_id'] in data:
            history = data[record['prompt_id']]
            if history['status']['status_str'] != 'success':
                atomic_json(RUN / f'{name}-failure.json', history)
                raise RuntimeError(f'{name} failed; see failure history')
            record.update(complete=True, finished=time.time(), history=history)
            atomic_json(state, manifest)
            print(f'{name} complete in {record["finished"] - record["started"]:.1f}s', flush=True)
            return history
        if time.time() - record['started'] > 7200:
            raise TimeoutError(name)
        time.sleep(10)


def copy_verified(source, dest):
    def sha(path):
        h = hashlib.sha256()
        with path.open('rb') as f:
            for chunk in iter(lambda: f.read(8 * 1024 * 1024), b''):
                h.update(chunk)
        return h.hexdigest()
    if dest.exists():
        assert sha(source) == sha(dest), f'Cache mismatch: {dest}'
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary = dest.with_suffix(dest.suffix + '.partial')
    shutil.copy2(source, temporary)
    assert sha(source) == sha(temporary), f'Copy failed: {source}'
    os.replace(temporary, dest)


def validate(path, row):
    import av
    import numpy as np
    with av.open(str(path)) as container:
        stream = container.streams.video[0]
        assert stream.codec_context.name == 'prores'
        assert stream.codec_context.format.name.startswith('yuva')
        assert (stream.width, stream.height) == (row['video']['width'], row['video']['height'])
        assert str(stream.average_rate) == row['video']['fps']
        count, partial, clear, opaque = 0, 0, 0, 0
        for frame in container.decode(stream):
            a = frame.to_ndarray(format='rgba')[:, :, 3]
            partial += int(np.count_nonzero((a > 0) & (a < 255)))
            clear += int(np.count_nonzero(a == 0))
            opaque += int(np.count_nonzero(a == 255))
            count += 1
        assert count == row['video']['frames'], (row['label'], count, row['video']['frames'])
        assert min(partial, clear, opaque) > 0
    def audio_hash(file):
        h = hashlib.sha256()
        with av.open(str(file)) as c:
            stream = c.streams.audio[0]
            for packet in c.demux(stream):
                h.update(bytes(packet))
        return h.hexdigest()
    assert audio_hash(path) == audio_hash(row['file']), 'Original audio must be unchanged'
    return {'frames': count, 'partial_alpha_pixels': partial, 'clear_pixels': clear, 'opaque_pixels': opaque,
            'audio_packets_identical': True, 'bytes': path.stat().st_size}


def main():
    RUN.mkdir(exist_ok=True)
    state = RUN / 'state.json'
    rows = json.loads(AUDIT.read_text())
    if state.exists():
        manifest = json.loads(state.read_text())
    else:
        queue = request('/queue')
        assert not queue['queue_running'] and not queue['queue_pending'], 'Server is busy'
        manifest = {'batch_id': uuid.uuid4().hex, 'started': time.time(), 'progress': {}, 'replacements': [],
                    'cache_policy': 'full raw frames and audio, retained until project finalization'}
        atomic_json(state, manifest)
    assert len(rows) == 48
    assert shutil.disk_usage(COMFY).free > 60 * 1024**3
    assert shutil.disk_usage(PROJECT).free > 60 * 1024**3
    order = sorted(rows, key=lambda r: ('CU' in r['label'], r['label']))
    info = request('/object_info')
    assert 'RoundtableAlphaRepair' in info, 'Restart ComfyUI to load alpha repair'
    for stage in ('prepare', 'generate', 'finish'):
        manifest['stage'] = stage
        atomic_json(state, manifest)
        for row in order:
            number = int(row['label'][:3])
            key = manifest['batch_id'] + '_' + str(number)
            cache = PROJECT / 'ComfyUI Cache' / Path(row['file']).stem
            graph = stage_graph(row['recipe'], stage, key, 'roundtable/belorta_alpha_rebuild/' + Path(row['file']).stem)
            history = run_prompt(graph, row['recipe'], stage, row, state, manifest)
            if stage == 'generate':
                for name in ('frames.pt', 'media.pt'):
                    copy_verified(COMFY / 'output/roundtable_batch' / key / name, cache / name)
                atomic_json(cache / 'recipe.json', row)
            if stage == 'finish' and not any(x['old'] == row['file'] for x in manifest['replacements']):
                outputs = [f for node in history['outputs'].values() for files in node.values()
                           if isinstance(files, list) for f in files if isinstance(f, dict)
                           and f.get('filename', '').lower().endswith('.mov')]
                assert outputs, history['outputs']
                # VHS may report both the intermediate silent movie and the final audio mux.
                f = outputs[-1]
                source = COMFY / 'output' / f.get('subfolder', '') / f['filename']
                old = Path(row['file'])
                staged = old.parent / 'alpha-replacements-staging' / (old.stem + '.mov')
                staged.parent.mkdir(exist_ok=True)
                temporary = staged.with_suffix('.partial.mov')
                # Keep the user's existing audio bit for bit; preserve video frames and cadence.
                subprocess.run([str(FFMPEG), '-v', 'error', '-y', '-i', str(source), '-i', str(old),
                    '-map', '0:v:0', '-map', '1:a:0', '-c', 'copy', '-map_metadata', '0',
                    '-movflags', '+use_metadata_tags', str(temporary)], check=True)
                validation = validate(temporary, row)
                os.replace(temporary, staged)
                manifest['replacements'].append({'old': str(old).replace('\\', '/'),
                    'new': str(old.with_suffix('.mov')).replace('\\', '/'), 'staged': str(staged),
                    'seed': row['seed'], 'cache': str(cache), 'validation': validation})
                atomic_json(state, manifest)
                print(f'{number:03} validated replacement and retained raw cache', flush=True)
    manifest['stage'] = 'ready-to-install'
    manifest['finished'] = time.time()
    atomic_json(state, manifest)
    print('All 48 replacements validated. Live project and old media have not been changed.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception:
        RUN.mkdir(exist_ok=True)
        (RUN / 'error.txt').write_text(traceback.format_exc())
        raise
