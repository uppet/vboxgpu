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


# ── Struct serialization ─────────────────────────────────────────────────────

def _classify_struct_member(reg, m, depth=0):
    """Classify a struct member for serialization. Returns classification string or None if unsupported."""
    if m.name in ('sType', 'pNext'):
        return 'skip'
    if not m.is_pointer:
        if reg.is_handle(m.type_name):
            return 'handle'
        if reg.get_scalar_size(m.type_name) > 0:
            return 'scalar'
        if reg.is_struct(m.type_name) and depth < 2:
            if _all_struct_members_ok(reg, m.type_name, depth + 1):
                return 'nested_struct'
        return None
    # Pointer member
    if m.optional and not m.len_expr:
        return 'skip'  # optional non-array pointer, skip
    if m.len_expr:
        len_name = m.len_expr.split(',')[0].strip()
        if not len_name.isidentifier():
            return None  # latexmath or complex len expression
        if reg.is_handle(m.type_name):
            return 'handle_array'
        if reg.get_scalar_size(m.type_name) > 0:
            return 'scalar_array'
        if reg.is_struct(m.type_name) and depth < 2:
            if _all_struct_members_ok(reg, m.type_name, depth + 1):
                return 'struct_array'
        return None
    return None

def _all_struct_members_ok(reg, type_name, depth=0):
    """Check if all members of a struct can be serialized."""
    t = reg.types.get(type_name)
    if not t or t.category != 'struct':
        return False
    for m in t.members:
        if _classify_struct_member(reg, m, depth) is None:
            return False
    return True

def is_simple_scalar_struct(reg, type_name, depth=0):
    """Check if a struct can be serialized (scalar members, handle/scalar/struct arrays)."""
    return _all_struct_members_ok(reg, type_name, depth)

def can_serialize_struct(reg, type_name):
    """Check if codegen can serialize this struct type."""
    return is_simple_scalar_struct(reg, type_name)

def _scalar_wire_info(reg, type_name):
    """Return (write_fn, read_fn, c_type) for a scalar type, or None."""
    if reg.is_handle(type_name):
        return ('writeU64', 'readU64', 'uint64_t')
    if type_name == 'float':
        return ('writeF32', 'readF32', 'float')
    if type_name == 'int32_t':
        return ('writeI32', 'readI32', 'int32_t')
    if reg.get_scalar_size(type_name) == 8:
        return ('writeU64', 'readU64', 'uint64_t')
    if reg.get_scalar_size(type_name) > 0:
        return ('writeU32', 'readU32', 'uint32_t')
    return None

# Wire member types for struct serialization
# Each entry is a dict with 'kind' and relevant fields
# kind='scalar': write_fn, read_fn, field, c_type
# kind='nested': field, sub_members (list of scalar entries)
# kind='array': field, count_field, elem_kind, elem_info

def get_struct_wire_plan(reg, struct_type):
    """Return a serialization plan for a struct: list of wire ops."""
    plan = []
    for m in struct_type.members:
        cls = _classify_struct_member(reg, m)
        if cls in ('skip', None):
            continue
        if cls in ('handle', 'scalar'):
            info = _scalar_wire_info(reg, m.type_name)
            if info:
                plan.append({'kind': 'scalar', 'field': m.name,
                             'write_fn': info[0], 'read_fn': info[1], 'c_type': info[2]})
        elif cls == 'nested_struct':
            nested = reg.types[m.type_name]
            subs = []
            for nm in nested.members:
                if nm.name in ('sType', 'pNext') or nm.is_pointer:
                    continue
                info = _scalar_wire_info(reg, nm.type_name)
                if info:
                    subs.append({'field': nm.name, 'write_fn': info[0],
                                 'read_fn': info[1], 'c_type': info[2]})
            plan.append({'kind': 'nested', 'field': m.name, 'sub_members': subs})
        elif cls in ('handle_array', 'scalar_array'):
            info = _scalar_wire_info(reg, m.type_name)
            count_name = m.len_expr.split(',')[0].strip()
            plan.append({'kind': 'array', 'field': m.name, 'count_field': count_name,
                         'elem_write': info[0], 'elem_read': info[1], 'elem_c_type': info[2]})
        elif cls == 'struct_array':
            nested = reg.types[m.type_name]
            count_name = m.len_expr.split(',')[0].strip()
            subs = []
            for nm in nested.members:
                if nm.name in ('sType', 'pNext') or nm.is_pointer:
                    continue
                info = _scalar_wire_info(reg, nm.type_name)
                if info:
                    subs.append({'field': nm.name, 'write_fn': info[0],
                                 'read_fn': info[1], 'c_type': info[2]})
            plan.append({'kind': 'struct_array', 'field': m.name, 'count_field': count_name,
                         'struct_name': m.type_name, 'sub_members': subs})
    return plan

