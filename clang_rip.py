'''
TODO:
- probably don't pass $stack everywhere since we don't need it
'''

import subprocess
import sys

# TODO: fixme
CLANG_PATH = "C:\\Program Files\\LLVM\\bin\\clang.exe"
OBJDUMP_PATH = "C:\\Program Files\\LLVM\\bin\\llvm-objdump.exe"
CLANG_FORMAT_PATH = "C:\\Program Files\\LLVM\\bin\\clang-format.exe"


def clang_format_for_patch_header(src):
    src = src.replace("__vectorcall ", "").replace("__vectorcall", "")
    pop = subprocess.Popen(
        [CLANG_FORMAT_PATH], stdin=subprocess.PIPE, stdout=subprocess.PIPE
    )
    return pop.communicate(src.encode("utf-8"))[0].decode("utf-8")


def process_obj_file_to_patch_func(
    obj_file, snip_name, snippets_file_handle, src, consts, continuations
):
    """
    As a quick first draft, parse the output of llvm-objdump. It wouldn't be very hard
    and probably more reliable to just read the .obj file directly.

    Either way, with the code bits and relocs in hand, write out a C
    function to the header that will 'assemble' into our target buffer.
    """
    result = subprocess.run(
        [
            OBJDUMP_PATH,
            "--section=.text",
            "--reloc",
            obj_file,
        ],
        stdout=subprocess.PIPE,
    )
    relocs = result.stdout.decode("utf-8").splitlines()
    relocs_no_header = relocs[4:]
    if not (
        relocs_no_header[0].startswith("OFFSET")
        and relocs_no_header[0].endswith("VALUE")
    ):
        print("llvm-objdump reloc parsing failed (no OFFSET header?)")
        sys.exit(1)
    relocs_body = relocs_no_header[1:]
    all_relocs = {}
    for line in relocs_body:
        offset, type, name = line.split()
        offset = int(offset, 16)
        all_relocs[offset] = (type, name)
    # print(all_relocs)

    result = subprocess.run(
        [
            OBJDUMP_PATH,
            "--section=.text",
            "--disassemble",
            "--disassemble-zeroes",
            "--no-addresses",
            "--x86-asm-syntax=intel",
            obj_file,
        ],
        stdout=subprocess.PIPE,
    )
    dis = result.stdout.decode("utf-8").splitlines()
    dis_no_header = dis[5:]
    if not dis_no_header[0].startswith("<") or not dis_no_header[0].endswith(">:"):
        print("llvm-objdump dis parsing failed (no func name?)")
        sys.exit(1)
    dis_body = dis_no_header[1:]
    all_bytes = []
    for line in dis_body:
        bytes, _, instrs = line.partition("\t")
        for b in bytes.split():
            all_bytes.append(int(b, 16))
    # print(all_bytes)

    def could_fallthrough():
        if len(continuations) != 1:
            return False
        if all_bytes[-5] != 0xe9:
            return False
        at = len(all_bytes) - 4
        r = all_relocs.get(at)
        if not r:
            return False
        if r[0] == 'IMAGE_REL_AMD64_REL32' and r[1] == '$CONT0':
            return True
        return False

    args = []
    for i, c in enumerate(consts):
        args.append(f"uintptr_t $X{i}")
    args_str_no_continuations = ", ".join(args)
    for i, c in enumerate(continuations):
        args.append(f"void* $CONT{i}")
    args_str = ", ".join(args)

    sf = snippets_file_handle

    def gen_snippet(fallthrough):
        ft = "_fallthrough" if fallthrough else ""
        arg = args_str_no_continuations if fallthrough else args_str
        if arg:
            arg = f", {arg}"
        sf.write(
            f"static inline unsigned char* {snip_name}{ft}(unsigned char* __restrict p{arg}) {{\n"
        )
        i = 0
        while i < len(all_bytes):
            b = all_bytes[i]
            r = all_relocs.get(i)
            if r:
                if r[0] == "IMAGE_REL_AMD64_ADDR64":
                    sf.write(f"  *(uintptr_t*)p = {r[1]};\n")
                    sf.write("  p += 8; /* %s ADDR64 */\n" % r[1])
                    i += 8
                elif r[0] == "IMAGE_REL_AMD64_REL32":
                    sf.write(
                        f"  *(int32_t*)p = (intptr_t){r[1]} - (intptr_t)p - 4;\n"
                    )
                    sf.write("  p += 4; /* %s REL32 */\n" % r[1])
                    i += 4
            else:
                if fallthrough and i == len(all_bytes) - 5:
                    break
                sf.write("  *p++ = 0x%02x;\n" % b)
                i += 1
        sf.write("  return p;\n")
        sf.write(f"}}\n\n")

    sf.write("#if 0\n\n")
    sf.write(clang_format_for_patch_header(src))
    sf.write("\n#endif\n\n")
    gen_snippet(False)
    if could_fallthrough():
        gen_snippet(True)


