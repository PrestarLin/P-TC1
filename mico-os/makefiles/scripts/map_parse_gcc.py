#!/usr/bin/python3

import sys
import re
from functools import reduce

map_file = sys.argv[1]

total_ram = 0
total_rom = 0
map_lines = []
with open(map_file, 'r') as f:
    s = f.read()

    mem_config_text = re.findall(r'Memory Configuration\n\nName             Origin             Length             Attributes\n([\s\S]+)\nLinker script and memory map', s)[0]
    rom_config_text = re.findall(r'\w+\s+(0x\w+)\s+(0x\w+)\s+xr\n', mem_config_text)
    rom_config = []
    for rom in rom_config_text:
        rom_config += [{'start':int(rom[0], 16), 'end':int(rom[0], 16) + int(rom[1], 16)}]
    ram_config_text = re.findall(r'\w+\s+(0x\w+)\s+(0x\w+)\s+xrw\n', mem_config_text)
    ram_config = []
    for ram in ram_config_text:
        ram_config += [{'start':int(ram[0], 16), 'end':int(ram[0], 16) + int(ram[1], 16)}]

    mem_map = re.findall(r'Linker script and memory map([\s\S]+?)START GROUP', s)[0]

    modules = list(set(item[0] for item in re.findall(r'0x\w+\s+0x\w+\s+.+?([^/\\]+\.[ao])(\(.+\.o\))?\n', mem_map)))
    modules.sort(key = lambda x : x.upper())
    modules += ['*fill*']

    for module in modules:
        rom_size = 0
        ram_size = 0
        module = module.replace('+', r'\+')
        if(module == '*fill*'):
            sections = list(map(lambda arg : {'address':int(arg[0], 16), 'size':int(arg[1], 16)}, re.findall(r'\*fill\*[ \t]+(0x\w+)[ \t]+(0x\w+)[ \t]+\n', mem_map)))
        else:
            sections = list(map(lambda arg : {'address':int(arg[0], 16), 'size':int(arg[1], 16)}, re.findall(r'(0x\w+)[ \t]+(0x\w+)[ \t]+.+[/\\]'+module+'(\(.+\.o\))?\n', mem_map)))
        if(not sections):
            continue

        def ram_size_fn(arg):
            for ram_info in ram_config:
                if(ram_info['start'] < arg['address'] < ram_info['end']):
                    return arg['size']
            return 0

        def rom_size_fn(arg):
            for rom_info in rom_config:
                if(rom_info['start'] < arg['address'] < rom_info['end']):
                    return arg['size']
            return 0

        ram_size = reduce(lambda x,y:x+y, map(ram_size_fn, sections))
        rom_size = reduce(lambda x,y:x+y, map(rom_size_fn, sections))

        total_ram += ram_size
        total_rom += rom_size

        map_lines.append('| %-40s | %-8d  | %-8d |'%(re.sub(r'\.[ao]','',module)[:40],rom_size,ram_size))

print('\n                        MICO MEMORY MAP                            ')
print('|=================================================================|')
print('| %-40s | %-8s  | %-8s |'%('MODULE','ROM','RAM'))
print('|=================================================================|')
for line in map_lines:
    print(line)
print('|=================================================================|')
print('| %-40s | %-8d  | %-8d |'%('TOTAL (bytes)', total_rom, total_ram))
print('|=================================================================|')