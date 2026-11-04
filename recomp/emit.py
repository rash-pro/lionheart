import json
from pathlib import Path


def u(value):
    return f"0x{value & 0xFFFFFFFF:08x}u"


GTE_COMMANDS = {
    0x01: "rtps", 0x06: "nclip", 0x0C: "op", 0x10: "dpcs", 0x11: "intpl",
    0x12: "mvmva", 0x13: "ncds", 0x14: "cdp", 0x16: "ncdt", 0x1B: "nccs",
    0x1C: "cc", 0x1E: "ncs", 0x20: "nct", 0x28: "sqr", 0x29: "dcpl",
    0x2A: "dpct", 0x2D: "avsz3", 0x2E: "avsz4", 0x30: "rtpt", 0x3D: "gpf",
    0x3E: "gpl", 0x3F: "ncct",
}


def gte_command(word):
    """Resolve command and option fields before generating a call site."""
    command = word & 63
    name = GTE_COMMANDS.get(command)
    if name is None:
        return f"gte_cmd(c, {u(word)});"
    arguments = ["c"]
    if command not in {6, 45, 46}:
        arguments += [str(12 if word & (1 << 19) else 0), str((word >> 10) & 1)]
    if command == 18:
        arguments += [str((word >> bit) & 3) for bit in (17, 15, 13)]
    return f"gte_{name}({", ".join(arguments)});"


def gte_read_expression(reg, control):
    # Reads of FIFO aliases and color/leading-bit registers stay canonical.
    if control or reg not in {15, 28, 29, 30, 31}:
        return f"c->gte_{'ctrl' if control else 'data'}[{reg}]"
    return f"gte_read(c, {reg}, {int(control)})"


def gte_write_statement(reg, value, control):
    special = {4, 12, 20, 26, 27, 29, 30, 31} if control else {
        1, 3, 5, 7, 8, 9, 10, 11, 15, 16, 17, 18, 19, 28, 29, 30, 31}
    if reg not in special:
        return f"c->gte_{'ctrl' if control else 'data'}[{reg}] = {value};"
    return f"gte_write(c, {reg}, {value}, {int(control)});"


def effects(i):
    n, rs, rt, rd, sh = i.name, i.rs, i.rt, i.rd, i.sh
    imm = u(i.signed_imm)
    lines = [f"uint32_t a = c->r[{rs}], b = c->r[{rt}];"]
    dst, value, load, loaded = 0, "0", 0, "0"
    alu = {
        "sll": f"b << {sh}", "srl": f"b >> {sh}", "sra": f"c_sra(b, {sh})",
        "sllv": "b << (a & 31)", "srlv": "b >> (a & 31)", "srav": "c_sra(b, a)",
        "add": "c_add(c, a, b)", "addu": "a + b", "sub": "c_sub(c, a, b)",
        "subu": "a - b", "and": "a & b", "or": "a | b", "xor": "a ^ b",
        "nor": "~(a | b)", "slt": "(int32_t)a < (int32_t)b", "sltu": "a < b",
        "mfhi": "c->hi", "mflo": "c->lo",
    }
    immediate = {
        "addi": f"c_add(c, a, {imm})", "addiu": f"a + {imm}",
        "slti": f"(int32_t)a < (int32_t){imm}", "sltiu": f"a < {imm}",
        "andi": f"a & {u(i.imm)}", "ori": f"a | {u(i.imm)}",
        "xori": f"a ^ {u(i.imm)}", "lui": u(i.imm << 16),
    }
    if n in alu:
        dst, value = rd, alu[n]
    elif n in immediate:
        dst, value = rt, immediate[n]
    elif n in {"mthi", "mtlo"}:
        lines.append(f"c->{'hi' if n == 'mthi' else 'lo'} = a;")
    elif n in {"mult", "multu"}:
        expr = "(uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b)" if n == "mult" else "(uint64_t)a * b"
        lines.extend([f"uint64_t product = {expr};", "c->lo = (uint32_t)product; c->hi = (uint32_t)(product >> 32);"])
    elif n in {"div", "divu"}:
        lines.append(f"c_div(c, a, b, {int(n == 'div')});")
    elif n in {"lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr"}:
        load = rt
        if n in {"lwl", "lwr"}:
            loaded = f"c_{n}(c, a + {imm}, c_merge_value(c, {rt}))"
        else:
            width = {"lb": 8, "lbu": 8, "lh": 16, "lhu": 16, "lw": 32}[n]
            cast = {"lb": "(uint32_t)(int32_t)(int8_t)", "lh": "(uint32_t)(int32_t)(int16_t)"}.get(n, "")
            loaded = f"{cast}mem_read{width}(c, a + {imm})"
    elif n in {"sb", "sh", "sw", "swl", "swr"}:
        if n in {"swl", "swr"}:
            lines.append(f"c_{n}(c, a + {imm}, b);")
        else:
            width = {"sb": 8, "sh": 16, "sw": 32}[n]
            lines.append(f"mem_write{width}(c, a + {imm}, b);")
    elif n in {"mfc0", "cfc0"}:
        load, loaded = rt, f"c->cop0[{rd}]"
    elif n in {"mtc0", "ctc0"}:
        lines.append("c_set_sr(c, b);" if rd == 12 else f"c->cop0[{rd}] = b;")
    elif n == "rfe":
        lines.append("c_set_sr(c, (c->cop0[12] & ~15u) | ((c->cop0[12] >> 2) & 15));")
    elif n in {"mfc2", "cfc2"}:
        load, loaded = rt, gte_read_expression(rd, n == "cfc2")
    elif n in {"mtc2", "ctc2"}:
        lines.append(gte_write_statement(rd, "b", n == "ctc2"))
    elif n == "lwc2":
        lines.append(f"gte_write(c, {rt}, mem_read32(c, a + {imm}), 0);")
    elif n == "swc2":
        lines.append(f"mem_write32(c, a + {imm}, gte_read(c, {rt}, 0));")
    elif n == "gte":
        lines.append(gte_command(i.word))
    elif n == "syscall":
        return lines, 0, "0", 0, "0"
    else:
        code = 9 if n == "break" else 11 if n == "cop_unusable" else 10
        lines.append(f"cpu_exception(c, {code}, 0);")
    return lines, dst, value, load, loaded


