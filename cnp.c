// Build:
//
//   Generate snippets.c:
//   > python clang_rip.py
//
//   Build this example:
//   > clang -Wall -Wextra -Werror -Oz -g cnp.c -o cnp.exe
//
//   (or m.bat to do both)
//
// This example calculates:
//
//   a = (b + c + f * g) * (d + 3)
//
// using a version of copy-and-patch jitting.
//
// Additionally, some pondering on smaller representations of Token and
// Ast (nodes) that are array-packed and don't do allocation or a lot of
// pointer chasing. It doesn't actually lex or parse, or even use the
// parse tree to dispatch to the jit generation, just for expository
// purposes.

#define _CRT_SECURE_NO_WARNINGS 1

#include "snippets.c"

#include <windows.h>
#undef ERROR
#undef CONST
#undef MAX
#undef MIN
#undef min
#undef max

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define countof(a) (sizeof(a) / sizeof(a[0]))
#define countofi(a) ((int)(sizeof(a) / sizeof(a[0])))
#define max(x, y) ((x) >= (y) ? (x) : (y))

typedef uint64_t Token;
// 8 for kind
// 56 for name/const value (inline! i.e. max ident length 7 bytes)
// (real-life might be something like 24 bit index into intern'd string
// table, and 32 bit index into source code for error reporting.)
#define TOKEN_KINDS \
  X(INVALID)        \
  X(EQ)             \
  X(IDENT)          \
  X(CONST)          \
  X(PLUS)           \
  X(TIMES)          \
  X(LPAREN)         \
  X(RPAREN)         \
  X(EOF)
typedef enum TokenKind {
#define X(x) TK_##x,
  TOKEN_KINDS
#undef X
      TK_Count
} TokenKind;
const char* token_names[] = {
#define X(x) #x,
    TOKEN_KINDS
#undef X
};
#define VAR(name)                          \
  (Token) {                                \
    (((uintptr_t)name[0]) << 8) | TK_IDENT \
  }
#define TOK(t) ((Token){TK_##t})
#define CONST(v) ((Token){v << 8 | TK_CONST})

typedef uint32_t Ast;
// 8 for kind
// 24 for disp 1 (assumed negative)
// disp 2 assumed to be immediately preceding the instruction due to
// being in post-order.
//
// or
//
// 8 for kind
// 24 for index into tokens

#define AST_KINDS \
  X(INVALID)      \
  X(ASSIGN)       \
  X(ADD)          \
  X(MUL)          \
  X(NAME)         \
  X(CONST)

typedef enum AstKind {
#define X(x) AST_##x,
  AST_KINDS
#undef X
      AST_Count
} AstKind;

const char* ast_names[] = {
#define X(x) #x,
    AST_KINDS
#undef X
};

