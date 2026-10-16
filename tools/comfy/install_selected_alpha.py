"""Install the validated 48-file rebuild using an exactly verified native project patch."""
from pathlib import Path
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import psutil

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rebuild_selected_alpha import RUN, PROJECT, WORK, atomic_json, request


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def editor_closed():
    return not any(p.info['name'] and p.info['name'].lower() == 'roundtable.exe'
                   for p in psutil.process_iter(['name']))


def main():
    assert editor_closed(), 'Save and close Roundtable before replacing project media'
    state_path = RUN / 'state.json'
    state = json.loads(state_path.read_text())
    assert state['stage'] == 'ready-to-install'
    replacements = state['replacements']
    assert len(replacements) == 48
    originals = json.loads((RUN / 'original-hashes.json').read_text())
    roots = {Path(row['old']).parent.resolve() for row in replacements}
    assert len(roots) == 1
    renders = roots.pop()
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = renders / ('before-alpha-' + stamp)
    backup.mkdir()
    for row in replacements:
        old, new, staged = map(Path, (row['old'], row['new'], row['staged']))
        assert old.parent.resolve() == new.parent.resolve() == renders
        assert staged.parent.resolve() == renders / 'alpha-replacements-staging'
        assert old.stem == new.stem == staged.stem and new.suffix == '.mov'
        assert digest(old) == originals[row['old']], f'Original render changed: {old}'
        assert staged.stat().st_size == row['validation']['bytes']
        assert not new.exists(), f'Replacement already exists: {new}'
        assert (Path(row['cache']) / 'frames.pt').is_file()
        assert (Path(row['cache']) / 'media.pt').is_file()
    # Back up rather than discard selected green versions. Earlier takes are untouched.
    for row in replacements:
        old = Path(row['old'])
        shutil.copy2(old, backup / old.name)
        assert digest(backup / old.name) == originals[row['old']]
        os.replace(row['staged'], row['new'])
    mapping = RUN / 'replacement-paths.txt'
    mapping.write_text('\n'.join(json.dumps(x['old'], ensure_ascii=False) + ' ' +
                                json.dumps(x['new'], ensure_ascii=False) for x in replacements), encoding='utf-8')
    project = PROJECT / 'GACHA IS SUFFERING - BELORTA.rtp'
    before_hash = digest(project)
    candidate = RUN / ('project-alpha-' + stamp + '.rtp')
    tool = WORK / 'build/bin/Release/roundtable_project_tool.exe'
    result = subprocess.run([str(tool), str(project), str(candidate), str(mapping), '--replace-alpha'],
                             capture_output=True, text=True, encoding='utf-8', errors='replace')
    (RUN / 'project-replacement-report.txt').write_text(result.stdout + result.stderr, encoding='utf-8')
    assert result.returncode == 0, result.stdout + result.stderr
    assert editor_closed() and digest(project) == before_hash, 'Project was opened or saved while preparing replacement'
    project_backup = project.with_name(project.name + '.before-alpha-' + stamp + '.bak')
    shutil.copy2(project, project_backup)
    temporary = project.with_name(project.name + '.alpha-installing')
    shutil.copy2(candidate, temporary)
    assert digest(temporary) == digest(candidate)
    os.replace(temporary, project)
    # The active files now have MOV names. Remove redundant old active files only
    # after verified backups and the project switch, using explicit checked paths.
    for row in replacements:
        old = Path(row['old'])
        assert old.parent.resolve() == renders
        assert digest(backup / old.name) == originals[row['old']]
        old.unlink()
    state.update(stage='installed', project=str(project), project_backup=str(project_backup),
                 original_media_backup=str(backup), installed_at=stamp)
    atomic_json(state_path, state)
    # Only temporary duplicate batch tensors/checkpoints; retained project caches
    # are outside the release endpoint's directory and remain untouched.
    for row in replacements:
        number = int(Path(row['new']).name[:3])
        request('/roundtable_batch/release', {'artifact_key': state['batch_id'] + '_' + str(number)})
    print(result.stdout)
    print('Installed all selected alpha replacements; original project and media backed up.')


if __name__ == '__main__':
    main()
