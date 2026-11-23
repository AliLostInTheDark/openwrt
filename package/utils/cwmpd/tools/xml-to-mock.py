#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Turn a TR-098/TR-181 configuration backup (the XML tree a Zyxel-style CPE
saves, <InternetGatewayDevice> with <iN> instances and TYPE attributes) into
the mock data model cwmpd answers from: name, xsd type and value, tab
separated. The output holds the account's data; keep it off shared storage.

    xml-to-mock.py config.xml > mock.tsv
"""
import sys
import xml.etree.ElementTree as ET

TYPES = {
    'string': 'xsd:string', 'mac': 'xsd:string',
    'boolean': 'xsd:boolean', 'bool': 'xsd:boolean',
    'int': 'xsd:int', 'int8': 'xsd:int', 'int16': 'xsd:int', 'int32': 'xsd:int',
    'sint7': 'xsd:int', 'sint15': 'xsd:int', 'sint31': 'xsd:int',
    'uint8': 'xsd:unsignedInt', 'uint16': 'xsd:unsignedInt', 'uint32': 'xsd:unsignedInt',
    'unsignedint': 'xsd:unsignedInt', 'int64': 'xsd:long', 'long': 'xsd:long',
    'uint64': 'xsd:unsignedLong', 'unsignedlong': 'xsd:unsignedLong',
    'datetime': 'xsd:dateTime', 'base64': 'xsd:base64', 'hexbinary': 'xsd:hexBinary',
}


def segment(tag):
    # <i3> is instance 3 of the object it sits in
    return tag[1:] if tag[:1] == 'i' and tag[1:].isdigit() else tag


def escape(v):
    return v.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '')


def walk(node, path, out):
    for child in node:
        name = path + segment(child.tag)
        if len(child):
            walk(child, name + '.', out)
        elif child.get('TYPE') is not None or child.text is not None:
            t = TYPES.get((child.get('TYPE') or 'string').lower(), 'xsd:string')
            out.append((name, t, (child.text or '').strip()))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = ET.parse(sys.argv[1]).getroot()
    out = []
    walk(root, root.tag + '.', out)
    for name, t, value in out:
        print('%s\t%s\t%s' % (name, t, escape(value)))
    print('%d parameters' % len(out), file=sys.stderr)


if __name__ == '__main__':
    main()