def get_struct_wire_members(reg, struct_type):
    """Return flat list of (write_fn, read_fn, field_expr, c_type) for simple struct members.
    Only returns scalar and nested-scalar members (no arrays)."""
    result = []
    plan = get_struct_wire_plan(reg, struct_type)
    for op in plan:
        if op['kind'] == 'scalar':
            result.append((op['write_fn'], op['read_fn'], op['field'], op['c_type']))
        elif op['kind'] == 'nested':
            for sub in op['sub_members']:
                result.append((sub['write_fn'], sub['read_fn'],
                               f"{op['field']}.{sub['field']}", sub['c_type']))
        # arrays handled separately in gen_encoder/gen_decoder
    return result

def _has_array_members(reg, struct_type):
    """Check if struct has any array members in its wire plan."""
    plan = get_struct_wire_plan(reg, struct_type)
    return any(op['kind'] in ('array', 'struct_array') for op in plan)

def _emit_struct_decoder_fields(lines, plan, prefix):
    """Emit decoder struct fields for a struct wire plan."""
    for op in plan:
        if op['kind'] == 'scalar':
            lines.append(f'    {op["c_type"]} {prefix}{op["field"]};')
        elif op['kind'] == 'nested':
            for sub in op['sub_members']:
                lines.append(f'    {sub["c_type"]} {prefix}{op["field"]}_{sub["field"]};')
        elif op['kind'] == 'array':
            lines.append(f'    std::vector<{op["elem_c_type"]}> {prefix}{op["field"]};')
        elif op['kind'] == 'struct_array':
            # For struct arrays, store each sub-member as a vector
            for sub in op['sub_members']:
                lines.append(f'    std::vector<{sub["c_type"]}> {prefix}{op["field"]}_{sub["field"]};')

def _emit_struct_decode(lines, plan, prefix):
    """Emit decoder read lines for a struct wire plan."""
    for op in plan:
        if op['kind'] == 'scalar':
            lines.append(f'    {prefix}{op["field"]} = r->{op["read_fn"]}();')
        elif op['kind'] == 'nested':
            for sub in op['sub_members']:
                lines.append(f'    {prefix}{op["field"]}_{sub["field"]} = r->{sub["read_fn"]}();')
        elif op['kind'] == 'array':
            f, c = op['field'], op['count_field']
            lines.append(f'    {prefix}{f}.resize({prefix}{c});')
            lines.append(f'    for (uint32_t _i = 0; _i < {prefix}{c}; _i++)')
            lines.append(f'        {prefix}{f}[_i] = r->{op["elem_read"]}();')
        elif op['kind'] == 'struct_array':
            f, c = op['field'], op['count_field']
            for sub in op['sub_members']:
                lines.append(f'    {prefix}{f}_{sub["field"]}.resize({prefix}{c});')
            lines.append(f'    for (uint32_t _i = 0; _i < {prefix}{c}; _i++) {{')
            for sub in op['sub_members']:
                lines.append(f'        {prefix}{f}_{sub["field"]}[_i] = r->{sub["read_fn"]}();')
            lines.append(f'    }}')

def _cast_expr(write_fn, expr):
    """Wrap expression in a cast if it's a handle being written as u64."""
    if write_fn == 'writeU64':
        return f'(uint64_t){expr}'
    return expr

