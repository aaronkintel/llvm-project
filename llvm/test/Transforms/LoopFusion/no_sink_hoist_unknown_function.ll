; RUN: opt -S -loop-simplify -loop-fusion -debug-only=loop-fusion < %s 2>&1 | FileCheck %s
; REQUIRES: asserts
; CHECK: Could not hoist/sink all instructions

declare void @unknown_func()

define void @sink_preheader(i32 %N) {
; CHECK:pre1:
; CHECK-NEXT:  br label %body1
pre1:
  br label %body1

; CHECK:body1: 
body1:  ; preds = %pre1, %body1
  %i = phi i32 [%i_next, %body1], [0, %pre1]
  %i_next = add i32 1, %i
  %cond = icmp ne i32 %i, %N
  br i1 %cond, label %body1, label %pre2

; CHECK:pre2:
; CHECK-NEXT: call void @unknown_func()
pre2:
  call void @unknown_func()
  br label %body2

; CHECK: body2:
body2:  ; preds = %pre2, %body2
  %i2 = phi i32 [%i_next2, %body2], [0, %pre2]
  %i_next2 = add i32 1, %i2
  %cond2 = icmp ne i32 %i2, %N
  br i1 %cond2, label %body2, label %exit

; CHECK: exit:
exit:
  ret void
}
