#!/usr/bin/env python3
"""
Bridge codegen: generates Venus-compatible encoder/decoder from vk.xml.

Usage: python3 bridge_codegen.py [--vk-xml PATH] [--outdir PATH]

Generates:
  vn_gen_command.h  - Command ID enum (Venus VkCommandTypeEXT values)
  vn_gen_encode.h   - Encoder functions: vn_encode_vkXxx(VnStreamWriter*, ...)
  vn_gen_decode.h   - Decoder functions: vn_decode_vkXxx(VnStreamReader*, ...)
  vn_gen_types.h    - Struct serializers
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET
from collections import OrderedDict
from bridge_api_list import CODEGEN_APIS, BRIDGE_COMMANDS


# ── vk.xml parsing ──────────────────────────────────────────────────────────

class VkType:
    """Parsed Vulkan type (handle, struct, enum, basetype)."""
    def __init__(self, name, category, members=None, parent=None, stype=None):
        self.name = name
        self.category = category  # 'handle', 'struct', 'enum', 'basetype', 'bitmask'
        self.members = members or []  # list of VkParam for structs
        self.parent = parent
        self.stype = stype  # VK_STRUCTURE_TYPE_* value for structs
        self.is_dispatchable = False

class VkParam:
    """A command parameter or struct member."""
    def __init__(self, name, type_name, is_const=False, is_pointer=False,
                 is_array=False, len_expr=None, optional=False):
        self.name = name
        self.type_name = type_name
        self.is_const = is_const
        self.is_pointer = is_pointer
        self.is_array = is_array
        self.len_expr = len_expr
        self.optional = optional

class VkCommand:
    """A parsed Vulkan command."""
    def __init__(self, name, return_type, params):
        self.name = name
        self.return_type = return_type
        self.params = params  # list of VkParam

class VkRegistry:
    """Parsed vk.xml registry."""
    def __init__(self, xml_path):
        self.types = {}      # name -> VkType
        self.commands = {}   # name -> VkCommand
        self.handles = set()
        self.enums = {}      # enum name -> dict of value_name -> value
        self._parse(xml_path)

    def _parse(self, xml_path):
        tree = ET.parse(xml_path)
        root = tree.getroot()
        self._parse_types(root)
        self._parse_commands(root)

    def _parse_types(self, root):
        for t in root.findall('.//type'):
            cat = t.get('category', '')
            name_el = t.find('name')
            name = t.get('name') or (name_el.text if name_el is not None else None)
            if not name:
                continue

            if cat == 'handle':
                type_el = t.find('type')
                is_disp = type_el is not None and type_el.text == 'VK_DEFINE_HANDLE'
                vt = VkType(name, 'handle', parent=t.get('parent'))
                vt.is_dispatchable = is_disp
                self.types[name] = vt
                self.handles.add(name)

            elif cat == 'struct':
                members = []
                stype_val = None
                for m in t.findall('member'):
                    mp = self._parse_param(m)
                    if mp:
                        members.append(mp)
                    if m.get('values'):
                        stype_val = m.get('values')
                vt = VkType(name, 'struct', members=members, stype=stype_val)
                self.types[name] = vt

            elif cat in ('basetype', 'bitmask', 'enum'):
                self.types[name] = VkType(name, cat)

    def _parse_param(self, elem):
        """Parse a <param> or <member> element."""
        type_el = elem.find('type')
        name_el = elem.find('name')
        if type_el is None or name_el is None:
            return None

        type_name = type_el.text
        name = name_el.text
        full_text = ''.join(elem.itertext())
        is_const = 'const' in full_text.split(name)[0]
        is_pointer = '*' in full_text
        len_expr = elem.get('len')
        optional = elem.get('optional', 'false') == 'true'

        return VkParam(
            name=name,
            type_name=type_name,
            is_const=is_const,
            is_pointer=is_pointer,
            is_array=bool(len_expr) and is_pointer,
            len_expr=len_expr,
            optional=optional,
        )

    def _parse_commands(self, root):
        for cmd_el in root.iter('command'):
            # Skip aliases
            if cmd_el.get('alias'):
                continue
            proto = cmd_el.find('proto')
            if proto is None:
                continue
            name_el = proto.find('name')
            type_el = proto.find('type')
            if name_el is None:
                continue

            params = []
            for p in cmd_el.findall('param'):
                pp = self._parse_param(p)
                if pp:
                    params.append(pp)

            self.commands[name_el.text] = VkCommand(
                name=name_el.text,
                return_type=type_el.text if type_el is not None else 'void',
                params=params,
            )

    def is_handle(self, type_name):
        return type_name in self.handles

    def is_struct(self, type_name):
        t = self.types.get(type_name)
        return t is not None and t.category == 'struct'

    def get_scalar_size(self, type_name):
        """Return wire size in bytes for scalar types, or 0 if not scalar."""
        SCALAR_MAP = {
            'uint32_t': 4, 'int32_t': 4, 'float': 4,
            'uint64_t': 8, 'int64_t': 8, 'double': 8,
            'VkBool32': 4, 'VkFlags': 4, 'VkFlags64': 8,
            'VkDeviceSize': 8, 'VkDeviceAddress': 8,
            'size_t': 8,  # serialize as u64 for portability
        }
        if type_name in SCALAR_MAP:
            return SCALAR_MAP[type_name]
        # Enums and bitmasks are u32
        t = self.types.get(type_name)
        if t and t.category in ('enum', 'bitmask'):
            return 4
        return 0


# ── Parameter classification ─────────────────────────────────────────────────

# Pointer types we skip entirely (always NULL in our bridge)
IGNORABLE_TYPES = {'VkAllocationCallbacks'}

def classify_param(reg, param):
    """Classify a parameter for codegen: handle, scalar, ignorable, handle_array, etc."""
    if not param.is_pointer:
        if reg.is_handle(param.type_name):
            return 'handle'
        if reg.get_scalar_size(param.type_name) > 0:
            return 'scalar'
        return 'unknown'

    # Pointer param
    if param.type_name in IGNORABLE_TYPES:
        return 'ignorable'

    if param.type_name == 'void' and param.len_expr:
        return 'byte_data'

    if param.len_expr:
        len_name = param.len_expr.split(',')[0].strip()
        if len_name.isidentifier() and not param.optional:
            if reg.is_handle(param.type_name):
                return 'handle_array'
            if reg.get_scalar_size(param.type_name) > 0:
                return 'scalar_array'

    return 'complex'

def can_generate(reg, cmd):
    """Check if all params can be code-generated (scalars, handles, ignorable, arrays)."""
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls in ('unknown', 'complex'):
            return False
    return True

def get_len_param_name(param):
    """Extract the count/size param name from len_expr."""
    if not param.len_expr:
        return None
    name = param.len_expr.split(',')[0].strip()
    return name if name.isidentifier() else None

def param_c_type_enc(reg, param, cls):
    """Return the C type for a param in the encoder function signature."""
    if cls == 'handle':
        return 'uint64_t'
    if cls == 'scalar':
        if param.type_name == 'float':
            return 'float'
        if param.type_name == 'int32_t':
            return 'int32_t'
        return 'uint64_t' if reg.get_scalar_size(param.type_name) == 8 else 'uint32_t'
    if cls == 'handle_array':
        return 'const uint64_t*'
    if cls == 'scalar_array':
        return 'const uint64_t*' if reg.get_scalar_size(param.type_name) == 8 else 'const uint32_t*'
    if cls == 'byte_data':
        return 'const void*'
    return None  # ignorable

def param_c_type_dec(reg, param, cls):
    """Return the C type for a param in the decoder struct."""
    if cls == 'handle':
        return 'uint64_t'
    if cls == 'scalar':
        if param.type_name == 'float':
            return 'float'
        if param.type_name == 'int32_t':
            return 'int32_t'
        return 'uint64_t' if reg.get_scalar_size(param.type_name) == 8 else 'uint32_t'
    if cls == 'handle_array':
        return 'std::vector<uint64_t>'
    if cls == 'scalar_array':
        elem = 'uint64_t' if reg.get_scalar_size(param.type_name) == 8 else 'uint32_t'
        return f'std::vector<{elem}>'
    if cls == 'byte_data':
        return 'std::vector<uint8_t>'
    return None  # ignorable


# ── Code generation ─────────────────────────────────────────────────────────

def gen_encoder(reg, cmd):
    """Generate encoder function for a command."""
    lines = []
    lines.append(f'static inline void vn_encode_{cmd.name}(VnStreamWriter* w')
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls == 'ignorable':
            continue
        ctype = param_c_type_enc(reg, p, cls)
        lines.append(f'    , {ctype} {p.name}')
    lines.append(')')
    lines.append('{')
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls == 'ignorable':
            continue
        if cls == 'handle':
            lines.append(f'    w->writeU64({p.name});')
        elif cls == 'scalar':
            if p.type_name == 'float':
                lines.append(f'    w->writeF32({p.name});')
            elif p.type_name == 'int32_t':
                lines.append(f'    w->writeI32({p.name});')
            elif reg.get_scalar_size(p.type_name) == 8:
                lines.append(f'    w->writeU64({p.name});')
            else:
                lines.append(f'    w->writeU32({p.name});')
        elif cls == 'handle_array':
            count = get_len_param_name(p)
            lines.append(f'    for (uint32_t i = 0; i < {count}; i++)')
            lines.append(f'        w->writeU64({p.name}[i]);')
        elif cls == 'scalar_array':
            count = get_len_param_name(p)
            wfn = 'writeU64' if reg.get_scalar_size(p.type_name) == 8 else 'writeU32'
            lines.append(f'    for (uint32_t i = 0; i < {count}; i++)')
            lines.append(f'        w->{wfn}({p.name}[i]);')
        elif cls == 'byte_data':
            size_param = get_len_param_name(p)
            lines.append(f'    w->writeBytes({p.name}, {size_param});')
    lines.append('}')
    return '\n'.join(lines)

def gen_decoder(reg, cmd):
    """Generate decoder struct + decode function for a command."""
    lines = []
    struct_name = f'VnDecode_{cmd.name}'
    lines.append(f'struct {struct_name} {{')
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls == 'ignorable':
            continue
        ctype = param_c_type_dec(reg, p, cls)
        lines.append(f'    {ctype} {p.name};')
    lines.append('};')
    lines.append('')
    lines.append(f'static inline void vn_decode_{cmd.name}(VnStreamReader* r, {struct_name}* args)')
    lines.append('{')
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls == 'ignorable':
            continue
        if cls == 'handle':
            lines.append(f'    args->{p.name} = r->readU64();')
        elif cls == 'scalar':
            if p.type_name == 'float':
                lines.append(f'    args->{p.name} = r->readF32();')
            elif p.type_name == 'int32_t':
                lines.append(f'    args->{p.name} = r->readI32();')
            elif reg.get_scalar_size(p.type_name) == 8:
                lines.append(f'    args->{p.name} = r->readU64();')
            else:
                lines.append(f'    args->{p.name} = r->readU32();')
        elif cls == 'handle_array':
            count = get_len_param_name(p)
            lines.append(f'    args->{p.name}.resize(args->{count});')
            lines.append(f'    for (uint32_t i = 0; i < args->{count}; i++)')
            lines.append(f'        args->{p.name}[i] = r->readU64();')
        elif cls == 'scalar_array':
            count = get_len_param_name(p)
            rfn = 'readU64' if reg.get_scalar_size(p.type_name) == 8 else 'readU32'
            lines.append(f'    args->{p.name}.resize(args->{count});')
            lines.append(f'    for (uint32_t i = 0; i < args->{count}; i++)')
            lines.append(f'        args->{p.name}[i] = r->{rfn}();')
        elif cls == 'byte_data':
            size_param = get_len_param_name(p)
            lines.append(f'    args->{p.name}.resize(args->{size_param});')
            lines.append(f'    r->readBytes(args->{p.name}.data(), args->{size_param});')
    lines.append('}')
    return '\n'.join(lines)


# ── File generation ─────────────────────────────────────────────────────────

HEADER = """\
// Auto-generated by codegen/bridge_codegen.py — DO NOT EDIT
// Source: vk.xml (Venus protocol standard)
#pragma once
"""

def gen_command_header(apis, bridge_cmds):
    """Generate vn_gen_command.h with Venus-standard command IDs."""
    lines = [HEADER]
    lines.append('#include <cstdint>')
    lines.append('')
    lines.append('// Venus-standard command IDs (from VkCommandTypeEXT)')
    lines.append('enum VnCommandId : uint32_t {')
    for name, cmd_id in sorted(apis.items(), key=lambda x: x[1]):
        lines.append(f'    VN_CMD_{name} = {cmd_id},')
    lines.append('')
    lines.append('    // Bridge-specific commands (not in Vulkan spec)')
    for name, cmd_id in sorted(bridge_cmds.items(), key=lambda x: x[1]):
        lines.append(f'    VN_CMD_{name} = 0x{cmd_id:X},')
    lines.append('};')
    return '\n'.join(lines)

def gen_encode_header(reg, apis):
    """Generate vn_gen_encode.h."""
    lines = [HEADER]
    lines.append('#include "vn_stream.h"')
    lines.append('')

    generated = []
    skipped = []

    for name in sorted(apis.keys()):
        cmd = reg.commands.get(name)
        if not cmd:
            skipped.append(f'// SKIP: {name} — not found in vk.xml')
            continue

        if can_generate(reg, cmd):
            lines.append(gen_encoder(reg, cmd))
            lines.append('')
            generated.append(name)
        else:
            # Show why it's complex
            complex_params = []
            for p in cmd.params:
                cls = classify_param(reg, p)
                if cls in ('unknown', 'complex'):
                    complex_params.append(f'{p.type_name}{"*" if p.is_pointer else ""} {p.name}')
            reason = ', '.join(complex_params) if complex_params else 'unknown'
            skipped.append(f'// TODO: {name} — complex: {reason}')

    if skipped:
        lines.append('// ── Not yet generated (complex parameter types) ──')
        lines.extend(skipped)
        lines.append('')

    return '\n'.join(lines), generated, [s.split(':')[1].strip() for s in skipped]

def gen_decode_header(reg, apis, generated_names):
    """Generate vn_gen_decode.h."""
    lines = [HEADER]
    lines.append('#include "vn_stream.h"')
    lines.append('#include <vector>')
    lines.append('')

    for name in sorted(generated_names):
        cmd = reg.commands.get(name)
        if cmd and can_generate(reg, cmd):
            lines.append(gen_decoder(reg, cmd))
            lines.append('')

    return '\n'.join(lines)


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description='Bridge codegen from vk.xml')
    parser.add_argument('--vk-xml', default=os.path.join(
        os.path.dirname(__file__), '..', 'third_party', 'venus-protocol', 'xmls', 'vk.xml'))
    parser.add_argument('--outdir', default=os.path.join(
        os.path.dirname(__file__), '..', 'common', 'venus'))
    args = parser.parse_args()

    print(f'Parsing {args.vk_xml}...')
    reg = VkRegistry(args.vk_xml)
    print(f'  {len(reg.commands)} commands, {len(reg.types)} types, {len(reg.handles)} handles')

    os.makedirs(args.outdir, exist_ok=True)

    # Generate command IDs
    cmd_h = gen_command_header(CODEGEN_APIS, BRIDGE_COMMANDS)
    with open(os.path.join(args.outdir, 'vn_gen_command.h'), 'w') as f:
        f.write(cmd_h)
    print(f'  wrote vn_gen_command.h')

    # Generate encoder
    enc_h, generated, skipped = gen_encode_header(reg, CODEGEN_APIS)
    with open(os.path.join(args.outdir, 'vn_gen_encode.h'), 'w') as f:
        f.write(enc_h)
    print(f'  wrote vn_gen_encode.h ({len(generated)} generated, {len(skipped)} skipped)')
    if generated:
        print(f'    generated: {", ".join(generated)}')
    if skipped:
        print(f'    skipped: {", ".join(skipped)}')

    # Generate decoder
    dec_h = gen_decode_header(reg, CODEGEN_APIS, generated)
    with open(os.path.join(args.outdir, 'vn_gen_decode.h'), 'w') as f:
        f.write(dec_h)
    print(f'  wrote vn_gen_decode.h')

    # Placeholder for types (structs)
    types_h = HEADER + '\n// Struct serializers will be added as complex commands are migrated.\n'
    with open(os.path.join(args.outdir, 'vn_gen_types.h'), 'w') as f:
        f.write(types_h)
    print(f'  wrote vn_gen_types.h (placeholder)')

    print(f'\nDone. {len(generated)}/{len(CODEGEN_APIS)} APIs generated.')

if __name__ == '__main__':
    main()