def _emit_struct_encode(lines, plan, prefix):
    """Emit encoder lines for a struct wire plan. prefix is 'p->' or 'name_'."""
    for op in plan:
        if op['kind'] == 'scalar':
            lines.append(f'    w->{op["write_fn"]}({_cast_expr(op["write_fn"], prefix + op["field"])});')
        elif op['kind'] == 'nested':
            for sub in op['sub_members']:
                lines.append(f'    w->{sub["write_fn"]}({prefix}{op["field"]}.{sub["field"]});')
        elif op['kind'] == 'array':
            f, c = op['field'], op['count_field']
            lines.append(f'    for (uint32_t _i = 0; _i < {prefix}{c}; _i++)')
            lines.append(f'        w->{op["elem_write"]}({_cast_expr(op["elem_write"], prefix + f + "[_i]")});')
        elif op['kind'] == 'struct_array':
            f, c = op['field'], op['count_field']
            lines.append(f'    for (uint32_t _i = 0; _i < {prefix}{c}; _i++) {{')
            for sub in op['sub_members']:
                lines.append(f'        w->{sub["write_fn"]}({_cast_expr(sub["write_fn"], prefix + f + "[_i]." + sub["field"])});')
            lines.append(f'    }}')


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
        if len_name.isidentifier():
            if reg.is_handle(param.type_name):
                if param.is_const:
                    return 'handle_array'
                else:
                    return 'output_handle_array'
            if reg.get_scalar_size(param.type_name) > 0:
                return 'scalar_array'  # works for both required and optional
            if param.is_const and reg.is_struct(param.type_name):
                if can_serialize_struct(reg, param.type_name):
                    return 'struct_array'

    # Output handle pointer: non-const handle* without len (e.g. VkFence* pFence)
    if not param.is_const and not param.len_expr and reg.is_handle(param.type_name):
        return 'output_handle'

    # Const struct pointer without len (e.g. const VkFenceCreateInfo* pCreateInfo)
    if param.is_const and not param.len_expr and reg.is_struct(param.type_name):
        if can_serialize_struct(reg, param.type_name):
            return 'struct'

    return 'complex'

def can_generate(reg, cmd):
    """Check if all params can be code-generated (scalars, handles, ignorable, arrays, structs)."""
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
    if cls == 'output_handle':
        return 'uint64_t'  # ICD sends the assigned handle ID
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
    if cls == 'output_handle_array':
        return None  # handled specially (output, not sent)
    if cls == 'struct':
        return None  # struct members are inlined, no single param
    if cls == 'struct_array':
        return f'const {param.type_name}*'
    return None  # ignorable

def param_c_type_dec(reg, param, cls):
    """Return the C type for a param in the decoder struct."""
    if cls == 'handle':
        return 'uint64_t'
    if cls == 'output_handle':
        return 'uint64_t'
    if cls == 'scalar':
        if param.type_name == 'float':
            return 'float'
        if param.type_name == 'int32_t':
            return 'int32_t'
        return 'uint64_t' if reg.get_scalar_size(param.type_name) == 8 else 'uint32_t'
    if cls == 'handle_array':
        return 'std::vector<uint64_t>'
    if cls == 'output_handle_array':
        return 'std::vector<uint64_t>'
    if cls == 'scalar_array':
        elem = 'uint64_t' if reg.get_scalar_size(param.type_name) == 8 else 'uint32_t'
        return f'std::vector<{elem}>'
    if cls == 'byte_data':
        return 'std::vector<uint8_t>'
    if cls == 'struct':
        return None  # struct members inlined into decoder struct
    if cls == 'struct_array':
        return None  # handled specially
    return None  # ignorable


# ── Code generation ─────────────────────────────────────────────────────────

def _needs_reorder(reg, cmd):
    """Check if command has output_handle or struct params that need reordering."""
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls in ('output_handle', 'output_handle_array', 'struct'):
            return True
    return False

def _order_params(reg, cmd):
    """Reorder params for Create commands: output handles moved right after input handles,
    before struct fields. This matches existing manual wire format: (device, objectId, ...struct).
    For other commands (Cmd*, Queue*), keep Vulkan spec order."""
    if not _needs_reorder(reg, cmd):
        return cmd.params  # keep original order

    first = []   # input handles + output handles
    rest = []    # everything else (scalars, structs, arrays)
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls in ('handle', 'output_handle', 'output_handle_array'):
            first.append(p)
        else:
            rest.append(p)
    return first + rest

def _get_consumed_count_params(reg, cmd):
    """Return set of param names that are consumed as counts by struct_array/output_handle_array params."""
    consumed = set()
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls in ('struct_array', 'output_handle_array'):
            count = get_len_param_name(p)
            if count:
                consumed.add(count)
    return consumed

