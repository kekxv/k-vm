#!/usr/bin/env python3
"""Read the project's literal MODULE.bazel version without executing Starlark."""
import argparse
import ast
from pathlib import Path
import re


def release_tag(path):
    tree = ast.parse(Path(path).read_text(encoding='utf-8'), filename=str(path))
    modules = [node.value for node in tree.body
               if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
               and isinstance(node.value.func, ast.Name) and node.value.func.id == 'module']
    if len(modules) != 1:
        raise ValueError('MODULE.bazel must contain exactly one module() declaration')
    values = [keyword.value for keyword in modules[0].keywords if keyword.arg == 'version']
    if len(values) != 1 or not isinstance(values[0], ast.Constant):
        raise ValueError('module() must declare a literal version string')
    version = values[0].value
    if not isinstance(version, str) or not re.fullmatch(
            r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
            r'(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?'
            r'(?:\+[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?', version):
        raise ValueError('module version must be a release version such as 0.1.0 or 0.2.0-rc.1')
    return 'v' + version


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', nargs='?', default='MODULE.bazel')
    args = parser.parse_args()
    try:
        print(release_tag(args.module))
    except (ValueError, SyntaxError, OSError) as error:
        parser.exit(1, str(error) + '\n')
