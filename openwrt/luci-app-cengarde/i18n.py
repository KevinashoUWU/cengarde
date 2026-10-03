#!/usr/bin/env python3
"""Keeps the translations in step with the views: rewrites
po/templates/cengarde.pot from the _('...') strings of the views and the
menu titles, and po/es/cengarde.po with the translations it already has
(new strings come in untranslated, unused ones are dropped). Exits 1 when
some string has no Spanish translation, so CI catches it.

    python3 openwrt/luci-app-cengarde/i18n.py

SPDX-License-Identifier: GPL-2.0-only
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PO = os.path.join(HERE, 'po/es/cengarde.po')
POT = os.path.join(HERE, 'po/templates/cengarde.pot')


def strings():
    out = []
    for f in sorted(glob.glob(os.path.join(HERE, 'htdocs/**/*.js'), recursive=True)):
        for m in re.finditer(r"_\(\s*'((?:[^'\\]|\\.)*)'\s*\)", open(f, encoding='utf-8').read()):
            s = re.sub(r"\\(.)", r"\1", m.group(1))
            if s not in out:
                out.append(s)
    for f in sorted(glob.glob(os.path.join(HERE, 'root/usr/share/luci/menu.d/*.json'))):
        for v in json.load(open(f, encoding='utf-8')).values():
            if v['title'] not in out:
                out.append(v['title'])
    return out


def unq(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s[1:-1])


def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def read_po(path):
    out, key, cur = {}, None, None
    if not os.path.exists(path):
        return out
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if line.startswith('msgid '):
            key, cur = unq(line[6:]), 'id'
        elif line.startswith('msgstr '):
            out[key] = unq(line[7:])
            cur = 'str'
        elif line.startswith('"') and key is not None:
            if cur == 'id':
                key += unq(line)
            else:
                out[key] += unq(line)
    out.pop('', None)
    return out


def write(path, header, items):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(header)
        for msgid, msgstr in items:
            f.write('\nmsgid %s\nmsgstr %s\n' % (q(msgid), q(msgstr)))


def main():
    s = strings()
    es = read_po(PO)
    write(POT, 'msgid ""\nmsgstr "Content-Type: text/plain; charset=UTF-8"\n', [(x, '') for x in s])
    write(PO, 'msgid ""\nmsgstr ""\n"Language: es\\n"\n"Content-Type: text/plain; charset=UTF-8\\n"\n',
          [(x, es.get(x, '')) for x in s])
    missing = [x for x in s if not es.get(x)]
    for x in missing:
        print('untranslated:', x)
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