def gen_encoder(reg, cmd):
    """Generate encoder function for a command."""
    lines = []
    lines.append(f'static inline void vn_encode_{cmd.name}(VnStreamWriter* w')

    ordered = _order_params(reg, cmd)
    consumed_counts = _get_consumed_count_params(reg, cmd)

    # Build parameter list: struct params become individual member params or struct pointer
    for p in ordered:
        cls = classify_param(reg, p)
        if cls == 'ignorable':
            continue
        if p.name in consumed_counts:
            continue  # count param consumed by struct_array/output_handle_array
        if cls == 'output_handle_array':
            # Output handle arrays: send count + handle IDs
            count = get_len_param_name(p)
            lines.append(f'    , uint32_t {count}')
            lines.append(f'    , const uint64_t* {p.name}')
            continue
        if cls == 'struct':
            st = reg.types[p.type_name]
            if _has_array_members(reg, st):
                lines.append(f'    , const {p.type_name}* {p.name}')
            else:
                for wfn, rfn, field, ctype in get_struct_wire_members(reg, st):
                    safe_name = field.replace('.', '_')
                    lines.append(f'    , {ctype} {p.name}_{safe_name}')
            continue
        if cls == 'struct_array':
            lines.append(f'    , uint32_t {get_len_param_name(p)}')
            lines.append(f'    , const {p.type_name}* {p.name}')
            continue
        ctype = param_c_type_enc(reg, p, cls)
        if ctype:
            lines.append(f'    , {ctype} {p.name}')
    lines.append(')')
    lines.append('{')

    for p in ordered:
        cls = classify_param(reg, p)
        if cls == 'ignorable':
            continue
        if p.name in consumed_counts:
            continue  # written by struct_array/output_handle_array handler
        if cls in ('handle', 'output_handle'):
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
        elif cls == 'struct':
            st = reg.types[p.type_name]
            plan = get_struct_wire_plan(reg, st)
            if _has_array_members(reg, st):
                # Struct pointer — access members via ->
                _emit_struct_encode(lines, plan, f'{p.name}->')
            else:
                # Inlined params
                for wfn, rfn, field, ctype in get_struct_wire_members(reg, st):
                    safe_name = field.replace('.', '_')
                    lines.append(f'    w->{wfn}({p.name}_{safe_name});')
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
        elif cls == 'output_handle_array':
            count = get_len_param_name(p)
            lines.append(f'    w->writeU32({count});')
            lines.append(f'    for (uint32_t i = 0; i < {count}; i++)')
            lines.append(f'        w->writeU64({p.name}[i]);')
        elif cls == 'struct_array':
            count = get_len_param_name(p)
            st = reg.types[p.type_name]
            plan = get_struct_wire_plan(reg, st)
            lines.append(f'    w->writeU32({count});')
            lines.append(f'    for (uint32_t _i = 0; _i < {count}; _i++) {{')
            for op in plan:
                if op['kind'] == 'scalar':
                    expr = f'{p.name}[_i].{op["field"]}'
                    lines.append(f'        w->{op["write_fn"]}({_cast_expr(op["write_fn"], expr)});')
                elif op['kind'] == 'nested':
                    for sub in op['sub_members']:
                        expr = f'{p.name}[_i].{op["field"]}.{sub["field"]}'
                        lines.append(f'        w->{sub["write_fn"]}({_cast_expr(sub["write_fn"], expr)});')
            lines.append(f'    }}')
    lines.append('}')
    return '\n'.join(lines)

