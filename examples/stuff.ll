; Target datalayout and triple would go here
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Type definitions
%struct.Vec3 = type { double, double, double }

declare i8* @malloc(i32)
declare void @free(i8*)
declare i32 @printf(i8*, ...)

%struct.Player = type {
  double,
  %struct.Vec3,
  %struct.Vec3
}

define void @init() {
entry:
  %v0 = alloca i32
  store i32 42, i32* %v0
  ret void
}

define void @move() {
entry:
  %v1 = add i32 0, 0
  ret void
}

define double @double(double %arg0) {
entry:
  %v2 = fmul double %arg0, 2.0
  ret double 0.0
}

