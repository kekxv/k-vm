#!/usr/bin/env python3
"""Stream CI commands and append failure diagnostics to Actions Summary."""
import argparse
import html
import json
import os
from pathlib import Path
import shlex
import shutil
import sys
import subprocess
import tempfile
from urllib.parse import urlparse
from urllib.request import url2pathname


def failed_tests(events):
    if not events.exists():
        return
    seen = set()
    for line in events.read_text(errors='replace').splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue  # A crashed build can leave a partial final event.
        result = event.get('testResult', {})
        if not result or result.get('status') in ('PASSED', 'FLAKY'):
            continue
        label = event.get('id', {}).get('testResult', {}).get('label', 'unknown test')
        for output in result.get('testActionOutput', []):
            uri = output.get('uri', '')
            if output.get('name') != 'test.log' or not uri.startswith('file:'):
                continue
            parsed = urlparse(uri)
            path = Path(url2pathname(parsed.path))
            if parsed.netloc:
                path = Path('//' + parsed.netloc + str(path))
            if path in seen or not path.is_file():
                continue
            seen.add(path)
            yield label, path


def detail(title, body, limit):
    if len(body) > limit:
        body = '[Earlier output omitted; see uploaded full logs.]\n' + body[-limit:]
    return ('<details open><summary>' + html.escape(title) + '</summary>\n\n<pre>'
            + html.escape(body) + '</pre>\n</details>\n\n')


def main():
    sys.stdout.reconfigure(errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bazel', action='store_true', help='collect Bazel Build Event Protocol logs')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command[:1] == ['--']:
        command = command[1:]
    if not command:
        parser.error('a command is required after --')
    root = Path(os.environ.get('RUNNER_TEMP', tempfile.gettempdir())) / 'k-vm-ci-logs'
    root.mkdir(parents=True, exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix='command-', dir=root))
    events = logs / 'events.jsonl'
    display = shlex.join(command)
    if args.bazel:
        # Bazel command options follow the verb (and any startup options).
        verb = next((i for i, value in enumerate(command) if value in ('test', 'build')), None)
        command.insert(verb + 1 if verb is not None else len(command),
                       '--build_event_json_file=' + str(events))
    log = logs / 'command.log'
    with log.open('w', encoding='utf-8') as out:
        try:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, encoding='utf-8', errors='replace', bufsize=1)
            for line in process.stdout:
                print(line, end='', flush=True)
                out.write(line)
            code = process.wait()
        except OSError as error:
            print(error, flush=True)
            out.write(str(error) + '\n')
            code = 127
    if code:
        report = '### CI command failed\n\nExit code: `' + str(code) + '`\n\n'
        report += detail('Command', display, 4000)
        report += detail('Command output (last 24,000 characters)', log.read_text(), 24000)
        total = len(report)
        for index, (label, path) in enumerate(failed_tests(events)):
            shutil.copyfile(path, logs / ('failed-' + str(index) + '.log'))
            section = detail(label, path.read_text(errors='replace'), 16000)
            if total + len(section) <= 150000:
                report += section
                total += len(section)
            elif 'Summary limit reached' not in report:
                report += 'Summary limit reached; remaining test logs are in the uploaded artifact.\n'
        summary = os.environ.get('GITHUB_STEP_SUMMARY')
        if summary:
            with open(summary, 'a', encoding='utf-8') as out:
                out.write(report)
        print('Full CI logs: ' + str(logs), flush=True)
    return code if code >= 0 or os.name == 'nt' else 128 - code


if __name__ == '__main__':
    raise SystemExit(main())