def munge_ll_file(infile, outfile):
    """
    It doesn't seem to be possible to tell clang to make the calling
    convention of a function the GHC one, so we instead tag both
    definitions and casts before continuation calls as __vectorcall, and
    then manually search and replace vectorcall with ghccc. Additionally,
    change tail calls to musttail to make sure we fail if the continuation
    wouldn't be compiled to a jmp.
    """
    with open(infile, "r") as f:
        contents = f.read()
    contents = contents.replace("tail call x86_vectorcallcc", "musttail call ghccc")
    contents = contents.replace("x86_vectorcallcc", "ghccc")
    with open(outfile, "w", newline="\n") as f:
        f.write(contents)


def toolchain_c_to_obj(base_name, src, consts, continuations, snippets_file_handle, model):
    c_file = base_name + ".c"
    first_ll_file = base_name + ".initial.ll"
    munged_ll_file = base_name + ".ghc.ll"
    obj_file = base_name + ".obj"

    with open(c_file, "w", newline="\n") as f:
        f.write(
            """\
#include <stdint.h>
extern uintptr_t $X0;
extern uintptr_t $X1;
extern uintptr_t $X2;
extern uintptr_t $X3;
#define $X0 ((uintptr_t)&$X0)
#define $X1 ((uintptr_t)&$X1)
#define $X2 ((uintptr_t)&$X2)
#define $X3 ((uintptr_t)&$X3)
extern uintptr_t $CONT0;
extern uintptr_t $CONT1;
extern uintptr_t $CONT2;
extern uintptr_t $CONT3;
#define $CONT0 ((uintptr_t)&$CONT0)
#define $CONT1 ((uintptr_t)&$CONT1)
#define $CONT2 ((uintptr_t)&$CONT2)
#define $CONT3 ((uintptr_t)&$CONT3)
"""
        )
        f.write(src)
        f.write("\n")

    subprocess.check_call(
        [CLANG_PATH, "-std=c2x", "-emit-llvm", c_file, "-O3", "-S", "-o", first_ll_file]
    )
    munge_ll_file(first_ll_file, munged_ll_file)

    # -mcmodel=medium is a useful hack to help with constants in the
    # snippets. I think medium is supposed to mean that most code and
    # data are in the low 2G range (so normally rip-relative addressing
    # would be used) and then "large data" is accessed with movabs full
    # 8 byte addresses. But, extern'd variables seem to automatically
    # count as "far away" if they're being used as data (rather than
    # function pointers). The upshot of this is that our "X" variables
    # get to write a full 8 bytes in to rax in the snippet bytestream,
    # so we can load a full size constant, but the CONTs that are used
    # to jump to the next snippet are more efficient rip-relative REL32
    # relocations still like they normally would be in the default small
    # model.
    subprocess.check_call(
        [
            CLANG_PATH,
            "-std=c2x",
            munged_ll_file,
            f"-mcmodel={model}",
            "-O3",
            "-c",
            "-o",
            obj_file,
        ]
    )
    # subprocess.check_call(["dumpbin", "/disasm", obj_file])
    process_obj_file_to_patch_func(
        obj_file, base_name, snippets_file_handle, src, consts, continuations
    )


