; A deliberately broad seed: every pathway needs something to act on, so this
; covers integer and float arithmetic, vectors, pointers and GEPs, calls with a
; repeated signature (for `splice`), a loop, and single-use chains (for
; `break-use` and `int-width`).

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

@g = global i32 7
@gp = global ptr null

declare i32 @ext(i32, i32)

define i32 @arith(i32 %a, i32 %b) {
entry:
  %s = add nsw i32 %a, %b
  %t = mul i32 %s, 3
  %u = xor i32 %t, -1
  %v = lshr i32 %u, 2
  %c = icmp slt i32 %v, 100
  %r = select i1 %c, i32 %v, i32 %b
  ret i32 %r
}

; `or`, `zext` and `uitofp` are here so the `flags` pathway can reach the
; disjoint, nneg and samesign flags, which have no other carrier in this file.
define i64 @flagbait(i32 %a, i32 %b) {
  %o = or i32 %a, %b
  %z = zext i32 %o to i64
  %f = uitofp i32 %o to double
  %c = icmp ult i32 %a, %b
  %w = select i1 %c, i64 %z, i64 0
  ret i64 %w
}

define i32 @alias(i32 %a, i32 %b) {
  %d = sub i32 %a, %b
  ret i32 %d
}

define double @fp(double %x, double %y) {
  %a = fadd double %x, %y
  %b = fmul fast double %a, 2.500000e+00
  %c = fdiv double %b, %x
  %d = fcmp ogt double %c, 0.000000e+00
  %e = select i1 %d, double %c, double %y
  ret double %e
}

define <4 x i32> @vec(<4 x i32> %v, <4 x i32> %w) {
  %a = and <4 x i32> %v, %w
  %b = shl <4 x i32> %a, <i32 1, i32 2, i32 3, i32 4>
  %c = extractelement <4 x i32> %b, i32 2
  %d = insertelement <4 x i32> %b, i32 %c, i32 0
  ret <4 x i32> %d
}

define i32 @mem(ptr %p, i64 %i) {
  %q = getelementptr inbounds i32, ptr %p, i64 %i
  %l = load i32, ptr %q, align 4
  %z = zext i32 %l to i64
  %t = trunc i64 %z to i16
  %e = sext i16 %t to i32
  store i32 %e, ptr %q, align 4
  ret i32 %e
}

define i32 @calls(i32 %a, i32 %b) {
  %x = call i32 @ext(i32 %a, i32 %b)
  %y = call i32 @arith(i32 %x, i32 %b)
  %z = call i32 @alias(i32 %y, i32 %a)
  ret i32 %z
}

define i32 @loop(i32 %n) {
entry:
  br label %head

head:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %acc = phi i32 [ 1, %entry ], [ %mul, %body ]
  %cmp = icmp slt i32 %i, %n
  br i1 %cmp, label %body, label %exit

body:
  %mul = mul nuw i32 %acc, %i
  %next = add nuw nsw i32 %i, 1
  br label %head

exit:
  ret i32 %acc
}

define i64 @intrin(i64 %x, i64 %y) {
  %a = call i64 @llvm.ctpop.i64(i64 %x)
  %b = call i64 @llvm.umax.i64(i64 %a, i64 %y)
  %c = call i64 @llvm.abs.i64(i64 %b, i1 false)
  ret i64 %c
}

declare i64 @llvm.ctpop.i64(i64)
declare i64 @llvm.umax.i64(i64, i64)
declare i64 @llvm.abs.i64(i64, i1)