def gen_decoder(reg, cmd):
    """Generate decoder struct + decode function for a command."""
    lines = []
    struct_name = f'VnDecode_{cmd.name}'

    ordered = _order_params(reg, cmd)
    consumed_counts = _get_consumed_count_params(reg, cmd)

    lines.append(f'struct {struct_name} {{')
    for p in ordered:
        cls = classify_param(reg, p)
        if cls == 'ignorable':
            continue
        if p.name in consumed_counts:
            continue  # count field declared by struct_array/output_handle_array
        if cls == 'struct':
            st = reg.types[p.type_name]
            plan = get_struct_wire_plan(reg, st)
            if _has_array_members(reg, st):
                _emit_struct_decoder_fields(lines, plan, f'{p.name}_')
            else:
                for wfn, rfn, field, ctype in get_struct_wire_members(reg, st):
                    safe_name = field.replace('.', '_')
                    lines.append(f'    {ctype} {p.name}_{safe_name};')
        elif cls == 'struct_array':
            count = get_len_param_name(p)
            lines.append(f'    uint32_t {count};')
            st = reg.types[p.type_name]
            plan = get_struct_wire_plan(reg, st)
            # Each scalar/nested field becomes a vector
            for op in plan:
                if op['kind'] == 'scalar':
                    lines.append(f'    std::vector<{op["c_type"]}> {p.name}_{op["field"]};')
                elif op['kind'] == 'nested':
                    for sub in op['sub_members']:
                        lines.append(f'    std::vector<{sub["c_type"]}> {p.name}_{op["field"]}_{sub["field"]};')
        else:
            ctype = param_c_type_dec(reg, p, cls)
            if ctype:
                lines.append(f'    {ctype} {p.name};')
    lines.append('};')
    lines.append('')
    lines.append(f'static inline void vn_decode_{cmd.name}(VnStreamReader* r, {struct_name}* args)')
    lines.append('{')
    for p in ordered:
        cls = classify_param(reg, p)
        if cls == 'ignorable':
            continue
        if p.name in consumed_counts:
            continue  # read by struct_array/output_handle_array handler
        if cls in ('handle', 'output_handle'):
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
        elif cls == 'struct':
            st = reg.types[p.type_name]
            plan = get_struct_wire_plan(reg, st)
            if _has_array_members(reg, st):
                _emit_struct_decode(lines, plan, f'args->{p.name}_')
            else:
                for wfn, rfn, field, ctype in get_struct_wire_members(reg, st):
                    safe_name = field.replace('.', '_')
                    lines.append(f'    args->{p.name}_{safe_name} = r->{rfn}();')
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
        elif cls == 'output_handle_array':
            count = get_len_param_name(p)
            lines.append(f'    uint32_t {count} = r->readU32();')
            lines.append(f'    args->{p.name}.resize({count});')
            lines.append(f'    for (uint32_t i = 0; i < {count}; i++)')
            lines.append(f'        args->{p.name}[i] = r->readU64();')
        elif cls == 'struct_array':
            count = get_len_param_name(p)
            st = reg.types[p.type_name]
            plan = get_struct_wire_plan(reg, st)
            lines.append(f'    args->{count} = r->readU32();')
            for op in plan:
                if op['kind'] == 'scalar':
                    lines.append(f'    args->{p.name}_{op["field"]}.resize(args->{count});')
                elif op['kind'] == 'nested':
                    for sub in op['sub_members']:
                        lines.append(f'    args->{p.name}_{op["field"]}_{sub["field"]}.resize(args->{count});')
            lines.append(f'    for (uint32_t _i = 0; _i < args->{count}; _i++) {{')
            for op in plan:
                if op['kind'] == 'scalar':
                    lines.append(f'        args->{p.name}_{op["field"]}[_i] = r->{op["read_fn"]}();')
                elif op['kind'] == 'nested':
                    for sub in op['sub_members']:
                        lines.append(f'        args->{p.name}_{op["field"]}_{sub["field"]}[_i] = r->{sub["read_fn"]}();')
            lines.append(f'    }}')
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

def _uses_vulkan_types(reg, cmd):
    """Check if a command's generated encoder uses Vulkan struct types (needs VK_VERSION_1_0 guard)."""
    for p in cmd.params:
        cls = classify_param(reg, p)
        if cls == 'struct':
            st = reg.types[p.type_name]
            if _has_array_members(reg, st):
                return True  # uses const VkXxxCreateInfo* param
        if cls in ('struct_array',):
            return True  # uses const VkXxx* param
    return False

def gen_encode_header(reg, apis):
    """Generate vn_gen_encode.h."""
    lines = [HEADER]
    lines.append('#include "vn_stream.h"')
    lines.append('')

    generated = []
    skipped = []
    guarded = []  # functions that need VK_VERSION_1_0 guard

    for name in sorted(apis.keys()):
        cmd = reg.commands.get(name)
        if not cmd:
            skipped.append(f'// SKIP: {name} — not found in vk.xml')
            continue

        if can_generate(reg, cmd):
            if _uses_vulkan_types(reg, cmd):
                guarded.append(name)
            else:
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

    if guarded:
        lines.append('#ifdef VK_VERSION_1_0  // Functions that use Vulkan struct types')
        lines.append('')
        for name in sorted(guarded):
            cmd = reg.commands.get(name)
            if cmd:
                lines.append(gen_encoder(reg, cmd))
                lines.append('')
        lines.append('#endif // VK_VERSION_1_0')
        lines.append('')

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