def operation(i):
    lines, dst, value, load, loaded = effects(i)
    lines.insert(0, f"c->pc = {u(i.pc)}; c_count_recompiled(c, {u(i.pc)}, 1);")
    if i.name == "syscall":
        return lines + ["c_retire(c, 0, 0, 0, 0); bios_syscall(c);", "if (c->halted) return;"]
    # Evaluate side-effecting memory/overflow expressions before retirement.
    lines.extend([f"uint32_t value = {value};", f"uint32_t loaded = {loaded};",
                  f"c_retire(c, {dst}, value, {load}, loaded);", "if (c->halted) return;"])
    return lines


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def fnv64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def code_spans(function):
    spans = []
    for pc in sorted(function.instructions.keys() | function.table_words):
        if spans and spans[-1][0] + spans[-1][1] == pc:
            spans[-1][1] += 4
        else:
            spans.append([pc, 4])
    return spans


def emit(image, functions, output, mode="fast", overlays=(), host_mode="fast", timing="exact", register_cache=None):
    """Emit isolated overlay translation units; only main entries are shared symbols."""
    import hashlib
    import re
    from .fast import emit_fast_function, event_gap, event_windows
    if mode not in {"trace", "fast"}:
        raise ValueError(f"unknown emission mode: {mode}")
    if host_mode not in {"precise", "fast"}:
        raise ValueError(f"unknown host mode: {host_mode}")
    if timing not in {"exact", "block", "loose"}:
        raise ValueError(f"unknown timing policy: {timing}")
    if timing != "exact" and mode == "trace":
        raise ValueError("experimental timing requires fast emission")
    cache_selection = None
    if timing != 'exact' or register_cache is not None:
        from .locals import lower, qualify, read_selection
        cache_selection = (register_cache if register_cache is not None else
                           read_selection(Path(__file__).resolve().parents[1] / 'games/ff8-usa/play-register-cache.json'))
    forward_regions = set(cache_selection.get("fuse_forward", [])) if cache_selection else set()
    used_forward_regions = set()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    def write(name, text):
        path = output / name
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    sets = [("", image, functions, None)] + [
        (f"ov{index}_", img, funcs, index) for index, img, funcs in overlays]
    owners, unique = {}, {}
    for prefix, img, funcs, _ in sets:
        for pc, f in funcs.items():
            spans = code_spans(f)
            key = (pc, tuple((start, img.data[start-img.base:start-img.base+n]) for start, n in spans))
            owners[prefix, pc] = unique.setdefault(key, prefix)
    def symbol(prefix, pc, kind='f'):
        return f'{owners[prefix, pc]}{kind}_{pc:08x}'
    declarations, tables, sources, manifests, units, profiles = [], [], [], [], [], []
    for prefix, img, funcs, index in sets:
        symbols = {pc: symbol('', pc) for pc in functions}
        symbols.update({pc: symbol(prefix, pc) for pc in funcs})
        available = dict(functions) if prefix else {}
        available.update(funcs)
        items = [f for pc, f in funcs.items() if owners[prefix, pc] == prefix]
        table_name = f"{prefix}entries" if prefix else "generated_entries"
        for f in items:
            pc = f.entry
            declarations += [f"void {symbols[pc]}(cpu_t *c);",
                             f"extern const code_region_t {prefix}code_{pc:08x};"]
        for start in range(0, len(items), 24):
            name = f"overlay_{index}_{start // 24:03d}.c" if prefix else f"functions_{start // 24:03d}.c"
            sources.append(name)
            units.append({'source': name, 'set': index, 'functions': len(items[start:start + 24]),
                          'instructions': len({pc for f in items[start:start + 24] for pc in f.instructions})})
            bodies = []
            for f in items[start:start + 24]:
                if timing != "exact":
                    from .timing import TimingEmitter
                    hot = bool(cache_selection and symbols[f.entry] in cache_selection['functions'])
                    fused = symbols[f.entry] in forward_regions
                    if fused:
                        used_forward_regions.add(symbols[f.entry])
                    body = TimingEmitter(f, available, loose=timing == "loose",
                                         inline_blocks=hot or len(f.instructions) <= 32,
                                         fuse_forward=fused).emit()
                    if cache_selection and symbols[f.entry] in cache_selection['functions'] and qualify(f):
                        cached, _ = lower(body, cache_selection['max_registers'])
                        body = '#ifdef FF8_PLAY_REG_CACHE\n' + cached + '#else\n' + body + '#endif\n'
                else:
                    body = emit_fast_function(f, available, trace=mode == "trace",
                                          host_precise=host_mode == "precise",
                                          poll_forward=index is None or getattr(img, 'metadata', {}).get('poll_forward', index % 32 in (0, 1)),
                                          direct_memory=index is None or (getattr(img, 'metadata', {}).get('poll_forward', index % 32 in (0, 1)) or
                                                                         getattr(img, 'metadata', {}).get('name', '') == 'movie' or
                                                                         (not hasattr(img, 'metadata') and index % 32 == 27)))
                body = re.sub(r"\bf_([0-9a-f]{8})\b", lambda m: symbols[int(m[1], 16)], body)
                body = body.replace(f"code_{f.entry:08x}", f"{prefix}code_{f.entry:08x}")
                bodies.append(body)
            body = "\n".join(bodies)
            callees = sorted(set(re.findall(r"\b((?:ov\d+_)?f_[0-9a-f]{8})\(", body)))
            regions = sorted(set(re.findall(r"\b((?:ov\d+_)?code_[0-9a-f]{8})\b", body)))
            local = [f"void {symbol}(cpu_t *c);" for symbol in callees]
            local += [f"extern const code_region_t {symbol};" for symbol in regions]
            poll_header = '#include "poll.h"\n' if 'c_poll_skip(' in body else ''
            write(name, '#include "generated.h"\n' + poll_header + "\n".join(local) + "\n" + body)
        for f in items:
            spans = code_spans(f)
            fingerprint = fnv64(b"".join(img.data[pc-img.base:pc-img.base+n] for pc, n in spans))
            name = f"{prefix}code_{f.entry:08x}"
            windows = event_windows(f)
            tables += [f"static const code_span_t {name}_spans[] = {{" +
                       ",".join(f"{{{u(pc)}, {n}}}" for pc, n in spans) + "};",
                       f"static const uint8_t {name}_windows[] = {{" +
                       ",".join(str(min(windows.get(pc, 0), 255)) for start, n in spans for pc in range(start, start + n, 4)) + "};",
                       f"const code_region_t {name} = {{{name}_spans, {len(spans)}, UINT64_C({fingerprint}), {event_gap(f)}, {name}_windows}};"]
        tables += [f"const entry_t {table_name}[] = {{"]
        from .fast import Emitter
        def entry_map(compact):
            result = {pc: pc for pc in funcs}
            for pc, f in funcs.items():
                for resume in Emitter(f, available, compact=compact).entries:
                    result.setdefault(resume, pc)
            return result
        dispatch_entries = entry_map(False)
        compact_entries = entry_map(True)
        compact_if = '#if defined(FF8_COMPACT)'
        def variant(compact, host):
            return [compact_if, *compact, '#else', *host, '#endif'] if compact != host else host
        def entries(mapping):
            return [f"{{{u(pc)}, {symbols[owner]}}}," for pc, owner in sorted(mapping.items())] or ['{0, NULL},']
        tables += variant(entries(compact_entries), entries(dispatch_entries))
        tables.append("};")
        region_name = f"{prefix}regions" if prefix else "generated_regions"
        tables.append(f"const code_region_t *const {region_name}[] = {{")
        def regions(mapping):
            return [f"&{symbol(prefix, owner, 'code')}," for _, owner in sorted(mapping.items())] or ['NULL,']
        tables += variant(regions(compact_entries), regions(dispatch_entries))
        tables.append("};")
        manifests.append({"index": index, "base": hex(img.base), "size": len(img.data),
                          "sha1": hashlib.sha1(img.data).hexdigest(), "functions": len(funcs),
                          "instructions": len({pc for f in funcs.values() for pc in f.instructions}),
                          "emitted_functions": len(items),
                          "emitted_instructions": len({pc for f in items for pc in f.instructions}),
                          "disc": index // 32 + 1 if index is not None else None,
                          "dependencies": sorted({owners[prefix, pc] for pc in funcs if owners[prefix, pc] != prefix}),
                          "dispatch_entries": len(dispatch_entries)})
        manifests[-1]['compact_dispatch_entries'] = len(compact_entries)
        manifests[-1].update(getattr(img, 'metadata', {}))
        if index is not None:
            words = sorted({pc for f in funcs.values() for pc in f.instructions})
            samples = [words[i * (len(words) - 1) // min(15, len(words) - 1)] for i in range(min(16, len(words)))] if len(words) > 1 else words
            profiles.append(f"{index} {img.base:08x} {len(img.data)} {len(samples)} " + " ".join(f"{pc:08x} {img.word(pc):08x}" for pc in samples))
    tables += ["const overlay_t generated_overlays[] = {"]
    for prefix, img, funcs, index in sets[1:]:
        digest = ",".join(str(b) for b in hashlib.sha1(img.data).digest())
        tables.append(f"{{{index}, {u(img.base)}, {len(img.data)}, {{{digest}}}, {prefix}entries, sizeof({prefix}entries)/sizeof(entry_t), {prefix}regions}},")
    if not overlays: tables.append("{0, 0, 0, {0}, NULL, 0, NULL},")
    tables += ["};", f"const size_t generated_overlay_count = {len(overlays)};"]
    write("generated.h",
        '#ifndef FF8_GENERATED_H\n#define FF8_GENERATED_H\n#define GENERATED_HAS_OVERLAYS 1\n' +
        ('#define FF8_COMPACT 1\n#include "timing.h"\n' if timing != "exact" else '') +
        '#include "cpu.h"\n#include "gte.h"\n' +
        "\nextern const entry_t generated_entries[];\nextern const size_t generated_count;\n"
        "extern const code_region_t *const generated_regions[];\nextern const overlay_t generated_overlays[];\nextern const size_t generated_overlay_count;\n"
        "extern const uint32_t generated_hash, generated_base, generated_size;\n#endif\n")
    write("dispatch_table.c",
        '#include "generated.h"\n' + "\n".join(declarations + tables) +
        f"\n{compact_if}\nconst size_t generated_count = {manifests[0]['compact_dispatch_entries']};\n#else\n"
        f"const size_t generated_count = {manifests[0]['dispatch_entries']};\n#endif\n"
        f"const uint32_t generated_hash = {u(fnv1a(image.data))};\n"
        f"const uint32_t generated_base = {u(image.base)}, generated_size = {len(image.data)};\n" +
        f"const unsigned generated_host_precise = {int(host_mode == 'precise' and mode != 'trace' and timing == 'exact')};\n" +
        (f"const uint32_t generated_timing_event_mask = {255 if timing == 'loose' else 0};\n" if timing != 'exact' else ''))
    manifest = {
        "mode": mode, "host_mode": host_mode, "entry": hex(image.entry), "base": hex(image.base), "size": len(image.data),
        "fnv1a": hex(fnv1a(image.data)), "functions": len(functions),
        "instructions": manifests[0]["instructions"], "overlays": manifests[1:],
        "translation_units": units,
        "jump_tables": {hex(pc): [hex(t) for t in targets] for f in functions.values() for pc, targets in f.tables.items()},
    }
    if timing != "exact":
        manifest['timing'] = timing
        manifest['timing_entry_points'] = [hex(pc) for pc in image.timing_entry_points if pc in functions]
        if used_forward_regions:
            manifest['forward_regions'] = sorted(used_forward_regions)
    write("manifest.json", json.dumps(manifest, indent=2) + "\n")
    write("profile-catalog.txt", "\n".join(profiles) + "\n")
    write("sources.cmake", "set(GENERATED_SOURCES\n" +
        "\n".join(f'  "${{CMAKE_CURRENT_LIST_DIR}}/{name}"' for name in sources + ["dispatch_table.c"]) + "\n)\n")
    for pattern in ("functions_*.c", "overlay_*.c"):
        for old in output.glob(pattern):
            if old.name not in sources:
                old.unlink()
    return manifest
