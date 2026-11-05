#!/usr/bin/env python3
"""Manage virtual clip folders without changing filenames or patch clip indices."""
import argparse
import re
import clip_catalog


def folder_name(value):
    name = value.strip().upper().replace(' ', '_')
    if not re.fullmatch(r'[A-Z0-9_]{1,12}', name):
        raise ValueError('folder must be 1–12 letters, digits or underscores')
    return name


def resolve_clip(value, order):
    if value.isdigit():
        number = int(value)
        if not 1 <= number <= len(order): raise ValueError('clip number out of range')
        return order[number-1]
    if value not in order: raise ValueError('clip filename not in catalog')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('list', help='show stable clip numbers and folders')
    commands.add_parser('folders', help='show available folders')
    commands.add_parser('next', help='print the next filename number')
    add = commands.add_parser('add', help='add a virtual folder')
    add.add_argument('folder')
    move = commands.add_parser('move', help='assign an existing clip to a folder')
    move.add_argument('clip', help='1-based clip number or exact filename')
    move.add_argument('folder')
    args = parser.parse_args()
    if args.command == 'add':
        name = folder_name(args.folder)
        def mutate(data):
            if name in data['folders']: return
            if len(data['folders']) >= clip_catalog.MAX_FOLDERS: raise ValueError('folder limit (15)')
            data['folders'].append(name)
        data = clip_catalog.update(mutate)
        print('Folder: ' + name)
    elif args.command == 'move':
        name = folder_name(args.folder)
        def mutate(data):
            clip = resolve_clip(args.clip, data['order'])
            if name not in data['folders']: raise ValueError('unknown folder: ' + name)
            data['assigned'][clip] = name
        data = clip_catalog.update(mutate)
        print('Assigned %s to %s' % (resolve_clip(args.clip, data['order']), name))
    else:
        data = clip_catalog.update()
        if args.command == 'next':
            numbers = [int(match.group(1)) for clip in data['order']
                       if (match := re.match(r'^(\d+)_', clip))]
            print(max(numbers, default=0) + 1)
        elif args.command == 'folders':
            for name in data['folders']: print(name)
        else:
            for index, clip in enumerate(data['order'], 1):
                print('%03d  %-12s %s' % (index, data['assigned'].get(clip, 'CLASSICS'), clip))


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError) as error:
        raise SystemExit('Error: ' + str(error))