#define UNARYOP(k, val) ((((uint32_t)(val & 0xffffff)) << 8) | (((uint32_t)(AST_##k))))
#define UNARYOP_LVAL(k, val) \
  ((((uint32_t)(val & 0xffffff)) << 8) | (0x80) | (((uint32_t)(AST_##k))))
#define BINOP(k, lhs_displ) ((((uint32_t)(lhs_displ & 0xfff)) << 20) | (((uint32_t)(AST_##k))))

int main() {
  // char* code = "a = (b + c + f * g) * (d + 3)";

  // -------------------------------------------------------------------------
  // "lex" into |Token|s resulting in |tokens|.
  // -------------------------------------------------------------------------

  Token tokens[] = {
      VAR("a"),     // 0
      TOK(EQ),      // 1
      TOK(LPAREN),  // 2
      VAR("b"),     // 3
      TOK(PLUS),    // 4
      VAR("c"),     // 5
      TOK(PLUS),    // 6
      VAR("f"),     // 7
      TOK(TIMES),   // 8
      VAR("g"),     // 9
      TOK(RPAREN),  // 10
      TOK(TIMES),   // 11
      TOK(LPAREN),  // 12
      VAR("d"),     // 13
      TOK(PLUS),    // 14
      CONST(3),     // 15
      TOK(RPAREN),  // 16
      TOK(EOF),     // 17
  };
#undef VAR
#undef TOK
#undef CONST

  printf("tokens:\n-------\n");
  for (int i = 0; i < countofi(tokens); ++i) {
    TokenKind kind = tokens[i] & 0xff;
    printf("%02d: %s", i, token_names[kind]);
    if (kind == TK_IDENT) {
      printf(" '%c'\n", (int)(tokens[i] >> 8));
    } else if (kind == TK_CONST) {
      printf(" %d\n", (int)(tokens[i] >> 8));
    } else {
      printf("\n");
    }
  }

  // -------------------------------------------------------------------------
  // "parse" into |Ast|s resulting in |nodes|.
  // -------------------------------------------------------------------------
  //           =
  //          / \
  //         a   *
  //            / \
  //           /   \
  //          +     +
  //         / \   / \
  //        /   \ d   3
  //       +     *
  //      / \   / \
  //     b   c f   g
  //
  // The tree is stored built as if it was the result of a post-order
  // left-to-right walk, so only one offset of binary operators is
  // needed, the other one is implicitly the entry immediately preceding
  // it.

  Ast nodes[] = {
      UNARYOP_LVAL(NAME, 0),  // a
      UNARYOP(NAME, 3),       // b
      UNARYOP(NAME, 5),       // c
      BINOP(ADD, 2),          // +
      UNARYOP(NAME, 7),       // f
      UNARYOP(NAME, 9),       // g
      BINOP(MUL, 2),          // *
      BINOP(ADD, 4),          // +
      UNARYOP(NAME, 13),      // d
      UNARYOP(CONST, 15),     // 3
      BINOP(ADD, 2),          // +
      BINOP(MUL, 4),          // *
      BINOP(ASSIGN, 12),      // =
  };

  printf("\nast:\n----\n");
  for (int i = 0; i < countofi(nodes); ++i) {
    AstKind kind = nodes[i] & 0x7f;
    bool lval = (bool)(nodes[i] & 0x80);
    printf("%02d: %s%s", i, ast_names[kind], lval ? " (lval)" : "");
    if (kind == AST_NAME) {
      printf(" '%c'\n", (int)(tokens[(int)nodes[i] >> 8] >> 8));
    } else if (kind == AST_CONST) {
      printf(" %d\n", (int)(tokens[(int)nodes[i] >> 8] >> 8));
    } else {
      printf("\n");
    }
  }

  // -------------------------------------------------------------------------
  // The ast is stored in postorder traversal order. Walk in post-order
  // (children before parents) assuming that we'll have to load all
  // names and values into registers before doing anything with them,
  // and always just do left before right. I tried to follow the code
  // that's supposed to go with https://arxiv.org/pdf/2011.13127 to
  // understand how their register allocation works, but I found the C++
  // a bit too convoluted and repetitive to get much out of. I think
  // it's near here:
  // https://github.com/sillycross/PochiVM/blob/master/pochivm/arith_expr_fastinterp.cpp
  // and the opaque items are managed during an abstract interpretation
  // by FIStackFrameManager.
  //
  // Here's what I'm imagining when walking the ast. Maybe it works out
  // to be the same thing. Simply maintain a virtual stack, and make
  // sure to tell the generated code to pass all those values, and keep
  // them in the same order, so there's no need to shuffle.
  //
  // To me, it seems like register allocation isn't even necessary, as
  // clang will implicitly be doing spills to the stack by maintaining
  // the variables as they flow through the continuations. This may be a
  // nice simplification available since the original paper was
  // published because clang may have previously disallowed more than
  // some fixed small N when calling a ghccc function, whereas now, it
  // appears to support an arbitrary number and deals with them on the
  // stack (at least when I tested up to about 25 arguments).
  //
  // The snippets also only support uintptr_t and int (i.e. only integer
  // registers). It should just be a matter of generating a lot more
  // possibilities and maintaining

  // Create two 64k blobs of memory to hold the code we're about to
  // generate, and to hold the named stack variables.
  unsigned char* locals_and_code =
      VirtualAlloc(NULL, 128 << 10, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  unsigned char* code_seg = locals_and_code;
  int* locals = (int*)(locals_and_code + (64 << 10));
  unsigned char* code_p = code_seg;

#define BYTE_OFFSET_OF_LOCAL(name) ((uintptr_t) & locals[(name[0] - 'a')])
  // "a" is at locals_stack[0], etc.
  *(int*)BYTE_OFFSET_OF_LOCAL("a") = 0x1111;  // uninitialized
  *(int*)BYTE_OFFSET_OF_LOCAL("b") = 2;
  *(int*)BYTE_OFFSET_OF_LOCAL("c") = 3;
  *(int*)BYTE_OFFSET_OF_LOCAL("d") = 4;
  *(int*)BYTE_OFFSET_OF_LOCAL("e") = 0;  // unused
  *(int*)BYTE_OFFSET_OF_LOCAL("f") = 6;
  *(int*)BYTE_OFFSET_OF_LOCAL("g") = 7;

  // So, the resulting calculation should be doing:
  // a = ((b+c +  (f*g) *  (d+3)
  // a = ((2+3) + (6*7)) * (4+3)
  // => 329

  // Now, stitch together the snippets. At node, call function. The _0_,
  // _1_, etc. suffix on the function name is "how many values are
  // currently on the virtual stack", which tells the generated code
  // what needs to be preserved. The _fallthrough suffix just means that
  // we don't want to provide a special condition, the next continuation
  // will be placed directly after the current one, so execution can
  // fall off the bottom (and doesn't need to jump anywhere). Because
  // we're only ever supplying one continuation in a simple expression
  // evaluation (i.e. there's no conditional jumps requiring two
  // possible different target locations), we can always use
  // _fallthrough in this example.
  //
  // This is pretty much the bare minimum as far as snippets that could
  // be pre-generated, and it ends up looking just like a bytecode JIT
  // (though efficient and good-at-register-allocation!)
  //
  // In real use, I'm thinking there'd be higher level nodes that do
  // some sort of "match longest" on the Nodes, so e.g. instead of
  // simply [load("x"), load("y"), add()], it could be a snippet that
  // matches 3 nodes, so just uses [add("x", "y")].
  //
  // And once you get pattern matching, you could arbitrarily enlarge
  // the patterns that match various shapes of code. This would tradeoff
  // compiler build time and compiler binary size (due to pregenerating
  // all the snippets) against the possibility of generating code ~as
  // good as clang -O3 if it matches a prebuilt snippet.

  code_p = load_addr_0_fallthrough(code_p, BYTE_OFFSET_OF_LOCAL("a"));
  // vstack now [&a                ]  0 NAME 'a'

  code_p = load_1_fallthrough(code_p, BYTE_OFFSET_OF_LOCAL("b"));
  // vstack now [b &a              ]  1 NAME 'b'

  code_p = load_2_fallthrough(code_p, BYTE_OFFSET_OF_LOCAL("c"));
  // vstack now [c b &a            ]  2 NAME 'c'

  code_p = add_1_fallthrough(code_p);
  // vstack now [r0 &a             ]  3 ADD

  code_p = load_2_fallthrough(code_p, BYTE_OFFSET_OF_LOCAL("f"));
  // vstack now [f r0 &a           ]  4 NAME 'f'

  code_p = load_3_fallthrough(code_p, BYTE_OFFSET_OF_LOCAL("g"));
  // vstack now [g f r0 &a         ]  5 NAME 'g'

  code_p = mul_2_fallthrough(code_p);
  // vstack now [r1 r0 &a          ]  6 MUL

  code_p = add_1_fallthrough(code_p);
  // vstack now [r2 &a             ]  7 ADD

  code_p = load_2_fallthrough(code_p, BYTE_OFFSET_OF_LOCAL("d"));
  // vstack now [d r2 &a           ]  8 NAME 'd'

  code_p = const_3_fallthrough(code_p, 3);
  // vstack now [3 d r2 &a         ]  9 CONST 3

  code_p = add_2_fallthrough(code_p);
  // vstack now [r3 r2 &a          ] 10 ADD

  code_p = mul_1_fallthrough(code_p);
  // vstack now [r4 &a             ] 11 MUL

  code_p = assign_indirect_0_fallthrough(code_p);
  // vstack now [                  ] 12 ASSIGN

  // Just for testing so we can `call` to the code we just generated.
  // Normally this would be inside of a large setup that could provide a
  // top-level continuation that would receive the result, or be inside
  // a normal __cdecl function that had set up locals, etc. could do a
  // real `ret` to the caller.
  *code_p++ = 0xc3;  // ret

  printf("\nGenerated %zu bytes of code, executing.\n", code_p - code_seg);

#if 0
  FILE* f = fopen("code.raw", "wb");
  fwrite(code_seg, 1, code_p - code_seg, f);
  fclose(f);
  // `ndisasm -b64 code.raw`
#endif

  DWORD old_protect;
  VirtualProtect(code_seg, 64 << 10, PAGE_EXECUTE_READ, &old_protect);
  VirtualProtect(locals, 64 << 10, PAGE_READWRITE, &old_protect);
  ((void (*)())code_seg)();

  printf("\nFinal value of 'a': %d\n", locals[0]);
}
