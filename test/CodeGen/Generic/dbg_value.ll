; RUN: llc < %s
; rdar://7759395

%0 = type { i32, i32 }

define void @t(%0*, i32, i32, i32, i32) nounwind {
  tail call void @llvm.dbg.value(metadata %0* %0, i64 0, metadata !0, metadata !MDExpression()), !dbg !MDLocation(scope: !MDSubprogram())
  unreachable
}

declare void @llvm.dbg.value(metadata, i64, metadata, metadata) nounwind readnone

; !0 should conform to the format of DIVariable.
!0 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "a", arg: 0, scope: !MDSubprogram())