class CToObj:
    def __init__(self, base_name, saved_int_regs, snippets_file, model='small'):
        self.base_name = base_name
        self.saved_int_regs = saved_int_regs
        self.code = ""
        self.consts_used = []
        self.continuations_used = []
        self.snippets_file = snippets_file
        self.model = model

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        toolchain_c_to_obj(
            f"{self.base_name}_{self.saved_int_regs}",
            self.code,
            self.consts_used,
            self.continuations_used,
            self.snippets_file,
            self.model,
        )

    def build_decl(self, args):
        result = (
            f"__vectorcall void {self.base_name}_{self.saved_int_regs}(uintptr_t $stack"
        )
        for i in range(self.saved_int_regs):
            result += f", uintptr_t $r{i}"
        for i in args:
            result += f", {i}"
        result += ")"
        self.emit(result)

    def build_const(self, const_index):
        self.emit(f"$X{const_index}")
        self.consts_used.append(const_index)

    def build_continuation(self, cont_index, to_add):
        # 'return' is necessary because it's converted to a [[musttail]],
        # which clang specifies has to be on a return even though it's void.
        num_args = self.saved_int_regs + len(to_add)
        result = "return ((void (*__vectorcall)(uintptr_t"
        for i in range(self.saved_int_regs):
            result += ", uintptr_t"
        for i in to_add:  # TODO: assuming all 'int' right now
            result += ", int"
        result += f"))$CONT{cont_index})($stack"
        for i in range(self.saved_int_regs):
            result += f", $r{i}"
        for i in to_add:
            result += f", {i}"
        result += ");"
        self.emit(result)
        self.continuations_used.append(cont_index)

    def emit(self, text):
        self.code += text


def main():
    with open("snippets.c", "w", newline="\n") as sf:
        sf.write("#include <stdint.h>\n\n")

        for ir in range(4):
            with CToObj("load_addr", ir, sf) as c:
                c.build_decl([])
                c.emit("{ uintptr_t v = (")
                c.build_const(0)
                c.emit(");")
                c.build_continuation(0, ["v"])
                c.emit("}")

        for ir in range(4):
            with CToObj("load", ir, sf) as c:
                c.build_decl([])
                c.emit("{ int v = *(int*)(")
                c.build_const(0)
                c.emit(");")
                c.build_continuation(0, ["v"])
                c.emit("}")

        for ir in range(4):
            with CToObj("const", ir, sf, model='medium') as c:
                c.build_decl([])
                c.emit("{ int v = (")
                c.build_const(0)
                c.emit(");")
                c.build_continuation(0, ["v"])
                c.emit("}")

        for ir in range(4):
            with CToObj("add", ir, sf) as c:
                c.build_decl(["int a", "int b"])
                c.emit("{ int v = a + b;")
                c.build_continuation(0, ["v"])
                c.emit("}")

        for ir in range(4):
            with CToObj("assign_indirect", ir, sf) as c:
                c.build_decl(["uintptr_t offset", "int v"])
                c.emit("{ *(int*)(offset) = v;")
                c.build_continuation(0, [])
                c.emit("}")

        for ir in range(4):
            with CToObj("mul", ir, sf) as c:
                c.build_decl(["int a", "int b"])
                c.emit("{ int v = a * b;")
                c.build_continuation(0, ["v"])
                c.emit("}")

        for ir in range(4):
            with CToObj("if_then_else", ir, sf) as c:
                c.build_decl(["bool cond"])
                c.emit("{ if (cond) {")
                c.build_continuation(0, [])
                c.emit("} else {")
                c.build_continuation(1, [])
                c.emit("}")
                c.emit("}")


if __name__ == "__main__":
    main()
