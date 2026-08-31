#!/usr/bin/env python3
"""Baja de PokeAPI los nombres oficiales por idioma y escribe tools/dex_names.py.

  python3 tools/gen_names.py

FR, DE y ZH usan sus nombres localizados; ES/IT/PT siguen usando los ingleses.
La fuente del firmware ZH es Unicode, mientras que las otras variantes siguen
normalizando los nombres a ASCII.
"""
import json
import os
import subprocess
import time
import unicodedata

LANGS = {'fr': 'fr', 'de': 'de', 'zh': 'zh-hans'}


def ascii_up(s):
    """'Salamèche' -> 'SALAMECHE'; conserva los simbolos de genero como f/m."""
    s = s.replace('♀', 'F').replace('♂', 'M')  # Nidoran hembra/macho
    s = unicodedata.normalize('NFD', s)
    s = ''.join(c for c in s if unicodedata.category(c) != 'Mn')
    return s.upper()


def fetch(num):
    url = f'https://pokeapi.co/api/v2/pokemon-species/{num}/'
    for intento in range(4):
        r = subprocess.run(['curl', '-s', '--max-time', '25', '-A', 'Mozilla/5.0', url],
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode == 0 and r.stdout.strip().startswith('{'):
            return json.loads(r.stdout)
        time.sleep(1 + intento)
    raise SystemExit(f'no se pudo bajar la especie {num}')


def main():
    out = {}
    for num in range(1, 152):
        d = fetch(num)
        names = {n['language']['name']: n['name'] for n in d['names']}
        en = ascii_up(names['en'])
        dif = {}
        for lg, api_lg in LANGS.items():
            v = names.get(api_lg, names['en'])
            if lg != 'zh':
                v = ascii_up(v)
            if v != en:
                dif[lg] = v
        if dif:
            out[num] = dif
        if num % 25 == 0:
            print(f'  {num}/151...')
        time.sleep(0.05)

    path = os.path.join(os.path.dirname(__file__), 'dex_names.py')
    with open(path, 'w', encoding='utf-8') as f:
        f.write('# -*- coding: utf-8 -*-\n')
        f.write('"""GENERADO por tools/gen_names.py desde PokeAPI - no editar a mano.\n\n')
        f.write('Nombres oficiales localizados de FR, DE y ZH para la Pokedex gen 1.\n')
        f.write('FR/DE se normalizan a ASCII; ZH conserva sus caracteres Unicode.\n"""\n\n')
        f.write('LOCAL_NAMES = {\n')
        for num in sorted(out):
            f.write(f'    {num}: {out[num]!r},\n')
        f.write('}\n')
    print(f'guardado {os.path.normpath(path)}: {len(out)} especies con nombre propio')


if __name__ == '__main__':
    main()
