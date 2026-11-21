#pragma once
// Tiny expression language used by `when:`, `count:` and `length:`.
//
//   expr    := or
//   or      := and ('||' and)*
//   and     := cmp ('&&' cmp)*
//   cmp     := bitor (('=='|'!='|'<'|'<='|'>'|'>=') bitor)?
//   bitor   := bitxor ('|' bitxor)*
//   bitxor  := bitand ('^' bitand)*
//   bitand  := shift ('&' shift)*
//   shift   := add (('<<'|'>>') add)*
//   add     := mul (('+'|'-') mul)*
//   mul     := unary (('*'|'/'|'%') unary)*
//   unary   := ('!'|'-'|'~') unary | primary
//   primary := INT | IDENT ('.' IDENT)* | '(' expr ')'
//
// INT accepts decimal, 0x hex and 0b binary. Identifiers refer to earlier fields of the same
// message (`count`, `flags.mode`) or enum constants (`Mode.fault`).

#include <string>

#include "diagnostics.hpp"
#include "ir/ir.hpp"

namespace gmpc {

/// Parse an expression; records an error and returns nullptr on failure.
ir::ExprPtr parse_expr(const std::string& text, const SourceLoc& loc, Diagnostics& diag);

/// Human readable rendering (used in diagnostics and generated comments).
std::string expr_to_string(const ir::Expr& e);

} // namespace gmpc
